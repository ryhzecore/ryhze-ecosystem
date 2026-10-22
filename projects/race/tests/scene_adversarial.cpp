#include "race/scene/scene.hpp"
#include "test.hpp"
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
using namespace race;
using namespace race::scene;
using namespace std::chrono_literals;
namespace {
PersistentId aid(std::uint64_t n) { return {91, n}; }
Object obj(std::uint64_t n) { return {aid(n), {}, {transform_component()}, {}}; }
Limits small() {
  Limits l;
  l.objects = 4;
  l.components = 8;
  l.template_objects = 4;
  return l;
}
struct Setup {
  TrackedAllocator allocator;
  std::shared_ptr<const Schemas> schemas;
  Setup()
      : allocator(std::move(TrackedAllocator::create({32 * 1024 * 1024})).value()),
        schemas(Schemas::create({{100, 1, "Counter", {{1, "value", std::int64_t{1}}}},
                                 {200, 1, "Link", {{1, "target", Reference{}}}}})
                    .value()) {}
  std::unique_ptr<World> world(Limits l = small(), std::shared_ptr<const Templates> t = {}) {
    auto result = World::create(allocator, schemas, l, std::move(t));
    CHECK(result);
    return std::move(result).value();
  }
};
void checked_view_handles() {
  Setup s;
  auto w = s.world();
  auto other = s.world();
  CHECK(w->apply({{Command::create(obj(1))}}));
  CHECK(other->apply({{Command::create(obj(1))}}));
  auto held = w->read();
  auto foreign = other->read();
  CHECK(held && foreign);
  const auto good = held.value().find(aid(1)).value();
  auto slot = good, generation = good, table = good;
  slot.slot = UINT32_MAX;
  ++generation.generation;
  ++table.table;
  const ObjectHandle invalid[] = {{}, slot, generation, table,
                                 foreign.value().find(aid(1)).value()};
  for (auto h : invalid) {
    CHECK_ERROR(held.value().object(h), ErrorCode::StaleHandle);
    CHECK_ERROR(held.value().value(h, 1, 1), ErrorCode::StaleHandle);
    CHECK_ERROR(held.value().world_transform(h), ErrorCode::StaleHandle);
  }
  held.value().release();
  auto version = w->stats().generation;
  for (auto h : invalid) {
    CHECK_ERROR(w->apply({{Command::set(aid(1), 1, 1, Vec3{4, 0, 0}, h)}}),
                ErrorCode::StaleHandle);
    CHECK(w->stats().generation == version);
  }
  CHECK(w->apply({{Command::destroy(aid(1), good)}}));
  CHECK(w->undo());
  auto restored = w->read();
  CHECK(restored);
  CHECK(restored.value().find(aid(1)).value() != good);
  CHECK_ERROR(restored.value().value(good, 1, 1), ErrorCode::StaleHandle);
  auto current = restored.value().find(aid(1)).value();
  w.reset();
  std::thread reader([view = std::move(restored.value()), current]() mutable {
    CHECK(view.object(current));
    CHECK(view.value(current, 1, 1));
    CHECK(view.world_transform(current));
    view.release();
  });
  reader.join();
}
void limits_and_queries() {
  Setup s;
  {
    auto l = small();
    l.components = 1;
    auto w = s.world(l);
    auto a = obj(1);
    a.components.push_back({100, 1, {}});
    CHECK_ERROR(w->apply({{Command::create(a)}}), ErrorCode::Exhausted);
    CHECK(w->stats().objects == 0);
  }
  {
    auto l = small();
    l.commands_per_transaction = 1;
    auto w = s.world(l);
    CHECK_ERROR(w->apply({{Command::create(obj(1)), Command::create(obj(2))}}),
                ErrorCode::QueueFull);
    CHECK(w->stats().objects == 0);
  }
  {
    auto l = small();
    l.history_groups = 1;
    auto w = s.world(l);
    CHECK(w->apply({{Command::create(obj(1))}}));
    auto generation = w->stats().generation;
    CHECK_ERROR(w->apply({{Command::set(aid(1), 1, 1, Vec3{2, 0, 0})}}), ErrorCode::Exhausted);
    CHECK(w->stats().generation == generation);
    CHECK(w->apply({{Command::set(aid(1), 1, 1, Vec3{2, 0, 0})}, EditMode::Simulation}));
    CHECK(w->stats().history_groups == 1);
    CHECK(w->clear_history());
    CHECK(w->apply({{Command::set(aid(1), 1, 1, Vec3{3, 0, 0})}}));
  }
  {
    auto l = small();
    l.pending_transactions = 1;
    l.views = 1;
    auto w = s.world(l);
    CHECK(w->apply({{Command::create(obj(1))}}));
    auto v = w->read();
    CHECK(v);
    CHECK_ERROR(w->read(), ErrorCode::Exhausted);
    auto bad = w->enqueue({{Command::destroy(aid(99))}});
    CHECK(bad);
    CHECK_ERROR(w->enqueue({{Command::destroy(aid(1))}}), ErrorCode::QueueFull);
    v.value().release();
    CHECK(w->publish_pending());
    CHECK_ERROR(w->enqueue({{Command::destroy(aid(1))}}), ErrorCode::QueueFull);
    auto completion = w->take_completion(bad.value());
    CHECK(completion);
    CHECK_ERROR(completion.value().result, ErrorCode::NotFound);
    auto good = w->enqueue({{Command::destroy(aid(1))}});
    CHECK(good);
    CHECK(w->publish_pending());
    CHECK(w->take_completion(good.value()).value().result);
    CHECK(w->stats().objects == 0 && w->stats().pending_bytes == 0);
  }
  {
    auto w = s.world();
    auto a = obj(1), b = obj(2), c = obj(3);
    a.components.push_back({100, 1, {{1, std::int64_t{11}}}});
    b.components.push_back({100, 1, {{1, std::int64_t{22}}}});
    c.components.push_back({100, 1, {{1, std::int64_t{33}}}});
    CHECK(w->apply({{Command::create(a), Command::create(b), Command::create(c)}}));
    CHECK(w->apply({{Command::destroy(aid(2))}}));
    auto d = obj(4);
    d.components.push_back({100, 1, {{1, std::int64_t{44}}}});
    CHECK(w->apply({{Command::create(d)}}));
    auto view = w->read();
    CHECK(view);
    auto rows = view.value().query(100);
    CHECK(rows && rows.value().size() == 3);
    std::int64_t sum = 0;
    for (auto &row : rows.value()) {
      sum += std::get<std::int64_t>(row.values[0]);
      CHECK(view.value().object(row.object));
    }
    CHECK(sum == 88);
  }
  {
    auto budget = TrackedAllocator::create({1});
    CHECK(budget);
    CHECK_ERROR(World::create(budget.value(), s.schemas, small()), ErrorCode::Exhausted);
    CHECK(budget.value().snapshot().total.live == 0);
  }
}
void section_edges() {
  Setup s;
  auto a = aid(100), b = aid(101);
  {
    auto l = small();
    l.prepared_bytes = 64;
    auto w = s.world(l);
    CHECK(w->declare_section({a, {}}));
    auto p = w->begin_prepare(a);
    CHECK(p);
    std::vector<Object> empty;
    empty.reserve(100);
    CHECK_ERROR(w->finish_prepare(p.value(), std::move(empty)), ErrorCode::Exhausted);
    CHECK(w->stats().prepared_bytes == 0 && w->section(a).value().state == SectionState::Failed);
  }
  {
    auto l = small();
    l.objects = l.template_objects = 2;
    auto w = s.world(l);
    CHECK(w->declare_section({a, {}}));
    CHECK(w->declare_section({b, {}}));
    auto p = w->begin_prepare(a);
    CHECK(p);
    auto first = obj(1), second = obj(2);
    first.section = second.section = a;
    CHECK(w->finish_prepare(p.value(), {first, second}));
    CHECK(w->activate(a));
    CHECK(w->unload(a, 0ms));
    auto q = w->begin_prepare(b);
    CHECK(q);
    CHECK(w->finish_prepare(q.value(), {}));
    CHECK(w->activate(b));
    auto third = obj(3);
    third.section = b;
    auto generation = w->stats().generation;
    CHECK_ERROR(w->apply({{Command::create(third)}}), ErrorCode::Exhausted);
    CHECK(w->stats().generation == generation);
  }
  {
    auto w = s.world();
    CHECK(w->declare_section({a, {}}));
    CHECK(w->declare_section({b, {}}));
    auto p = w->begin_prepare(a);
    CHECK(p);
    CHECK(w->finish_prepare(p.value(), {}));
    CHECK(w->activate(a));
    auto owner = obj(1);
    owner.components.push_back({200, 1, {{1, Reference{b, aid(2)}}}});
    CHECK(w->apply({{Command::create(owner)}}));
    auto target = obj(2);
    target.section = a;
    CHECK_ERROR(w->apply({{Command::create(target)}}), ErrorCode::InvalidDependency);
    auto q = w->begin_prepare(a);
    CHECK(q);
    CHECK_ERROR(w->finish_prepare(q.value(), {target}), ErrorCode::InvalidDependency);
    CHECK(w->stats().objects == 1);
  }
  {
    auto w = s.world();
    CHECK(w->declare_section({a, {}}));
    auto p = w->begin_prepare(a);
    CHECK(p);
    auto one = obj(1);
    one.section = a;
    CHECK(w->finish_prepare(p.value(), {one}));
    CHECK(w->activate(a));
    auto view = w->read();
    CHECK(view);
    std::mutex mutex;
    std::condition_variable cv;
    bool held = false, release = false;
    std::thread reader([v = std::move(view).value(), &mutex, &cv, &held, &release]() mutable {
      std::unique_lock lock(mutex);
      held = true;
      cv.notify_all();
      cv.wait(lock, [&] { return release; });
      CHECK(v.find(aid(1)));
      v.release();
    });
    {
      std::unique_lock lock(mutex);
      cv.wait(lock, [&] { return held; });
    }
    CHECK_ERROR(w->unload(a, 0ms), ErrorCode::Timeout);
    CHECK(w->stats().objects == 1);
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    cv.notify_all();
    CHECK(w->unload(a, std::chrono::milliseconds::max()));
    reader.join();
    CHECK(w->stats().objects == 0);
  }
}
void transform_edges() {
  Setup s;
  auto w = s.world();
  auto parent = obj(1), child = obj(2);
  auto q = axis_angle({0, 1, 0}, .7);
  CHECK(q);
  parent.components[0] = transform_component({{}, {}, {2, 1, 1}});
  child.components[0] = transform_component({{}, q.value(), {1, 1, 1}}, {{}, aid(1)});
  CHECK(w->apply({{Command::create(parent), Command::create(child)}}));
  Affine original;
  {
    auto v = w->read();
    CHECK(v);
    original = v.value().world_transform(v.value().find(aid(2)).value()).value();
    CHECK(near(original, compose(affine({{}, {}, {2, 1, 1}}).value(),
                                 affine({{}, q.value(), {1, 1, 1}}).value())
                             .value()));
  }
  auto generation = w->stats().generation;
  CHECK_ERROR(w->apply({{Command::parent_to(aid(2), {}, ReparentMode::PreserveWorld)}}),
              ErrorCode::Unsupported);
  CHECK(w->stats().generation == generation);
  {
    auto v = w->read();
    CHECK(v);
    CHECK(near(original, v.value().world_transform(v.value().find(aid(2)).value()).value()));
  }
  auto basis = affine({{}, axis_angle({0, 1, 0}, std::acos(-1.) / 2).value(), {1, 1, 1}}).value();
  CHECK(std::abs(basis.m[0]) < 1e-12 && std::abs(basis.m[8] + 1) < 1e-12);
}
void template_edges() {
  Setup s;
  auto l = small();
  TemplateKey one{aid(500), 1}, two{aid(500), 2}, three{aid(500), 3};
  auto root = obj(10);
  auto added = obj(11);
  auto catalog = Templates::create(
      {{one, {root}, {}}, {two, {root}, {}}, {three, {root, added}, {}}}, *s.schemas, l);
  CHECK(catalog);
  auto w = s.world(l, catalog.value());
  std::array<PersistentId, 1> fresh{aid(1)};
  CHECK(w->instantiate(one, aid(600), fresh));
  Command put;
  put.kind = CommandKind::PutComponent;
  put.target = aid(1);
  put.component = {100, 1, {{1, std::int64_t{5}}}};
  CHECK(w->apply({{put, Command::set(aid(1), 100, 1, std::int64_t{88})}}));
  CHECK(w->update_instance(aid(600), two));
  {
    auto v = w->read();
    CHECK(v);
    CHECK(std::get<std::int64_t>(v.value().value(v.value().find(aid(1)).value(), 100, 1).value()) ==
          88);
  }
  CHECK(w->reset_override(aid(1), 100, 0));
  Command remove;
  remove.kind = CommandKind::RemoveComponent;
  remove.target = aid(1);
  remove.type = 1;
  CHECK(w->apply({{Command::set(aid(1), 1, 1, Vec3{9, 0, 0}), remove}}));
  CHECK(w->update_instance(aid(600), one));
  CHECK(w->reset_override(aid(1), 1, 0));
  std::array<PersistentId, 1> bad{aid(11)};
  CHECK_ERROR(w->update_instance(aid(600), three, bad), ErrorCode::Duplicate);
  auto external = obj(12);
  PersistentId reserved_looking{UINT64_MAX, 1};
  external.components.push_back({200, 1, {{1, Reference{{}, reserved_looking}}}});
  auto refs = Templates::create({{{aid(501), 1}, {external}, {}}}, *s.schemas, l);
  CHECK(refs);
  auto world = s.world(l, refs.value());
  CHECK(world->instantiate({aid(501), 1}, aid(601), fresh));
  {
    auto v = world->read();
    CHECK(v);
    CHECK(std::get<Reference>(v.value().value(v.value().find(aid(1)).value(), 200, 1).value())
              .object == reserved_looking);
  }
  CHECK_ERROR(Templates::create({{one, {root}, {{aid(10), two}}}}, *s.schemas, l),
              ErrorCode::InvalidDependency);
  l.template_depth = 1;
  CHECK_ERROR(
      Templates::create(
          {{one, {root}, {{aid(10), two}}}, {two, {root}, {{aid(10), three}}}, {three, {root}, {}}},
          *s.schemas, l),
      ErrorCode::Exhausted);
}
struct Output {
  double x{};
  bool fails{};
  std::atomic<unsigned> called{};
};
Result<std::vector<Command>> output_system(const SystemContext &, void *raw) {
  auto &s = *static_cast<Output *>(raw);
  ++s.called;
  if (s.fails)
    return fail(ErrorCode::PlatformFailure, "injected failure");
  return std::vector<Command>{Command::set(aid(1), 1, 1, Vec3{s.x, 0, 0})};
}
void schedules() {
  Setup s;
  auto w = s.world();
  CHECK(w->apply({{Command::create(obj(1))}}));
  auto platform = platform::make_services();
  CHECK(platform);
  auto scheduler = core::Scheduler::create(*platform.value().threads, {8, 2, 4});
  CHECK(scheduler);
  auto step = core::Rational::make(1, 10).value();
  auto a = std::make_shared<Output>(), b = std::make_shared<Output>(),
       c = std::make_shared<Output>();
  a->x = 10;
  b->x = 20;
  c->x = 30;
  auto system = [&](std::uint64_t id, std::vector<std::uint64_t> deps, std::shared_ptr<Output> out,
                    core::AccessMode mode = core::AccessMode::Write) {
    return System{id, std::move(deps), {{1, mode}}, output_system, std::move(out)};
  };
  {
    auto e = Execution::create(*w, *scheduler.value(), {system(2, {1}, b), system(1, {}, a)}, step,
                               {4, 8, core::OverloadAction::RejectElapsed});
    CHECK(e);
    CHECK(e.value()->evaluate({EvaluationMode::ExactRational, {0, step}, {}},
                              std::chrono::milliseconds::max()));
    {
      auto v = w->read();
      CHECK(v);
      CHECK(std::get<Vec3>(v.value().value(v.value().find(aid(1)).value(), 1, 1).value()).x == 20);
    }
    CHECK(e.value()->evaluate({EvaluationMode::ExactRational, {1, step}, step},
                              std::chrono::milliseconds::max()));
    CHECK_ERROR(
        e.value()->evaluate(
            {EvaluationMode::ExactRational, {2, step}, core::Rational::make(3, 10).value()}, 0ms),
        ErrorCode::InvalidInput);
    CHECK_ERROR(e.value()->evaluate({EvaluationMode::FixedTick, {2, step}, {}}, 0ms),
                ErrorCode::InvalidInput);
  }
  {
    b->fails = true;
    a->called = b->called = c->called = 0;
    auto e = Execution::create(*w, *scheduler.value(),
                               {system(1, {}, a), system(2, {1}, b), system(3, {2}, c)}, step,
                               {4, 8, core::OverloadAction::RejectElapsed});
    CHECK(e);
    auto generation = w->stats().generation;
    CHECK_ERROR(e.value()->evaluate({EvaluationMode::FixedTick, {0, step}, {}},
                                    std::chrono::milliseconds::max()),
                ErrorCode::PlatformFailure);
    CHECK(a->called == 1 && b->called == 1 && c->called == 0);
    CHECK(w->stats().generation == generation && e.value()->published_tick() == 0 &&
          scheduler.value()->stats().occupied == 0);
    b->fails = false;
  }
  {
    auto e = Execution::create(*w, *scheduler.value(), {system(1, {}, a, core::AccessMode::Read)},
                               step, {4, 8, core::OverloadAction::RejectElapsed});
    CHECK(e);
    auto generation = w->stats().generation;
    CHECK_ERROR(e.value()->evaluate({EvaluationMode::FixedTick, {0, step}, {}},
                                    std::chrono::milliseconds::max()),
                ErrorCode::InvalidInput);
    CHECK(w->stats().generation == generation);
  }
  {
    auto l = small();
    l.commands_per_transaction = 1;
    auto bounded = s.world(l);
    CHECK(bounded->apply({{Command::create(obj(1))}}));
    auto e = Execution::create(*bounded, *scheduler.value(), {system(1, {}, a), system(2, {}, b)},
                               step, {4, 8, core::OverloadAction::RejectElapsed});
    CHECK(e);
    auto generation = bounded->stats().generation;
    CHECK_ERROR(e.value()->evaluate({EvaluationMode::FixedTick, {0, step}, {}},
                                    std::chrono::milliseconds::max()),
                ErrorCode::Exhausted);
    CHECK(bounded->stats().generation == generation && scheduler.value()->stats().occupied == 0);
  }
  {
    auto tiny = core::Scheduler::create(*platform.value().threads, {3, 1, 2});
    CHECK(tiny);
    auto occupied = tiny.value()->create_group();
    CHECK(occupied);
    core::JobDesc held([](const core::JobContext &) -> Result<void> { return {}; });
    CHECK(tiny.value()->submit(occupied.value(), held));
    CHECK(tiny.value()->close(occupied.value()));
    CHECK(tiny.value()->wait(occupied.value(), std::chrono::milliseconds::max()));
    auto e =
        Execution::create(*w, *tiny.value(), {system(1, {}, a), system(2, {}, b), system(3, {}, c)},
                          step, {4, 8, core::OverloadAction::RejectElapsed});
    CHECK(e);
    auto generation = w->stats().generation;
    CHECK_ERROR(e.value()->evaluate({EvaluationMode::FixedTick, {0, step}, {}},
                                    std::chrono::milliseconds::max()),
                ErrorCode::QueueFull);
    CHECK(w->stats().generation == generation && tiny.value()->stats().occupied == 1);
    CHECK(w->read());
    CHECK(tiny.value()->release(occupied.value()));
    CHECK(tiny.value()->stats().occupied == 0);
    e.value().reset();
    CHECK(tiny.value()->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max()));
  }
  CHECK(scheduler.value()->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max()));
}
} // namespace
void scene_adversarial_cases() {
  checked_view_handles();
  limits_and_queries();
  section_edges();
  transform_edges();
  template_edges();
  schedules();
}
