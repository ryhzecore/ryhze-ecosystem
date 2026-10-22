#include "race/scene/scene.hpp"
#include "test.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#ifdef _WIN32
#include <crtdbg.h>
#include <exception>
#endif
using namespace race;
using namespace race::scene;
void scene_allocation_failures();
void scene_adversarial_cases();
namespace {
PersistentId id(std::uint64_t n) { return {42, n}; }
Object object(std::uint64_t n, Vec3 position = {}, Reference parent = {}) {
  return {id(n), {}, {transform_component({position, {}, {1, 1, 1}}, parent)}, {}};
}
struct Fixture {
  std::unique_ptr<TrackedAllocator> allocator;
  std::shared_ptr<const Schemas> schemas;
  std::unique_ptr<World> world;
  explicit Fixture(std::size_t count = 16) {
    auto a = TrackedAllocator::create({64 * 1024 * 1024});
    CHECK(a);
    allocator = std::make_unique<TrackedAllocator>(std::move(a).value());
    auto s = Schemas::create({{100,
                               1,
                               "Health",
                               {{10, "points", std::int64_t{100}, 0, 1000, true},
                                {20, "asset", PersistentId{}, -1e300, 1e300, false}}}});
    CHECK(s);
    schemas = s.value();
    Limits l;
    l.objects = count;
    l.components = count * 2;
    l.template_objects = count;
    auto w = World::create(*allocator, schemas, l);
    CHECK(w);
    world = std::move(w).value();
  }
};
void objects_and_transactions() {
  Fixture f(3);
  auto &w = *f.world;
  CHECK(w.apply(
      {{Command::create(object(1)), Command::create(object(2)), Command::create(object(3))}}));
  CHECK(w.stats().objects == 3);
  auto view = w.read();
  CHECK(view);
  auto old = view.value().find(id(2));
  CHECK(old);
  auto other = World::create(*f.allocator, f.schemas);
  CHECK(other);
  {
    auto v = other.value()->read();
    CHECK(v);
    CHECK_ERROR(v.value().object(old.value()), ErrorCode::StaleHandle);
  }
  CHECK_ERROR(w.apply({{Command::destroy(id(2))}}), ErrorCode::Busy);
  view.value().release();
  auto generation = w.stats().generation;
  CHECK_ERROR(
      w.apply({{Command::destroy(id(1)), Command::create(object(4)), Command::create(object(5))}}),
      ErrorCode::Exhausted);
  CHECK(w.stats().generation == generation);
  {
    auto v = w.read();
    CHECK(v);
    CHECK(v.value().find(id(1)));
    CHECK(v.value().object(old.value()));
  }
  CHECK(w.apply({{Command::destroy(id(2)), Command::create(object(4))}}));
  {
    auto v = w.read();
    CHECK(v);
    CHECK_ERROR(v.value().object(old.value()), ErrorCode::StaleHandle);
    CHECK(v.value().find(id(4)));
  }
  CHECK(w.undo());
  {
    auto v = w.read();
    CHECK(v);
    auto restored = v.value().find(id(2));
    CHECK(restored && restored.value() != old.value());
    CHECK_ERROR(v.value().object(old.value()), ErrorCode::StaleHandle);
  }
  CHECK(w.redo());
  Command put;
  put.kind = CommandKind::PutComponent;
  put.target = id(1);
  put.component = {100, 1, {}};
  CHECK(w.apply({{put}}));
  {
    auto v = w.read();
    CHECK(v);
    auto h = v.value().find(id(1));
    CHECK(h);
    CHECK(std::get<std::int64_t>(v.value().value(h.value(), 100, 10).value()) == 100);
  }
  auto before = w.stats();
  CHECK_ERROR(w.apply({{Command::set(id(1), 100, 10, std::int64_t{50}),
                        Command::set(id(1), 100, 10, std::int64_t{-1})}}),
              ErrorCode::InvalidInput);
  CHECK(w.stats().generation == before.generation);
  auto history = w.stats().history_groups;
  CHECK(w.apply({{Command::set(id(1), 100, 10, std::int64_t{33})}, EditMode::Simulation}));
  CHECK(w.stats().history_groups == history);
  CHECK_ERROR(w.undo(), ErrorCode::Busy);
  std::thread wrong(
      [&] { CHECK_ERROR(w.apply({{Command::destroy(id(1))}}), ErrorCode::WrongThread); });
  wrong.join();
}
void transforms() {
  Fixture f;
  auto &w = *f.world;
  CHECK(w.apply(
      {{Command::create(object(1, {10, 0, 0})), Command::create(object(2, {0, 2, 0}, {{}, id(1)})),
        Command::create(object(3, {0, 0, 3}, {{}, id(2)}))}}));
  auto before = w.stats();
  {
    auto v = w.read();
    CHECK(v);
    auto h = v.value().find(id(3));
    CHECK(h);
    auto m = v.value().world_transform(h.value());
    CHECK(m && m.value().m[3] == 10 && m.value().m[7] == 2 && m.value().m[11] == 3);
  }
  CHECK(w.stats().derived_recomputations == before.derived_recomputations);
  CHECK(w.apply({{Command::set(id(3), 1, 1, Vec3{0, 0, 4})}}));
  CHECK(w.stats().derived_recomputations == before.derived_recomputations + 1);
  auto gen = w.stats().generation;
  CHECK_ERROR(w.apply({{Command::parent_to(id(1), {{}, id(3)}, ReparentMode::PreserveLocal)}}),
              ErrorCode::Cycle);
  CHECK(w.stats().generation == gen);
  CHECK_ERROR(w.apply({{Command::destroy(id(2))}}), ErrorCode::InvalidDependency);
  Affine previous;
  {
    auto v = w.read();
    CHECK(v);
    previous = v.value().world_transform(v.value().find(id(3)).value()).value();
  }
  CHECK(w.apply({{Command::parent_to(id(3), {}, ReparentMode::PreserveWorld)}}));
  {
    auto v = w.read();
    CHECK(v);
    CHECK(near(previous, v.value().world_transform(v.value().find(id(3)).value()).value()));
  }
  CHECK(w.apply({{Command::set(id(2), 1, 3, Vec3{0, 1, 1})}}));
  CHECK_ERROR(w.apply({{Command::parent_to(id(3), {{}, id(2)}, ReparentMode::PreserveWorld)}}),
              ErrorCode::InvalidInput);
  CHECK_ERROR(
      w.apply({{Command::set(id(1), 1, 1, Vec3{std::numeric_limits<double>::infinity(), 0, 0})}}),
      ErrorCode::InvalidInput);
  CHECK(axis_angle({1e-320, 0, 0}, 1.0));
  auto q = axis_angle({0, 1, 0}, .7);
  CHECK(q);
  auto shear =
      compose(affine({{}, {}, {2, 1, 1}}).value(), affine({{}, q.value(), {1, 1, 1}}).value());
  CHECK(shear);
  CHECK_ERROR(decompose(shear.value()), ErrorCode::Unsupported);
  for (auto scale : {Vec3{0, 0, 0}, Vec3{0, 2, 0}, Vec3{0, -2, 3}, Vec3{-1, 2, 3}}) {
    auto a = affine({{1, 2, 3}, q.value(), scale});
    CHECK(a);
    auto d = decompose(a.value());
    CHECK(d);
    CHECK(near(a.value(), affine(d.value()).value()));
  }
  CHECK(w.set_origin({1e12, 0, 0}));
  auto v = w.read();
  CHECK(v);
  auto relative = relative_position({1e12 + .25, 0, 0}, v.value().origin());
  CHECK(relative && relative.value().metres.x == .25);
  auto origin = v.value().origin();
  v.value().release();
  CHECK(w.set_origin({1e12 + 1, 0, 0}));
  CHECK_ERROR(absolute_position(relative.value(), {{1e12 + 1, 0, 0}, origin.revision + 1}),
              ErrorCode::StaleHandle);
}
void readers_and_queue() {
  Fixture f;
  auto &w = *f.world;
  CHECK(w.apply({{Command::create(object(1))}}));
  auto read = w.read();
  CHECK(read);
  std::atomic<std::uint64_t> ticket{};
  std::thread producer([&] {
    auto r = w.enqueue({{Command::set(id(1), 1, 1, Vec3{8, 0, 0})}});
    CHECK(r);
    ticket = r.value();
  });
  producer.join();
  CHECK_ERROR(w.read(), ErrorCode::Busy);
  CHECK_ERROR(w.publish_pending(), ErrorCode::Busy);
  auto h = read.value().find(id(1));
  CHECK(h);
  CHECK(std::get<Vec3>(read.value().value(h.value(), 1, 1).value()).x == 0);
  read.value().release();
  CHECK(w.publish_pending());
  auto c = w.take_completion(ticket);
  CHECK(c && c.value().result);
  CHECK_ERROR(w.take_completion(ticket), ErrorCode::NotFound);
  auto v = w.read();
  CHECK(v);
  CHECK(std::get<Vec3>(v.value().value(h.value(), 1, 1).value()).x == 8);
  // The facade may be destroyed while a view retains all backing allocations.
  auto live = f.allocator->snapshot().total.live;
  CHECK(live > 0);
  f.world.reset();
  CHECK(v.value().object(h.value()));
  CHECK(f.allocator->snapshot().total.live == live);
  v.value().release();
  CHECK(f.allocator->snapshot().total.live == 0);
}
void sections() {
  Fixture f;
  auto &w = *f.world;
  auto a = id(100), b = id(101);
  CHECK(w.declare_section({a, {}}));
  CHECK(w.declare_section({b, {a}}));
  auto p = w.begin_prepare(a);
  CHECK(p);
  Object oa = object(1);
  oa.section = a;
  CHECK(w.finish_prepare(p.value(), {oa}));
  CHECK(w.activate(a));
  auto q = w.begin_prepare(b);
  CHECK(q);
  Object ob = object(2, {}, {a, id(1)});
  ob.section = b;
  CHECK(w.finish_prepare(q.value(), {ob}));
  CHECK(w.activate(b));
  CHECK_ERROR(w.unload(a, std::chrono::milliseconds{0}), ErrorCode::Busy);
  auto view = w.read();
  CHECK(view);
  CHECK_ERROR(w.unload(b, std::chrono::milliseconds{0}), ErrorCode::Timeout);
  CHECK(w.stats().objects == 2);
  CHECK_ERROR(w.read(), ErrorCode::Busy);
  auto queued = w.enqueue({{Command::destroy(id(2))}});
  CHECK(queued);
  view.value().release();
  CHECK(w.publish_pending());
  auto completion = w.take_completion(queued.value());
  CHECK(completion);
  CHECK_ERROR(completion.value().result, ErrorCode::Busy);
  CHECK(w.unload(b, std::chrono::milliseconds{0}));
  CHECK(w.resolve({b, id(2)}).state == ReferenceState::Unloaded);
  CHECK(w.resolve({b, id(99)}).state == ReferenceState::Missing);
  CHECK(w.resolve({id(999), id(2)}).state == ReferenceState::Invalid);
  auto r = w.begin_prepare(a);
  CHECK(r);
  auto bad = oa;
  bad.components[0].fields[0].value = Vec3{INFINITY, 0, 0};
  CHECK_ERROR(w.finish_prepare(r.value(), {bad}), ErrorCode::InvalidInput);
  CHECK(w.section(a).value().state == SectionState::Active);
  CHECK(w.resolve({a, id(1)}).state == ReferenceState::Available);
  auto cancel = w.begin_prepare(b);
  CHECK(cancel);
  std::thread worker([&] { CHECK(w.cancel_prepare(cancel.value())); });
  worker.join();
  CHECK_ERROR(w.finish_prepare(cancel.value(), {ob}), ErrorCode::Cancelled);
  auto fresh = w.begin_prepare(b);
  CHECK(fresh);
  CHECK_ERROR(w.finish_prepare(cancel.value(), {ob}), ErrorCode::StaleHandle);
  CHECK(w.finish_prepare(fresh.value(), {ob}));
  CHECK(w.activate(b));
  auto d = w.document();
  CHECK(d);
  auto other = World::create(*f.allocator, f.schemas);
  CHECK(other);
  CHECK(other.value()->load(d.value()));
  CHECK(other.value()->stats().objects == 2);
  auto invalid = d.value();
  invalid.objects.push_back(invalid.objects[0]);
  CHECK_ERROR(other.value()->load(invalid), ErrorCode::Duplicate);
  CHECK(other.value()->stats().objects == 2);
  auto held = w.read();
  CHECK(held);
  CHECK_ERROR(w.stop(std::chrono::milliseconds{0}), ErrorCode::Timeout);
  CHECK(w.stats().objects == 2);
  held.value().release();
  CHECK(w.stop(std::chrono::milliseconds{0}));
  CHECK(w.stats().objects == 0);
  CHECK(w.teardown_order() == std::vector<PersistentId>({b, a}));
  CHECK(w.stop(std::chrono::milliseconds{0}));
  Fixture cycle;
  CHECK(cycle.world->declare_section({a, {b}}));
  CHECK_ERROR(cycle.world->declare_section({b, {a}}), ErrorCode::Cycle);
}

void templates() {
  Fixture f;
  Limits limits;
  limits.objects = 16;
  limits.components = 32;
  limits.template_objects = 16;
  TemplateKey key{id(500), 1}, second{id(500), 2}, removed{id(500), 3}, nested{id(501), 1};
  auto root = object(10, {1, 0, 0});
  auto child = object(11, {0, 2, 0}, {{}, id(10)});
  auto next = root;
  next.components[0].fields[0].value = Vec3{3, 0, 0};
  auto catalog = Templates::create({{key, {root, child}, {}},
                                    {second, {next, child}, {}},
                                    {removed, {root}, {}},
                                    {nested, {object(12)}, {{id(12), key}}}},
                                   *f.schemas, limits);
  CHECK(catalog);
  auto result = World::create(*f.allocator, f.schemas, limits, catalog.value());
  CHECK(result);
  auto &w = *result.value();
  std::array<PersistentId, 2> ids{id(1), id(2)};
  CHECK(w.instantiate(key, id(600), ids));
  {
    auto v = w.read();
    CHECK(v);
    auto a = v.value().find(id(1));
    auto b = v.value().find(id(2));
    CHECK(a && b);
    auto parent = v.value().value(b.value(), 1, 4);
    CHECK(parent && std::get<Reference>(parent.value()).object == id(1));
  }
  CHECK(w.apply({{Command::set(id(2), 1, 1, Vec3{0, 9, 0})}}));
  CHECK(w.update_instance(id(600), second));
  {
    auto v = w.read();
    CHECK(v);
    auto o = v.value().object(v.value().find(id(2)).value());
    CHECK(o && o.value().binding->pinned == second);
    CHECK(std::get<Vec3>(v.value().value(v.value().find(id(2)).value(), 1, 1).value()).y == 9);
  }
  auto gen = w.stats().generation;
  CHECK_ERROR(w.update_instance(id(600), removed), ErrorCode::Busy);
  CHECK(w.stats().generation == gen);
  CHECK(w.reset_override(id(2), 1, 1));
  {
    auto v = w.read();
    CHECK(v);
    CHECK(std::get<Vec3>(v.value().value(v.value().find(id(2)).value(), 1, 1).value()).y == 2);
  }
  CHECK(w.detach(id(600)));
  {
    auto v = w.read();
    CHECK(v);
    CHECK(!v.value().object(v.value().find(id(1)).value()).value().binding);
  }
  std::array<PersistentId, 3> nested_ids{id(3), id(4), id(5)};
  CHECK(w.instantiate(nested, id(601), nested_ids));
  CHECK(w.stats().objects == 5);
  auto cycle = Templates::create(
      {{key, {root}, {{id(10), second}}}, {second, {root}, {{id(10), key}}}}, *f.schemas, limits);
  CHECK_ERROR(cycle, ErrorCode::Cycle);
}

struct MemoryFiles : platform::Files {
  std::vector<std::byte> saved;
  bool fail_write{};
  Result<std::vector<std::byte>> read(std::string_view, std::size_t limit) override {
    if (saved.size() > limit)
      return fail(ErrorCode::Exhausted, "fixture limit");
    return saved;
  }
  Result<void> replace(std::string_view, std::span<const std::byte> bytes,
                       platform::Durability durability) override {
    if (durability != platform::Durability::AtomicVisibility)
      return fail(ErrorCode::Unsupported, "fixture durable unsupported");
    if (fail_write)
      return fail(ErrorCode::IoFailure, "injected atomic replacement failure");
    std::vector<std::byte> next(bytes.begin(), bytes.end());
    saved.swap(next);
    return {};
  }
};
void persistence() {
  Fixture f;
  auto &w = *f.world;
  auto o = object(1, {.125, 1e12, 3});
  o.components.push_back(
      {100, 1, {{10, std::int64_t{999}}, {20, PersistentId{UINT64_MAX, UINT64_MAX}}}});
  CHECK(w.apply({{Command::create(o), Command::create(object(2, {}, {{}, id(1)}))}}));
  auto document = w.document();
  CHECK(document);
  Limits limits;
  Limits small = limits;
  small.document_bytes = 256;
  auto empty = encode_json(Document{}, *f.schemas, small);
  CHECK(empty);
  CHECK(decode_json(empty.value(), *f.schemas, small));
  auto json = encode_json(document.value(), *f.schemas, limits);
  CHECK(json);
  auto decoded = decode_json(json.value(), *f.schemas, limits);
  CHECK(decoded);
  CHECK(decoded.value().objects == document.value().objects);
  auto recoded = encode_json(decoded.value(), *f.schemas, limits);
  CHECK(recoded && recoded.value() == json.value());
  auto cooked = cook(document.value(), *f.schemas, limits, "source-test-identity");
  CHECK(cooked);
  auto binary = decode_cooked(cooked.value().bytes, *f.schemas, limits, "source-test-identity");
  CHECK(binary && binary.value().objects == decoded.value().objects);
  auto reordered = document.value();
  std::reverse(reordered.objects.begin(), reordered.objects.end());
  CHECK(cook(reordered, *f.schemas, limits, "source-test-identity").value().bytes ==
        cooked.value().bytes);
  CHECK_ERROR(decode_cooked(cooked.value().bytes, *f.schemas, limits, "wrong-source"),
              ErrorCode::InvalidInput);
  for (std::size_t size = 0; size < cooked.value().bytes.size(); size += 13)
    CHECK(!decode_cooked(std::span(cooked.value().bytes).first(size), *f.schemas, limits));
  auto malformed = cooked.value().bytes;
  malformed[8] = std::byte{2};
  CHECK_ERROR(decode_cooked(malformed, *f.schemas, limits), ErrorCode::IncompatibleVersion);
  malformed = cooked.value().bytes;
  malformed[16] = std::byte{255};
  CHECK(!decode_cooked(malformed, *f.schemas, limits));
  for (auto bad :
       {std::string("{}"), std::string("{\"schema\":1,\"schema\":1}"), json.value() + "x",
        std::string("{\"schema\":2,\"origin\":[],\"objects\":[],\"sections\":[]}")})
    CHECK(!decode_json(bad, *f.schemas, limits));
  auto unknown = json.value();
  auto pos = unknown.find("\"type\": 1");
  CHECK(pos != std::string::npos);
  unknown.replace(pos, 9, "\"type\": 9");
  CHECK_ERROR(decode_json(unknown, *f.schemas, limits), ErrorCode::Unsupported);
  MemoryFiles files;
  CHECK(save_json(files, "/scene.json", w, *f.schemas, limits));
  auto previous = files.saved;
  files.fail_write = true;
  CHECK_ERROR(save_json(files, "/scene.json", w, *f.schemas, limits), ErrorCode::IoFailure);
  CHECK(files.saved == previous);
  files.fail_write = false;
  CHECK_ERROR(save_json(files, "/scene.json", w, *f.schemas, limits, platform::Durability::Durable),
              ErrorCode::Unsupported);
  CHECK(files.saved == previous);
}

void adversarial_sections() {
  Fixture f(2);
  auto &w = *f.world;
  auto a = id(100), b = id(101);
  CHECK(w.declare_section({a, {}}));
  CHECK(w.declare_section({b, {}}));
  auto p = w.begin_prepare(a);
  CHECK(p);
  auto o = object(1);
  o.section = a;
  CHECK(w.finish_prepare(p.value(), {o}));
  CHECK(w.apply({{Command::create(object(1))}}));
  CHECK_ERROR(w.activate(a), ErrorCode::Duplicate);
  {
    auto v = w.read();
    CHECK(v);
    CHECK(v.value().object(v.value().find(id(1)).value()).value().section == PersistentId{});
  }
  CHECK(w.cancel_prepare(p.value()));
  auto old = w.begin_prepare(a);
  CHECK(old);
  CHECK(w.cancel_prepare(old.value()));
  auto d = w.document();
  CHECK(d);
  CHECK(w.load(d.value()));
  auto fresh = w.begin_prepare(a);
  CHECK(fresh && fresh.value().ticket != old.value().ticket);
  CHECK_ERROR(w.cancel_prepare(old.value()), ErrorCode::StaleHandle);
  CHECK(w.cancel_prepare(fresh.value()));
  Fixture other(2);
  CHECK(other.world->declare_section({a, {}}));
  auto foreign = other.world->begin_prepare(a);
  CHECK(foreign);
  CHECK_ERROR(w.cancel_prepare(foreign.value()), ErrorCode::StaleHandle);
  Transaction empty;
  empty.commands.reserve(20000);
  CHECK_ERROR(w.enqueue(std::move(empty)), ErrorCode::Exhausted);
  auto invalid_mode = Transaction{{}, static_cast<EditMode>(99)};
  CHECK_ERROR(w.apply(invalid_mode), ErrorCode::InvalidInput);
  CHECK(w.stop(std::chrono::milliseconds{0}));
  CHECK(other.world->stop(std::chrono::milliseconds{0}));
  CHECK(other.world->section(a).value().state == SectionState::Cancelled);
}

struct SystemState {
  std::mutex mutex;
  std::condition_variable cv;
  bool blocked{}, entered{}, release{}, fail_once{};
  std::thread::id caller;
  std::atomic<unsigned> calls{};
};
Result<std::vector<Command>> step_system(const SystemContext &ctx, void *raw) {
  auto &s = *static_cast<SystemState *>(raw);
  CHECK(std::this_thread::get_id() != s.caller);
  ++s.calls;
  {
    std::unique_lock lock(s.mutex);
    s.entered = true;
    s.cv.notify_all();
    if (s.blocked)
      s.cv.wait(lock, [&] { return s.release; });
    if (s.fail_once) {
      s.fail_once = false;
      return fail(ErrorCode::PlatformFailure, "injected Scene system failure");
    }
  }
  if (ctx.job.cancelled())
    return fail(ErrorCode::Cancelled, "cancelled system");
  auto h = ctx.view.find(id(1));
  if (!h)
    return h.error();
  auto v = ctx.view.value(h.value(), 1, 1);
  if (!v)
    return v.error();
  auto pos = std::get<Vec3>(v.value());
  ++pos.x;
  return std::vector<Command>{Command::set(id(1), 1, 1, pos, h.value())};
}
void execution() {
  Fixture f;
  auto &w = *f.world;
  CHECK(w.apply({{Command::create(object(1))}}));
  auto services = platform::make_services();
  CHECK(services);
  auto scheduler = core::Scheduler::create(*services.value().threads, {16, 2, 4});
  CHECK(scheduler);
  auto state = std::make_shared<SystemState>();
  state->caller = std::this_thread::get_id();
  auto step = core::Rational::make(1, 10).value();
  auto make = [&]() {
    return Execution::create(w, *scheduler.value(),
                             {{1, {}, {{1, core::AccessMode::Write}}, step_system, state}}, step,
                             {4, 8, core::OverloadAction::RejectElapsed});
  };
  auto executor = make();
  CHECK(executor);
  auto e = Evaluation{EvaluationMode::FixedTick, {0, step}, {}};
  CHECK(executor.value()->evaluate(e, std::chrono::milliseconds::max()));
  CHECK(executor.value()->published_tick() == 1);
  CHECK(w.stats().history_groups == 1);
  {
    auto v = w.read();
    CHECK(v);
    CHECK(std::get<Vec3>(v.value().value(v.value().find(id(1)).value(), 1, 1).value()).x == 1);
  }
  auto held = w.read();
  CHECK(held);
  e.tick.tick_index = 1;
  CHECK_ERROR(executor.value()->evaluate(e, std::chrono::milliseconds::max()), ErrorCode::Busy);
  CHECK(executor.value()->published_tick() == 1);
  CHECK_ERROR(w.read(), ErrorCode::Busy);
  held.value().release();
  CHECK(executor.value()->poll(std::chrono::milliseconds{0}));
  CHECK(executor.value()->published_tick() == 2);
  {
    std::lock_guard lock(state->mutex);
    state->blocked = true;
    state->release = false;
    state->entered = false;
  }
  e.tick.tick_index = 2;
  CHECK_ERROR(executor.value()->evaluate(e, std::chrono::milliseconds{0}), ErrorCode::Timeout);
  {
    std::unique_lock lock(state->mutex);
    state->cv.wait(lock, [&] { return state->entered; });
  }
  CHECK_ERROR(executor.value()->advance({0, 1}, std::chrono::milliseconds{0}),
              ErrorCode::InvalidInput);
  CHECK_ERROR(w.stop(std::chrono::milliseconds::max()), ErrorCode::Busy);
  CHECK_ERROR(w.stop(std::chrono::milliseconds{0}), ErrorCode::Timeout);
  CHECK(w.stats().objects == 1);
  CHECK(executor.value()->cancel());
  {
    std::lock_guard lock(state->mutex);
    state->release = true;
  }
  state->cv.notify_all();
  CHECK_ERROR(executor.value()->poll(std::chrono::milliseconds::max()), ErrorCode::Cancelled);
  CHECK(executor.value()->published_tick() == 2);
  CHECK(w.stop(std::chrono::milliseconds{0}));
  executor.value().reset();
  CHECK(scheduler.value()->stats().occupied == 0);
  Fixture real;
  CHECK(real.world->apply({{Command::create(object(1))}}));
  auto faulty = std::make_shared<SystemState>();
  faulty->caller = std::this_thread::get_id();
  faulty->fail_once = true;
  auto realtime = Execution::create(*real.world, *scheduler.value(),
                                    {{1, {}, {{1, core::AccessMode::Write}}, step_system, faulty}},
                                    step, {4, 8, core::OverloadAction::RejectElapsed});
  CHECK(realtime);
  CHECK(realtime.value()->advance({0, 1}, std::chrono::milliseconds::max()));
  CHECK_ERROR(realtime.value()->advance({200000000, 1}, std::chrono::milliseconds::max()),
              ErrorCode::PlatformFailure);
  CHECK(realtime.value()->published_tick() == 0);
  auto retry = realtime.value()->advance({300000000, 1}, std::chrono::milliseconds::max());
  CHECK(retry && retry.value().steps == 2);
  CHECK(realtime.value()->published_tick() == 2);
  auto next = realtime.value()->advance({300000000, 1}, std::chrono::milliseconds::max());
  CHECK(next && next.value().steps == 1);
  CHECK(realtime.value()->published_tick() == 3);
  auto cycle =
      Execution::create(*real.world, *scheduler.value(),
                        {{1, {2}, {}, step_system, faulty}, {2, {1}, {}, step_system, faulty}},
                        step, {4, 8, core::OverloadAction::RejectElapsed});
  CHECK_ERROR(std::move(cycle), ErrorCode::Cycle);
  realtime.value().reset();
  CHECK(scheduler.value()->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max()));
}

} // namespace
int main() {
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
  _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
  _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
  std::set_terminate([] {
    std::fprintf(stderr, "scene: std::terminate\n");
    if (auto exception = std::current_exception()) {
      try { std::rethrow_exception(exception); }
      catch (const std::exception& e) { std::fprintf(stderr, "scene: exception %s\n", e.what()); }
      catch (...) { std::fprintf(stderr, "scene: unknown exception\n"); }
    }
    std::_Exit(86);
  });
#endif
  objects_and_transactions();
  transforms();
  readers_and_queue();
  sections();
  templates();
  persistence();
  adversarial_sections();
  execution();
  scene_allocation_failures();
  scene_adversarial_cases();
  std::puts("scene objects/transforms/transactions/views: passed");
}
