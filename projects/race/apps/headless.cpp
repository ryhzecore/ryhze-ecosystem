#include "race/runtime/lifecycle.hpp"
#include "race/build_identity.hpp"
#include "race/core/time.hpp"
#include "race/core/config.hpp"
#include "race/foundation/identity.hpp"
#include "mock_io.hpp"
#include <cstdio>
#include <cstring>
#include <optional>

using namespace race;
using namespace std::chrono_literals;
namespace {
struct Payload { OwnedBlock bytes; ArenaLease scratch; std::uint64_t value = 0; };
struct Shared {
  HandleTable<Payload> table{4};
  Handle<Payload> handle;
  core::JobHandle produced;
  core::GroupHandle consumer_group;
};
class Producer final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_{"mock-producer",1,{},0,runtime::Memory|runtime::Jobs};
  Shared& shared_;
  runtime::ModuleContext context_;
  std::shared_ptr<Payload> payload_;
  std::optional<ScopedArena> arena_;
  bool registered_ = false;
public:
  explicit Producer(Shared& shared) : shared_(shared) {}
  const runtime::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return {}; }
  Result<void> initialize(runtime::ModuleContext context) noexcept override {
    context_ = context;
    try {
      auto arena = ScopedArena::create(*context.memory,4096); if (!arena) return arena.error();
      arena_.emplace(std::move(arena).value());
      auto scratch = arena_->allocate(64,16); if (!scratch) return scratch.error();
      auto bytes = context.memory->allocate({256,64,0,0}); if (!bytes) return bytes.error();
      payload_ = std::make_shared<Payload>(Payload{std::move(bytes).value(),std::move(scratch).value(),0});
      auto handle = shared_.table.insert(payload_); if (!handle) return handle.error();
      shared_.handle = handle.value(); registered_ = true;
      return {};
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"mock producer allocation"); }
  }
  Result<void> start() noexcept override {
    const core::ResourceAccess access{1,core::AccessMode::Write};
    core::JobDesc desc; desc.owned_context = payload_; desc.accesses = std::span(&access,1); desc.unknown_access = false;
    desc.function = [](const core::JobContext& ctx)->Result<void> {
      if (ctx.cancelled()) return fail(ErrorCode::Cancelled,"mock producer cancelled");
      auto& payload = *static_cast<Payload*>(ctx.context());
      std::memset(payload.bytes.data(),42,payload.bytes.size());
      payload.scratch.bytes()[0] = std::byte{42}; payload.value = 42; return {};
    };
    auto job = context_.jobs->submit(context_.task_group,desc); if (!job) return job.error();
    shared_.produced = job.value();
    auto reset = arena_->reset();
    if (reset || reset.error().code != ErrorCode::Busy) return fail(ErrorCode::Invariant,"borrow did not protect arena");
    return context_.jobs->close(context_.task_group);
  }
  void request_stop() noexcept override {}
  Result<void> quiesce(std::chrono::milliseconds) noexcept override { return {}; }
  void cleanup() noexcept {
    if (registered_) { auto erased = shared_.table.erase(shared_.handle); if (!erased) std::abort(); registered_ = false; }
    payload_.reset();
    if (arena_) { auto reset = arena_->reset(); if (!reset) std::abort(); arena_.reset(); }
  }
  void shutdown() noexcept override { cleanup(); }
  void rollback_initialization() noexcept override { cleanup(); }
};
class Consumer final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_{"mock-consumer",1,{"mock-producer"},0,runtime::Jobs};
  Shared& shared_;
  runtime::ModuleContext context_;
  struct OwnedAccess { AccessScope<Payload> access; };
  std::shared_ptr<OwnedAccess> access_;
public:
  explicit Consumer(Shared& shared) : shared_(shared) {}
  const runtime::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return {}; }
  Result<void> initialize(runtime::ModuleContext context) noexcept override {
    context_ = context; shared_.consumer_group = context.task_group;
    auto handle = shared_.table.resolve(shared_.handle); if (!handle) return handle.error();
    try { access_ = std::make_shared<OwnedAccess>(OwnedAccess{std::move(handle).value()}); return {}; }
    catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"mock consumer allocation"); }
  }
  Result<void> start() noexcept override {
    const core::ResourceAccess access{1,core::AccessMode::Read};
    core::JobDesc desc; desc.owned_context = access_; desc.prerequisites = std::span(&shared_.produced,1);
    desc.accesses = std::span(&access,1); desc.unknown_access = false;
    desc.function = [](const core::JobContext& ctx)->Result<void> {
      auto& p = static_cast<OwnedAccess*>(ctx.context())->access.get();
      if (p.value != 42 || p.bytes.data()[0] != std::byte{42}) return fail(ErrorCode::Invariant,"invalid produced fixture");
      return {};
    };
    auto job = context_.jobs->submit(context_.task_group,desc); if (!job) return job.error();
    return context_.jobs->close(context_.task_group);
  }
  void request_stop() noexcept override {}
  Result<void> quiesce(std::chrono::milliseconds) noexcept override { return {}; }
  void shutdown() noexcept override { access_.reset(); }
  void rollback_initialization() noexcept override { access_.reset(); }
};
Result<void> evaluate(core::Scheduler& jobs, Shared& shared, std::uint64_t index) {
  struct Evaluation { AccessScope<Payload> source; std::uint64_t index; std::uint64_t result = 0; };
  auto source = shared.table.resolve(shared.handle); if (!source) return source.error();
  auto context = std::make_shared<Evaluation>(Evaluation{std::move(source).value(),index,0});
  auto group = jobs.create_group(); if (!group) return group.error();
  core::JobDesc job; job.owned_context = context;
  job.function = [](const core::JobContext& ctx)->Result<void> {
    if (ctx.cancelled()) return fail(ErrorCode::Cancelled,"mock evaluation cancelled");
    auto& value = *static_cast<Evaluation*>(ctx.context());
    value.result = value.source->value + value.index; return {};
  };
  auto submitted = jobs.submit(group.value(),job); if (!submitted) return submitted.error();
  auto closed = jobs.close(group.value()); if (!closed) return closed;
  auto completed = jobs.wait(group.value(),5s); if (!completed) return completed.error();
  if (completed.value().succeeded != 1 || context->result != 42 + index)
    return fail(ErrorCode::FailedPrerequisite,"mock evaluation failed");
  return jobs.release(group.value());
}
Result<void> run(std::string_view mode) {
  auto platform_result = platform::make_services(); if (!platform_result) return platform_result.error();
  auto services = std::move(platform_result).value();
  core::ConfigPatch project;
  if (mode == "authority") { auto set = project.set("authoritative",true); if (!set) return set; }
  if (mode == "film") { auto set = project.set("fixed_quality",true); if (!set) return set; }
  auto config = core::resolve_config(core::headless_schema(),project); if (!config) return config.error();
  auto info = platform::memory_info(); if (!info) return info.error();
  // Fixture policy: one percent of detected physical memory; not an engine budget.
  auto budget = automatic_memory_budget(info.value().physical_bytes,1,100); if (!budget) return budget.error();
  auto allocator = TrackedAllocator::create({budget.value()}); if (!allocator) return allocator.error();
  auto memory = std::move(allocator).value();
  auto scheduler = core::Scheduler::create(*services.threads,{64,2,8}); if (!scheduler) return scheduler.error();
  auto jobs = std::move(scheduler).value();
  core::Diagnostics diagnostics(*services.clock);
  Shared shared;
  runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
  auto added = runtime.add(std::make_unique<Consumer>(shared)); if (!added) return added;
  added = runtime.add(std::make_unique<Producer>(shared)); if (!added) return added;
  auto started = runtime.start(); if (!started) return started;
  auto completion = jobs->wait(shared.consumer_group,5s); if (!completion) return completion.error();
  if (completion.value().succeeded != 1) return fail(ErrorCode::Invariant,"consumer fixture failed");
  // Mock asynchronous ownership is independent of the CPU worker pool.
  auto io_buffer = memory.allocate({32}); if (!io_buffer) return io_buffer.error();
  auto io = fixtures::MockIo::begin(*services.threads,std::move(io_buffer).value()); if (!io) return io.error();
  auto io_done = io.value()->wait(1s); if (!io_done) return io_done;
  auto delivered = io.value()->take(); if (!delivered) return delivered.error();
  if (!delivered.value() || delivered.value()->cancelled || delivered.value()->buffer.data()[0] != std::byte{42})
    return fail(ErrorCode::Invariant,"mock I/O completion failed");
  delivered.value().reset(); io.value().reset();
  std::uint64_t ticks = 0, samples = 0;
  if (mode == "film") {
    core::CheckpointIdentity identity{"mock-source-v1","producer-consumer-contract-1",config.value()->identity(),"seconds-v1",7};
    auto sequence = core::OfflineSequence::create({0,2,2,core::Rational::make(24000,1001).value(),identity});
    if (!sequence) return sequence.error();
    while (!sequence.value().finished()) {
      auto time = sequence.value().next(); if (!time) return time.error();
      auto exact = time.value().seconds(); if (!exact) return exact.error();
      auto evaluated = evaluate(*jobs,shared,samples); if (!evaluated) return evaluated;
      auto committed = sequence.value().commit(identity,true); if (!committed) return committed;
      ++samples;
    }
  } else {
    auto fixed = core::FixedStepper::create(core::Rational::make(1,60).value(),{4,8,core::OverloadAction::Pause});
    if (!fixed) return fixed.error();
    auto anchored = fixed.value().advance({0,99}); if (!anchored) return anchored.error();
    for (std::uint64_t frame = 1; frame <= 6; ++frame) {
      auto report = fixed.value().advance({frame*16666667,99}); if (!report) return report.error();
      for (std::uint64_t step = 0; step < report.value().steps; ++step) {
        auto evaluated = evaluate(*jobs,shared,report.value().first_tick + step); if (!evaluated) return evaluated;
      }
      ticks += report.value().steps;
    }
    if (ticks != 6) return fail(ErrorCode::Invariant,"fixed tick fixture mismatch");
    if (mode == "authority") {
      core::ConfigPatch local;
      auto set = local.set("authoritative",false); if (!set) return set;
      auto rejected = core::resolve_config(core::headless_schema(),project,{},local);
      if (rejected) return fail(ErrorCode::Invariant,"local authority override accepted");
    }
  }
  auto stopped = runtime.stop(5s); if (!stopped) return stopped;
  auto stale = shared.table.resolve(shared.handle); if (stale) return fail(ErrorCode::Invariant,"stale producer handle resolved");
  auto shutdown = jobs->shutdown(core::ShutdownMode::Drain,5s); if (!shutdown) return shutdown;
  auto snapshot = memory.snapshot();
  if (snapshot.total.live) return fail(ErrorCode::Invariant,"fixture allocation leaked");
  std::printf("{\"milestone\":\"first-linux-headless-core\",\"fixture\":\"%.*s\",\"ticks\":%llu,\"offline_samples\":%llu,\"completed_jobs\":%llu,\"tracked_peak_bytes\":%zu,\"live_bytes_after_stop\":%zu}\n",
    static_cast<int>(mode.size()),mode.data(),static_cast<unsigned long long>(ticks),static_cast<unsigned long long>(samples),static_cast<unsigned long long>(jobs->stats().completed),snapshot.total.peak,snapshot.total.live);
  return {};
}
}
int main(int argc,char** argv) {
  core::set_emergency_sink(platform::emergency_write);
  if (argc == 2 && std::string_view(argv[1]) == "--identity") {
    std::printf("{\"source_commit\":\"%s\",\"authored_input_sha256\":\"%s\",\"configuration\":\"%s\",\"sanitizer\":\"%s\"}\n",build::source_commit,build::authored_input_sha256,build::configuration,build::sanitizer);
    return 0;
  }
  std::string_view mode = "game";
  if (argc == 3 && std::string_view(argv[1]) == "--mode") mode = argv[2];
  else if (argc != 1) { std::fprintf(stderr,"usage: race_headless [--mode game|film|authority]\n"); return 2; }
  if (mode != "game" && mode != "film" && mode != "authority") return 2;
  try { auto result = run(mode); if (!result) { std::fprintf(stderr,"runtime failure %u: %s\n",static_cast<unsigned>(result.error().code),result.error().message); return 1; } }
  catch (...) { std::fprintf(stderr,"runtime composition failed before safe continuation\n"); return 1; }
}
