#include "race/build_identity.hpp"
#include "race/scene/scene.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <iostream>
#include <locale>
#include <mutex>
#include <string>
#include <thread>
using namespace race;
using namespace race::scene;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
namespace {
struct Failure {
  const char *message;
};
template <class T> T must(Result<T> result) {
  if (!result)
    throw Failure{result.error().message};
  return std::move(result).value();
}
void must(Result<void> result) {
  if (!result)
    throw Failure{result.error().message};
}
void check(bool value, const char *message) {
  if (!value)
    throw Failure{message};
}
std::uint64_t elapsed(Clock::time_point since) {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - since).count());
}
PersistentId id(std::uint64_t i) { return {17, i + 1}; }
Object object(PersistentId identity, Vec3 position = {1, 0, 0}, Reference parent = {}) {
  return {identity, {}, {transform_component({position, {}, {1, 1, 1}}, parent)}, {}};
}
Limits workload_limits(std::size_t n) {
  Limits l;
  l.objects = 2 * n;
  l.components = 4 * n;
  l.commands_per_transaction = 2 * n + 64;
  l.transaction_bytes = 64 * 1024 * 1024;
  l.history_bytes = 128 * 1024 * 1024;
  l.pending_bytes = 64 * 1024 * 1024;
  l.prepared_bytes = 64 * 1024 * 1024;
  l.document_bytes = 128 * 1024 * 1024;
  l.json_scratch_bytes = 512 * 1024 * 1024;
  l.template_objects = 2 * n;
  return l;
}
struct Phase {
  std::string name;
  std::uint64_t ns{}, items{};
  std::vector<std::uint64_t> samples;
};
struct Trial {
  std::vector<Phase> phases;
  std::uint64_t setup{}, verify{}, teardown{}, wall{}, checksum{}, peak_rss{}, pool_bytes{},
      allocator_peak{}, metadata_bytes{}, recomputations{}, touched{}, json_bytes{}, cooked_bytes{};
};
template <class F> void phase(Trial &t, std::string name, std::uint64_t items, F &&f) {
  auto begin = Clock::now();
  f();
  auto ns = elapsed(begin);
  t.phases.push_back({std::move(name), ns, items, {}});
}
template <class F>
void operations(Trial &t, std::string name, std::size_t count, std::uint64_t items, F &&f) {
  Phase p{std::move(name), 0, items, {}};
  p.samples.reserve(count);
  auto begin = Clock::now();
  for (std::size_t i = 0; i < count; ++i) {
    auto op = Clock::now();
    f(i);
    p.samples.push_back(elapsed(op));
  }
  p.ns = elapsed(begin);
  t.phases.push_back(std::move(p));
}
template <class F> void verify(Trial &t, F &&f) {
  auto begin = Clock::now();
  f();
  t.verify += elapsed(begin);
}
struct Resources {
  std::shared_ptr<const Schemas> schemas;
  std::shared_ptr<const Templates> templates;
  platform::Services platform;
  explicit Resources(std::size_t n)
      : schemas(must(Schemas::create({{100, 1, "Counter", {{10, "value", std::int64_t{0}}}},
                                      {200, 1, "Tag", {{10, "enabled", true}}}}))),
        platform(must(platform::make_services())) {
    auto l = workload_limits(n);
    std::vector<Object> first, second, inner;
    for (std::size_t i = 0; i < 8; ++i)
      first.push_back(object({50, i + 1}, {1, 0, 0}, i ? Reference{{}, {50, 1}} : Reference{}));
    second = first;
    second[0].components[0] = transform_component({{3, 0, 0}, {}, {1, 1, 1}});
    inner.push_back(object({51, 1}));
    inner.push_back(object({51, 2}, {1, 0, 0}, {{}, {51, 1}}));
    templates = must(Templates::create({{{{52, 1}, 1}, first, {{{50, 1}, {{52, 2}, 1}}}},
                                        {{{52, 1}, 2}, second, {{{50, 1}, {{52, 2}, 1}}}},
                                        {{{52, 2}, 1}, inner, {}}},
                                       *schemas, l));
  }
};
struct Fixture {
  TrackedAllocator allocator;
  std::unique_ptr<World> world;
  std::size_t n;
  Limits limits;
  Fixture(Resources &r, std::size_t count)
      : allocator(must(TrackedAllocator::create({1024ULL * 1024 * 1024}))), n(count),
        limits(workload_limits(count)) {
    world = must(World::create(allocator, r.schemas, limits, r.templates));
    Transaction initial;
    initial.mode = EditMode::Simulation;
    initial.commands.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      auto o = object(id(i), {1, 0, 0}, i ? Reference{{}, id((i - 1) / 4)} : Reference{});
      if (i % 2 == 0)
        o.components.push_back({100, 1, {{10, static_cast<std::int64_t>(i)}}});
      if (i % 4 == 0)
        o.components.push_back({200, 1, {}});
      initial.commands.push_back(Command::create(std::move(o)));
    }
    must(world->apply(initial));
  }
};
void sample_memory(Trial &t, const Fixture &f) {
  auto rss = must(platform::memory_info());
  t.peak_rss = std::max(t.peak_rss, rss.resident_bytes);
  auto s = f.world->stats();
  t.pool_bytes = s.tracked_pool_bytes;
  t.metadata_bytes = s.bounded_metadata_capacity_bytes;
  t.allocator_peak = f.allocator.snapshot().total.peak;
  t.recomputations = s.derived_recomputations;
  t.touched = s.transaction_touched_objects;
}
void iteration(Trial &t, Fixture &f, Resources &r) {
  auto view = must(f.world->read());
  std::size_t repeats = std::max(std::size_t{8}, 131072 / f.n);
  std::uint64_t single = 0, joined = 0;
  operations(t, "single_pool_scan", repeats, repeats * (f.n / 2), [&](std::size_t) {
    std::uint64_t sum = 0;
    for (auto &row : must(view.query(100)))
      sum += static_cast<std::uint64_t>(std::get<std::int64_t>(row.values[0]));
    single += sum;
  });
  operations(t, "two_pool_intersection", repeats, repeats * (f.n / 4), [&](std::size_t) {
    std::uint64_t sum = 0;
    for (auto &row : must(view.query(200)))
      sum +=
          static_cast<std::uint64_t>(std::get<std::int64_t>(must(view.value(row.object, 100, 10))));
    joined += sum;
  });
  verify(t, [&] {
    check(single == repeats * (f.n / 2) * (f.n / 2 - 1), "single pool checksum");
    check(joined == repeats * 2 * (f.n / 4) * (f.n / 4 - 1), "intersection checksum");
    t.checksum = single ^ joined;
  });
  view.release();
  struct Context {
    std::size_t index;
    std::shared_ptr<std::array<std::uint64_t, 2>> sums;
  };
  auto sums = std::make_shared<std::array<std::uint64_t, 2>>();
  std::vector<System> systems;
  for (std::size_t i = 0; i < 2; ++i) {
    auto context = std::make_shared<Context>(Context{i, sums});
    systems.push_back({i + 1,
                       {},
                       {{100, core::AccessMode::Read}},
                       [](const SystemContext &ctx, void *raw) -> Result<std::vector<Command>> {
                         auto &c = *static_cast<Context *>(raw);
                         auto rows = ctx.view.query(100);
                         if (!rows)
                           return rows.error();
                         std::uint64_t sum = 0;
                         for (std::size_t i = c.index; i < rows.value().size(); i += 2)
                           sum += static_cast<std::uint64_t>(
                               std::get<std::int64_t>(rows.value()[i].values[0]));
                         (*c.sums)[c.index] = sum;
                         return std::vector<Command>{};
                       },
                       context});
  }
  std::unique_ptr<core::Scheduler> scheduler;
  phase(t, "scheduler_startup", 1,
        [&] { scheduler = must(core::Scheduler::create(*r.platform.threads, {16, 2, 4})); });
  auto step = must(core::Rational::make(1, 60));
  auto execution = must(Execution::create(*f.world, *scheduler, std::move(systems), step,
                                          {4, 8, core::OverloadAction::RejectElapsed}));
  operations(t, "declared_system_evaluation", 16, 16 * (f.n / 2), [&](std::size_t i) {
    must(execution->evaluate({EvaluationMode::FixedTick, {i, step}, {}},
                             std::chrono::milliseconds::max()));
  });
  verify(t, [&] {
    check((*sums)[0] + (*sums)[1] == (f.n / 2) * (f.n / 2 - 1), "scheduled iteration checksum");
    check(execution->published_tick() == 16, "evaluation count");
    check(scheduler->stats().occupied == 0, "retained scheduler records");
  });
  phase(t, "scheduler_shutdown", 1, [&] {
    execution.reset();
    must(scheduler->shutdown(core::ShutdownMode::Drain, std::chrono::milliseconds::max()));
    scheduler.reset();
  });
}
void structural(Trial &t, Fixture &f) {
  std::size_t count = std::max(std::size_t{4}, f.n / 64);
  Transaction edit;
  phase(t, "prepare_authoring_batch", count, [&] {
    edit.commands.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
      edit.commands.push_back(
          Command::set(id(i * 2), 100, 10, static_cast<std::int64_t>(i * 2 + 1)));
  });
  phase(t, "apply_authoring_batch", count, [&] { must(f.world->apply(edit)); });
  phase(t, "undo_batch", count, [&] { must(f.world->undo()); });
  phase(t, "redo_batch", count, [&] { must(f.world->redo()); });
  verify(t, [&] {
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < count; ++i)
      check(std::get<std::int64_t>(must(v.value(must(v.find(id(i * 2))), 100, 10))) ==
                static_cast<std::int64_t>(i * 2 + 1),
            "grouped edit checksum");
  });
  operations(t, "single_simulation_edit", count, count, [&](std::size_t i) {
    must(f.world->apply({{Command::set(id(i * 2), 100, 10, static_cast<std::int64_t>(i * 2 + 2))},
                         EditMode::Simulation}));
  });
  std::vector<ObjectHandle> stale;
  {
    auto view = must(f.world->read());
    for (std::size_t i = 0; i < count; ++i)
      stale.push_back(must(view.find(id(f.n - count + i))));
  }
  Transaction replace;
  replace.mode = EditMode::Simulation;
  for (std::size_t i = 0; i < count; ++i) {
    replace.commands.push_back(Command::destroy(id(f.n - count + i)));
    replace.commands.push_back(Command::create(object({18, i + 1})));
  }
  phase(t, "structural_replace_batch", count, [&] { must(f.world->apply(replace)); });
  verify(t, [&] {
    check(f.world->stats().objects == f.n, "structural object count");
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < count; ++i) {
      auto old = v.object(stale[i]);
      check(!old && old.error().code == ErrorCode::StaleHandle,
            "stale replacement handle survived");
      check(!v.find(id(f.n - count + i)), "deleted identity still live");
      check(bool(v.find({18, i + 1})), "replacement identity missing");
    }
    t.checksum = f.world->stats().objects;
  });
}
void transforms(Trial &t, Fixture &f) {
  auto count = std::max(std::size_t{4}, f.n / 64);
  auto before = f.world->stats().derived_recomputations;
  std::uint64_t sum = 0;
  phase(t, "unchanged_transform_queries", f.n, [&] {
    auto v = must(f.world->read());
    for (auto h : v.objects())
      sum += static_cast<std::uint64_t>(must(v.world_transform(h)).m[3]);
  });
  verify(t, [&] {
    check(f.world->stats().derived_recomputations == before, "query recomputed transforms");
    std::uint64_t expected = 0;
    for (std::size_t i = 0; i < f.n; ++i) {
      std::size_t at = i;
      std::uint64_t depth = 1;
      while (at) {
        at = (at - 1) / 4;
        ++depth;
      }
      expected += depth;
    }
    check(sum == expected, "hierarchy checksum");
  });
  operations(t, "leaf_transform_edit", count, count, [&](std::size_t i) {
    must(f.world->apply(
        {{Command::set(id(f.n - count + i), 1, 1, Vec3{2, 0, 0})}, EditMode::Simulation}));
  });
  verify(t, [&] {
    check(f.world->stats().derived_recomputations == before + count, "leaf recomputation count");
  });
  phase(t, "root_transform_edit", f.n, [&] {
    must(f.world->apply({{Command::set(id(0), 1, 1, Vec3{2, 0, 0})}, EditMode::Simulation}));
  });
  verify(t, [&] {
    check(f.world->stats().derived_recomputations == before + count + f.n,
          "root recomputation count");
    auto view = must(f.world->read());
    for (std::size_t i = 0; i < f.n; ++i) {
      std::size_t at = i;
      double expected = 2;
      while (at) {
        at = (at - 1) / 4;
        ++expected;
      }
      if (i >= f.n - count)
        ++expected;
      auto actual = must(view.world_transform(must(view.find(id(i)))));
      check(actual.m[3] == expected && actual.m[0] == 1 && actual.m[5] == 1 && actual.m[10] == 1,
            "independent hierarchy matrix oracle");
    }
  });
  std::vector<Affine> previous;
  {
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < count; ++i)
      previous.push_back(must(v.world_transform(must(v.find(id(f.n - count + i))))));
  }
  operations(t, "preserve_world_reparent", count, count, [&](std::size_t i) {
    must(f.world->apply({{Command::parent_to(id(f.n - count + i), {}, ReparentMode::PreserveWorld)},
                         EditMode::Simulation}));
  });
  verify(t, [&] {
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < count; ++i)
      check(near(previous[i], must(v.world_transform(must(v.find(id(f.n - count + i)))))),
            "reparent matrix mismatch");
    t.checksum = sum;
  });
}
void loading(Trial &t, Fixture &f, Resources &r) {
  auto section = PersistentId{19, 1};
  must(f.world->declare_section({section, {}}));
  auto prepare = must(f.world->begin_prepare(section));
  std::vector<Object> objects;
  for (std::size_t i = 0; i < f.n / 4; ++i) {
    auto o = object({20, i + 1});
    o.section = section;
    objects.push_back(std::move(o));
  }
  phase(t, "section_prepare", objects.size(),
        [&] { must(f.world->finish_prepare(prepare, std::move(objects))); });
  phase(t, "section_activate", f.n / 4, [&] { must(f.world->activate(section)); });
  verify(t, [&] { check(f.world->stats().objects == f.n + f.n / 4, "section activation count"); });
  auto view = must(f.world->read());
  phase(t, "blocked_unload_probe", 1, [&] {
    auto blocked = f.world->unload(section, 0ms);
    check(!blocked && blocked.error().code == ErrorCode::Timeout, "unload failed to retain reader");
  });
  view.release();
  phase(t, "eligible_section_unload", f.n / 4, [&] { must(f.world->unload(section, 0ms)); });
  verify(t, [&] {
    check(f.world->resolve({section, {20, 1}}).state == ReferenceState::Unloaded,
          "unloaded reference status");
  });
  Document document;
  phase(t, "authored_snapshot", f.n, [&] { document = must(f.world->document()); });
  std::string json;
  Cooked cooked;
  Document parsed, decoded;
  phase(t, "json_encode", f.n, [&] { json = must(encode_json(document, *r.schemas, f.limits)); });
  phase(t, "json_decode", f.n, [&] { parsed = must(decode_json(json, *r.schemas, f.limits)); });
  phase(t, "cook_binary", f.n,
        [&] { cooked = must(cook(document, *r.schemas, f.limits, build::authored_input_sha256)); });
  phase(t, "decode_binary", f.n, [&] {
    decoded = must(decode_cooked(cooked.bytes, *r.schemas, f.limits, build::authored_input_sha256));
  });
  sample_memory(t, f);
  auto destination = must(World::create(f.allocator, r.schemas, f.limits, r.templates));
  phase(t, "load_validated_document", f.n, [&] { must(destination->load(parsed)); });
  verify(t, [&] {
    check(must(encode_json(parsed, *r.schemas, f.limits)) == json, "JSON roundtrip mismatch");
    check(must(encode_json(decoded, *r.schemas, f.limits)) == json, "cooked roundtrip mismatch");
    check(must(encode_json(must(destination->document()), *r.schemas, f.limits)) == json,
          "load roundtrip mismatch");
    auto view = must(destination->read());
    std::uint64_t sum = 0;
    for (auto &row : must(view.query(100)))
      sum += std::get<std::int64_t>(row.values[0]);
    check(sum == (f.n / 2) * (f.n / 2 - 1), "independent loaded counter checksum");
    check(view.objects().size() == f.n, "independent loaded object count");
    check(destination->resolve({section, {20, 1}}).state == ReferenceState::Unloaded,
          "loaded manifest status");
    t.checksum = sum;
    t.json_bytes = json.size();
    t.cooked_bytes = cooked.bytes.size();
  });
  sample_memory(t, f);
  must(destination->stop(0ms));
  destination.reset();
}
void templates(Trial &t, Fixture &f) {
  auto instances = f.n / 32;
  std::vector<std::array<PersistentId, 10>> ids(instances);
  for (std::size_t i = 0; i < instances; ++i)
    for (std::size_t j = 0; j < 10; ++j)
      ids[i][j] = {21, i * 10 + j + 1};
  operations(t, "instantiate_template_and_clear_history", instances, instances * 10,
             [&](std::size_t i) {
               must(f.world->instantiate({{52, 1}, 1}, {22, i + 1}, ids[i]));
               must(f.world->clear_history());
             });
  operations(t, "set_template_override_and_clear_history", instances, instances,
             [&](std::size_t i) {
               must(f.world->apply({{Command::set(ids[i][1], 1, 1, Vec3{9, 0, 0})}}));
               must(f.world->clear_history());
             });
  operations(t, "update_template_revision_and_clear_history", instances, instances * 10,
             [&](std::size_t i) {
               must(f.world->update_instance({22, i + 1}, {{52, 1}, 2}));
               must(f.world->clear_history());
             });
  verify(t, [&] {
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < instances; ++i) {
      auto o = must(v.object(must(v.find(ids[i][1]))));
      check(o.binding && o.binding->pinned == TemplateKey{{52, 1}, 2}, "template pin mismatch");
      check(std::get<Vec3>(must(v.value(must(v.find(ids[i][1])), 1, 1))).x == 9,
            "override lost in update");
    }
  });
  operations(t, "reset_template_override_and_clear_history", instances, instances,
             [&](std::size_t i) {
               must(f.world->reset_override(ids[i][1], 1, 1));
               must(f.world->clear_history());
             });
  operations(t, "detach_template_and_clear_history", instances, instances * 10, [&](std::size_t i) {
    must(f.world->detach({22, i + 1}));
    must(f.world->clear_history());
  });
  verify(t, [&] {
    auto v = must(f.world->read());
    for (std::size_t i = 0; i < instances; ++i) {
      auto o = must(v.object(must(v.find(ids[i][1]))));
      check(!o.binding, "template relationship survived detach");
      check(std::get<Vec3>(must(v.value(must(v.find(ids[i][1])), 1, 1))).x == 1,
            "override reset failed");
    }
    check(f.world->stats().objects == f.n + instances * 10, "template object count");
    t.checksum = f.world->stats().objects;
  });
}
void retained(Trial &t, Fixture &f) {
  auto first = must(f.world->read()), second = must(f.world->read());
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false, release = false, reader_valid = false;
  std::thread reader(
      [view = std::move(second), &mutex, &cv, &ready, &release, &reader_valid]() mutable {
        std::unique_lock lock(mutex);
        ready = true;
        cv.notify_one();
        cv.wait(lock, [&] { return release; });
        reader_valid = bool(view.find(id(0)));
        view.release();
      });
  struct Guard {
    std::thread &thread;
    std::mutex &mutex;
    std::condition_variable &cv;
    bool &release;
    ~Guard() {
      if (thread.joinable()) {
        {
          std::lock_guard lock(mutex);
          release = true;
        }
        cv.notify_one();
        thread.join();
      }
    }
  } guard{reader, mutex, cv, release};
  {
    std::unique_lock lock(mutex);
    cv.wait(lock, [&] { return ready; });
  }
  std::array<std::uint64_t, 8> tickets;
  std::array<Clock::time_point, 8> submitted;
  operations(t, "retained_view_admission", 8, 8, [&](std::size_t i) {
    submitted[i] = Clock::now();
    tickets[i] = must(f.world->enqueue(
        {{Command::set(id(0), 1, 1, Vec3{double(i + 2), 0, 0})}, EditMode::Simulation}));
  });
  auto blocked = f.world->publish_pending();
  check(!blocked && blocked.error().code == ErrorCode::Busy,
        "retained views did not block publication");
  verify(t, [&] {
    check(std::get<Vec3>(must(first.value(must(first.find(id(0))), 1, 1))).x == 1,
          "held view changed");
  });
  phase(t, "controlled_release_and_join", 2, [&] {
    first.release();
    {
      std::lock_guard lock(mutex);
      release = true;
    }
    cv.notify_one();
    reader.join();
  });
  phase(t, "eligible_queued_publication", 8, [&] { must(f.world->publish_pending()); });
  Phase latency{"held_admission_to_publication_return", 0, 8, {}};
  for (auto start : submitted)
    latency.samples.push_back(elapsed(start));
  latency.ns = latency.samples.front();
  t.phases.push_back(std::move(latency));
  phase(t, "collect_command_receipts", 8, [&] {
    for (auto ticket : tickets)
      must(must(f.world->take_completion(ticket)).result);
  });
  verify(t, [&] {
    check(reader_valid, "retained reader lost object");
    auto v = must(f.world->read());
    check(std::get<Vec3>(must(v.value(must(v.find(id(0))), 1, 1))).x == 9,
          "FIFO queued publication order");
    check(f.world->stats().pending_transactions == 0, "queue not empty");
    t.checksum = 9;
  });
}
void print_trial(const Trial &t, std::size_t iteration, bool warmup, std::string_view family,
                 std::size_t n) {
  std::cout << "{\"kind\":\"trial\",\"index\":" << iteration
            << ",\"warmup\":" << (warmup ? "true" : "false") << ",\"family\":\"" << family
            << "\",\"objects\":" << n << ",\"setup_ns\":" << t.setup
            << ",\"verification_ns\":" << t.verify << ",\"teardown_ns\":" << t.teardown
            << ",\"full_trial_ns\":" << t.wall << ",\"checksum\":" << t.checksum
            << ",\"sampled_rss_bytes\":" << t.peak_rss << ",\"tracked_pool_bytes\":" << t.pool_bytes
            << ",\"tracked_allocator_peak_bytes\":" << t.allocator_peak
            << ",\"metadata_estimate_bytes\":" << t.metadata_bytes
            << ",\"derived_recomputations\":" << t.recomputations
            << ",\"touched_objects\":" << t.touched << ",\"json_bytes\":" << t.json_bytes
            << ",\"cooked_bytes\":" << t.cooked_bytes << ",\"correct\":true,\"phases\":[";
  for (std::size_t i = 0; i < t.phases.size(); ++i) {
    auto &p = t.phases[i];
    if (i)
      std::cout << ',';
    std::cout << "{\"name\":\"" << p.name << "\",\"elapsed_ns\":" << p.ns
              << ",\"items\":" << p.items << ",\"operation_ns\":[";
    for (std::size_t j = 0; j < p.samples.size(); ++j) {
      if (j)
        std::cout << ',';
      std::cout << p.samples[j];
    }
    std::cout << "]}";
  }
  std::cout << "]}\n" << std::flush;
}
std::size_t number(std::string_view s) {
  std::size_t value{};
  auto r = std::from_chars(s.data(), s.data() + s.size(), value);
  if (r.ec != std::errc{} || r.ptr != s.data() + s.size())
    throw Failure{"invalid numeric argument"};
  return value;
}
void identity() {
  std::printf("{\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\",\"configuration\":\"%s\","
              "\"sanitizer\":\"%s\"}\n",
              build::source_commit, build::authored_input_sha256, build::configuration,
              build::sanitizer);
}
} // namespace
int main(int argc, char **argv) {
  std::cout.imbue(std::locale::classic());
  try {
    std::string family;
    std::size_t n = 0, trials = 9, warmups = 2;
    if (argc == 2 && std::string_view(argv[1]) == "--identity") {
      identity();
      return 0;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      std::puts("--family iteration|structural|transform|loading|template|retained --objects "
                "64..16384 --trials 1..99 --warmups 0..16");
      return 0;
    }
    for (int i = 1; i < argc; ++i) {
      std::string_view arg = argv[i];
      if (i + 1 == argc)
        throw Failure{"option needs value"};
      std::string_view value = argv[++i];
      if (arg == "--family")
        family = value;
      else if (arg == "--objects")
        n = number(value);
      else if (arg == "--trials")
        trials = number(value);
      else if (arg == "--warmups")
        warmups = number(value);
      else
        throw Failure{"unknown option"};
    }
    if (n < 64 || n > 16384 || n % 64 || !trials || trials > 99 || warmups > 16 ||
        (family != "iteration" && family != "structural" && family != "transform" &&
         family != "loading" && family != "template" && family != "retained"))
      throw Failure{"invalid bounded workload configuration"};
    auto process = Clock::now();
    Resources resources(n);
    auto startup = elapsed(process);
    std::cout << "{\"kind\":\"identity\",\"source_commit\":\"" << build::source_commit
              << "\",\"authored_input_sha256\":\"" << build::authored_input_sha256
              << "\",\"configuration\":\"" << build::configuration << "\",\"sanitizer\":\""
              << build::sanitizer << "\",\"family\":\"" << family << "\",\"objects\":" << n
              << ",\"capacity\":" << 2 * n << ",\"warmups\":" << warmups << ",\"trials\":" << trials
              << ",\"resource_startup_ns\":" << startup
              << ",\"scheduler_workers\":" << (family == "iteration" ? 2 : 0)
              << ",\"caller_executes_jobs\":false,\"retained_reader_threads\":"
              << (family == "retained" ? 1 : 0) << "}\n";
    for (std::size_t i = 0; i < warmups + trials; ++i) {
      Trial trial;
      auto begin = Clock::now();
      auto setup = Clock::now();
      auto fixture = std::make_unique<Fixture>(resources, n);
      trial.setup = elapsed(setup);
      if (family == "iteration")
        iteration(trial, *fixture, resources);
      else if (family == "structural")
        structural(trial, *fixture);
      else if (family == "transform")
        transforms(trial, *fixture);
      else if (family == "loading")
        loading(trial, *fixture, resources);
      else if (family == "template")
        templates(trial, *fixture);
      else
        retained(trial, *fixture);
      sample_memory(trial, *fixture);
      auto teardown = Clock::now();
      must(fixture->world->stop(0ms));
      fixture->world.reset();
      check(fixture->allocator.snapshot().total.live == 0,
            "tracked storage leaked after World destruction");
      fixture.reset();
      trial.teardown = elapsed(teardown);
      trial.wall = elapsed(begin);
      print_trial(trial, i, i < warmups, family, n);
    }
    std::cout << "{\"kind\":\"complete\",\"correct\":true,\"records\":" << warmups + trials
              << ",\"process_elapsed_ns\":" << elapsed(process) << "}\n";
    return 0;
  } catch (const Failure &e) {
    std::fprintf(stderr, "scene workload failed: %s\n", e.message);
    return 1;
  } catch (const std::bad_alloc &) {
    std::fputs("scene workload allocation failed\n", stderr);
    return 1;
  } catch (const std::exception &e) {
    std::fprintf(stderr, "scene workload exception: %s\n", e.what());
    return 1;
  }
}
