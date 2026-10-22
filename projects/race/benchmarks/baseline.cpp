#include "race/core/memory.hpp"
#include "race/build_identity.hpp"
#include "race/core/jobs.hpp"
#include "race/core/diagnostics.hpp"
#include "race/runtime/lifecycle.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <memory>
#include <new>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace {
using namespace race;
using namespace race::core;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;
constexpr std::size_t warmup = 2, trials = 9, batch_size = 128;
constexpr std::size_t allocation_batches = 64, job_batches = 8, diagnostic_operations = 100000;
constexpr std::uint64_t seed = 0x52414345;
std::uint64_t sink = 0;
std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
template<class T> T take(Result<T> result) {
  if (!result) throw std::runtime_error(result.error().message);
  return std::move(result).value();
}
void require(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
void verify(bool condition) { if (!condition) throw std::runtime_error("benchmark workload check failed"); }
struct Summary { double median, p95, minimum, maximum, mean, standard_deviation; };
Summary summarize(std::vector<double> values) {
  verify(!values.empty());
  std::sort(values.begin(), values.end());
  double sum = 0; for (auto value : values) sum += value;
  const double mean = sum / static_cast<double>(values.size());
  double variance = 0; for (auto value : values) variance += (value - mean) * (value - mean);
  return {values[values.size() / 2], values[static_cast<std::size_t>(std::ceil(values.size() * 0.95)) - 1],
          values.front(), values.back(), mean, std::sqrt(variance / static_cast<double>(values.size()))};
}
void distribution(std::string_view field, const std::vector<double>& values, bool raw) {
  const auto s = summarize(values);
  std::cout << ",\"" << field << "\":{\"median\":" << s.median << ",\"p95\":" << s.p95
            << ",\"min\":" << s.minimum << ",\"max\":" << s.maximum << ",\"mean\":" << s.mean
            << ",\"population_stddev\":" << s.standard_deviation;
  if (raw) {
    std::cout << ",\"raw\":[";
    for (std::size_t i = 0; i < values.size(); ++i) { if (i) std::cout << ','; std::cout << values[i]; }
    std::cout << ']';
  }
  std::cout << '}';
}
void begin_row(std::string_view name, std::size_t operations) {
  std::cout << "{\"schema\":1,\"source_commit\":\"" << build::source_commit << "\",\"authored_input_sha256\":\"" << build::authored_input_sha256 << "\",\"name\":\"" << name << "\",\"seed\":" << seed
            << ",\"warmup_trials\":" << warmup << ",\"measured_trials\":" << trials
            << ",\"operations_per_trial\":" << operations;
}
void memory_row(const BudgetSnapshot& snapshot) {
  std::cout << ",\"tracked_requested_bytes\":" << snapshot.total.requested
            << ",\"tracked_live_bytes\":" << snapshot.total.live
            << ",\"tracked_peak_bytes\":" << snapshot.total.peak
            << ",\"tracked_reserved_payload_bytes\":" << snapshot.total.reserved
            << ",\"tracked_committed_bytes\":null";
}
void resident_row() {
  auto memory = take(platform::memory_info());
  std::cout << ",\"process_resident_bytes_snapshot\":" << memory.resident_bytes
            << ",\"physical_bytes\":" << memory.physical_bytes;
}
void touch(void* pointer, std::uint64_t value) {
  auto* words = static_cast<volatile std::uint64_t*>(pointer);
  for (std::size_t j = 0; j < 8; ++j) words[j] = value + j;
  sink ^= words[7];
}
struct AlignedDelete {
  void operator()(std::byte* pointer) const noexcept { ::operator delete(pointer, std::align_val_t(64)); }
};
void allocation_bench(std::string_view mode) {
  std::vector<double> durations;
  auto allocator = take(TrackedAllocator::create(BudgetConfig{batch_size * 64 + 64}));
  std::unique_ptr<ScopedArena> arena;
  if (mode == "arena") arena = std::make_unique<ScopedArena>(take(ScopedArena::create(allocator, batch_size * 64 + 64)));
  std::array<void*, batch_size> pointers{};
  std::array<std::unique_ptr<std::byte, AlignedDelete>, batch_size> standard;
  std::array<OwnedBlock, batch_size> blocks;
  std::array<ArenaLease, batch_size> leases;
  for (std::size_t trial = 0; trial < warmup + trials; ++trial) {
    auto start = now_ns();
    for (std::size_t batch = 0; batch < allocation_batches; ++batch) {
      for (std::size_t i = 0; i < batch_size; ++i) {
        if (mode == "standard") { standard[i].reset(static_cast<std::byte*>(::operator new(64, std::align_val_t(64)))); pointers[i] = standard[i].get(); }
        else if (mode == "tracked") { blocks[i] = take(allocator.allocate({64, 64, 0, 0})); pointers[i] = blocks[i].data(); }
        else { leases[i] = take(arena->allocate(64, 64)); pointers[i] = leases[i].bytes().data(); }
        touch(pointers[i], seed + batch * batch_size + i);
      }
      // Every case retains the same 128 payloads until this batch boundary.
      for (std::size_t i = 0; i < batch_size; ++i) {
        if (mode == "standard") standard[i].reset();
        else if (mode == "tracked") blocks[i].reset();
        else leases[i].release();
      }
      if (arena) require(arena->reset());
    }
    const auto elapsed = static_cast<double>(now_ns() - start);
    if (trial >= warmup) durations.push_back(elapsed);
  }
  begin_row(mode == "standard" ? "allocation_standard" : mode == "tracked" ? "allocation_tracked" : "allocation_scoped_arena", allocation_batches * batch_size);
  std::cout << ",\"payload_bytes\":64,\"alignment\":64,\"lifetime_batch\":128,\"setup_in_timing\":false,\"payload_write_bytes\":64";
  distribution("trial_ns", durations, true); memory_row(allocator.snapshot()); resident_row();
  std::cout << "}\n";
}
std::uint64_t cpu_work(std::uint64_t value) {
  for (unsigned i = 0; i < 64; ++i) { value ^= value >> 12; value ^= value << 25; value ^= value >> 27; value *= 2685821657736338717ULL; }
  return value;
}
struct JobSample { std::uint64_t input = 0, output = 0, submitted = 0, started = 0, finished = 0; };
Result<void> benchmark_job(const JobContext& context) {
  auto& sample = *static_cast<JobSample*>(context.context());
  sample.started = now_ns(); sample.output = cpu_work(sample.input); sample.finished = now_ns(); return {};
}
void jobs_bench(platform::Threads& threads, std::size_t workers) {
  std::unique_ptr<Scheduler> scheduler;
  if (workers) scheduler = take(Scheduler::create(threads, {batch_size, workers, 2}));
  std::vector<double> durations, rates, start_latency, completion_latency;
  const auto count = job_batches * batch_size;
  auto samples = std::make_shared<std::vector<JobSample>>(count);
  std::array<JobDesc, batch_size> descriptors;
  for (std::size_t trial = 0; trial < warmup + trials; ++trial) {
    for (std::size_t i = 0; i < count; ++i) (*samples)[i] = {seed + i, 0, 0, 0, 0};
    const auto start = now_ns();
    for (std::size_t batch = 0; batch < job_batches; ++batch) {
      if (scheduler) {
        auto group = take(scheduler->create_group());
        for (std::size_t i = 0; i < batch_size; ++i) {
          auto& sample = (*samples)[batch * batch_size + i];
          descriptors[i] = JobDesc{benchmark_job, std::shared_ptr<void>(samples, &sample)};
          descriptors[i].unknown_access = false;
        }
        const auto submitted = now_ns();
        for (std::size_t i = 0; i < batch_size; ++i) (*samples)[batch * batch_size + i].submitted = submitted;
        auto handles = take(scheduler->submit_batch(group, descriptors));
        verify(handles.size() == batch_size);
        require(scheduler->close(group)); auto completion = take(scheduler->wait(group, 10s));
        verify(completion.succeeded == batch_size && completion.failed == 0 && completion.cancelled == 0);
        require(scheduler->release(group));
      } else {
        for (std::size_t i = 0; i < batch_size; ++i) {
          auto& sample = (*samples)[batch * batch_size + i];
          sample.submitted = now_ns(); sample.started = now_ns();
          sample.output = cpu_work(sample.input); sample.finished = now_ns();
        }
      }
    }
    const auto elapsed = static_cast<double>(now_ns() - start);
    if (trial >= warmup) {
      durations.push_back(elapsed); rates.push_back(static_cast<double>(count) * 1e9 / elapsed);
      for (const auto& sample : *samples) {
        verify(sample.output == cpu_work(sample.input)); sink ^= sample.output;
        start_latency.push_back(static_cast<double>(sample.started - sample.submitted));
        completion_latency.push_back(static_cast<double>(sample.finished - sample.submitted));
      }
    }
  }
  begin_row(workers ? "jobs_bounded_scheduler" : "jobs_inline_serial", count);
  std::cout << ",\"workers\":" << workers << ",\"capacity\":" << (workers ? batch_size : 0)
            << ",\"batch\":128,\"cpu_mix_rounds\":64,\"scheduler_setup_in_timing\":false,\"latency_clock_calls_enabled\":true"
            << ",\"completion_latency_definition\":\"timestamp before batch timestamp assignment and submit_batch to callback finish; excludes terminal bookkeeping\"";
  distribution("trial_ns", durations, true); distribution("jobs_per_second", rates, true);
  distribution("submit_to_callback_start_ns", start_latency, false); distribution("submit_to_callback_finish_ns", completion_latency, false);
  if (scheduler) {
    auto stats = scheduler->stats();
    std::cout << ",\"peak_pending_jobs\":" << stats.peak_pending << ",\"completed_jobs_including_warmup\":" << stats.completed;
    require(scheduler->shutdown(ShutdownMode::Drain, 10s));
  }
  resident_row(); std::cout << "}\n";
}
void diagnostics_bench(platform::Clock& clock, bool capture) {
  Diagnostics diagnostics(clock); diagnostics.enable_detailed_capture(capture);
  std::vector<double> durations;
  for (std::size_t trial = 0; trial < warmup + trials; ++trial) {
    auto start = now_ns();
    for (std::size_t i = 0; i < diagnostic_operations; ++i) {
      TimingScope timing(diagnostics, clock); diagnostics.count_operation();
      sink ^= (seed + i) * 2654435761ULL;
    }
    const auto elapsed = static_cast<double>(now_ns() - start);
    if (trial >= warmup) durations.push_back(elapsed);
  }
  auto counters = diagnostics.counters();
  verify(counters.operations == diagnostic_operations * (warmup + trials));
  verify(counters.timing_samples == (capture ? counters.operations : 0));
  begin_row(capture ? "diagnostics_capture_on" : "diagnostics_capture_off", diagnostic_operations);
  distribution("trial_ns", durations, true);
  std::cout << ",\"timing_samples_including_warmup\":" << counters.timing_samples
            << ",\"diagnostic_timing_errors\":" << counters.timing_errors;
  resident_row(); std::cout << "}\n";
}
struct LifecyclePayload {
  OwnedBlock block;
  std::shared_ptr<LifecyclePayload> predecessor;
  std::uint64_t value = 0;
};
struct ShutdownOrder {
  std::array<unsigned, 2> values{};
  std::size_t count = 0;
};
class BenchmarkModule final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_;
  BenchmarkModule* predecessor_;
  ShutdownOrder& order_;
  unsigned index_;
  runtime::ModuleContext services_;
  std::shared_ptr<LifecyclePayload> payload_;
  JobHandle job_;
public:
  BenchmarkModule(std::string id, BenchmarkModule* predecessor, ShutdownOrder& order, unsigned index)
      : predecessor_(predecessor), order_(order), index_(index) {
    descriptor_.id = std::move(id);
    descriptor_.services = runtime::Memory | runtime::Jobs;
    if (predecessor_) descriptor_.dependencies.push_back(predecessor_->descriptor().id);
  }
  const runtime::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
  Result<void> validate() noexcept override { return {}; }
  Result<void> initialize(runtime::ModuleContext services) noexcept override {
    services_ = services;
    auto block = services_.memory->allocate({64, 64, index_, 0});
    if (!block) return block.error();
    try {
      payload_ = std::make_shared<LifecyclePayload>();
      payload_->block = std::move(block).value();
      if (predecessor_) payload_->predecessor = predecessor_->payload_;
      return {};
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "benchmark payload allocation"); }
  }
  Result<void> start() noexcept override {
    JobDesc desc{[](const JobContext& context) -> Result<void> {
      auto& payload = *static_cast<LifecyclePayload*>(context.context());
      payload.value = cpu_work(payload.predecessor ? payload.predecessor->value : seed);
      for (std::size_t i = 0; i < payload.block.size(); ++i)
        payload.block.data()[i] = static_cast<std::byte>((payload.value >> ((i % 8) * 8)) & 255);
      return {};
    }, payload_};
    std::array<JobHandle, 1> prerequisite;
    if (predecessor_) { prerequisite[0] = predecessor_->job_; desc.prerequisites = prerequisite; }
    desc.unknown_access = false;
    auto submitted = services_.jobs->submit(services_.task_group, desc);
    if (!submitted) return submitted.error();
    job_ = submitted.value();
    return services_.jobs->close(services_.task_group);
  }
  Result<GroupCompletion> wait_startup() {
    return services_.jobs->wait(services_.task_group, 5s);
  }
  std::uint64_t value() const { return payload_->value; } // Only after wait_startup.
  void request_stop() noexcept override {}
  Result<void> quiesce(std::chrono::milliseconds) noexcept override { return {}; }
  void shutdown() noexcept override {
    if (order_.count >= order_.values.size()) std::abort();
    order_.values[order_.count++] = index_;
    payload_.reset();
  }
  void rollback_initialization() noexcept override { payload_.reset(); }
};
void populated_lifecycle_bench(platform::Services& services) {
  std::vector<double> startup, work_wait, start_to_ready, shutdown;
  auto allocator = take(TrackedAllocator::create(BudgetConfig{1024}));
  Diagnostics diagnostics(*services.clock);
  for (std::size_t trial = 0; trial < warmup + trials; ++trial) {
    auto scheduler = take(Scheduler::create(*services.threads, {128, 2, 2}));
    ShutdownOrder order;
    runtime::Runtime runtime(allocator, *scheduler, *services.clock, diagnostics);
    auto producer = std::make_unique<BenchmarkModule>("benchmark.producer", nullptr, order, 0);
    auto* producer_view = producer.get();
    auto consumer = std::make_unique<BenchmarkModule>("benchmark.consumer", producer_view, order, 1);
    auto* consumer_view = consumer.get();
    require(runtime.add(std::move(consumer))); // Graph ordering, not insertion ordering.
    require(runtime.add(std::move(producer)));
    const auto started = now_ns();
    require(runtime.start());
    const auto started_modules = now_ns();
    const auto producer_done = take(producer_view->wait_startup());
    const auto consumer_done = take(consumer_view->wait_startup());
    const auto ready = now_ns();
    verify(producer_done.succeeded == 1 && producer_done.failed == 0 && producer_done.cancelled == 0);
    verify(consumer_done.succeeded == 1 && consumer_done.failed == 0 && consumer_done.cancelled == 0);
    verify(producer_view->value() == cpu_work(seed));
    verify(consumer_view->value() == cpu_work(cpu_work(seed)));
    sink ^= consumer_view->value();
    verify(allocator.snapshot().total.live == 128);
    const auto stopping = now_ns();
    require(runtime.stop(5s));
    const auto stopped = now_ns();
    verify(order.count == 2 && order.values[0] == 1 && order.values[1] == 0);
    verify(allocator.snapshot().total.live == 0 && scheduler->stats().occupied == 0);
    require(scheduler->shutdown(ShutdownMode::Drain, 5s));
    if (trial >= warmup) {
      startup.push_back(static_cast<double>(started_modules - started));
      work_wait.push_back(static_cast<double>(ready - started_modules));
      start_to_ready.push_back(static_cast<double>(ready - started));
      shutdown.push_back(static_cast<double>(stopped - stopping));
    }
  }
  begin_row("lifecycle_producer_consumer_startup_jobs", 1);
  std::cout << ",\"modules\":2,\"dependency_edges\":1,\"startup_jobs\":2,\"job_dependency_edges\":1"
            << ",\"tracked_bytes_per_module\":64,\"allocation_alignment\":64,\"payload_write_bytes_per_job\":64"
            << ",\"cpu_mix_rounds_per_job\":64,\"workers\":2,\"capacity\":128,\"runtime_and_module_construction_in_timing\":false"
            << ",\"scheduler_create_and_join_in_timing\":false,\"shutdown_order_verified\":\"consumer then producer\""
            << ",\"startup_wait_definition\":\"wait for both module-owned startup jobs after Runtime.start returns; successful completion required\"";
  distribution("runtime_start_ns", startup, true);
  distribution("startup_jobs_wait_after_start_ns", work_wait, true);
  distribution("runtime_start_to_successful_jobs_ready_ns", start_to_ready, true);
  distribution("runtime_stop_and_release_resources_ns", shutdown, true);
  memory_row(allocator.snapshot()); resident_row(); std::cout << "}\n";
}
void lifecycle_bench(platform::Services& services) {
  std::vector<double> startup, shutdown, scheduler_create, scheduler_stop;
  auto allocator = take(TrackedAllocator::create(BudgetConfig{1024 * 1024}));
  Diagnostics diagnostics(*services.clock);
  for (std::size_t trial = 0; trial < warmup + trials; ++trial) {
    auto begin = now_ns(); auto scheduler = take(Scheduler::create(*services.threads, {128, 2, 2}));
    auto create_ns = static_cast<double>(now_ns() - begin);
    runtime::Runtime runtime(allocator, *scheduler, *services.clock, diagnostics);
    begin = now_ns(); require(runtime.start()); auto start_ns = static_cast<double>(now_ns() - begin);
    begin = now_ns(); require(runtime.stop(5s)); auto stop_ns = static_cast<double>(now_ns() - begin);
    begin = now_ns(); require(scheduler->shutdown(ShutdownMode::Drain, 5s)); auto join_ns = static_cast<double>(now_ns() - begin);
    if (trial >= warmup) { startup.push_back(start_ns); shutdown.push_back(stop_ns); scheduler_create.push_back(create_ns); scheduler_stop.push_back(join_ns); }
  }
  begin_row("lifecycle_empty_runtime_and_two_worker_scheduler", 1);
  std::cout << ",\"modules\":0,\"workers\":2,\"capacity\":128,\"runtime_construction_in_startup_timing\":false";
  distribution("runtime_start_ns", startup, true); distribution("runtime_stop_ns", shutdown, true);
  distribution("scheduler_create_ns", scheduler_create, true); distribution("scheduler_quiesce_and_join_ns", scheduler_stop, true);
  memory_row(allocator.snapshot()); resident_row(); std::cout << "}\n";
}
}
int main() {
  try {
    std::cout.precision(12);
    auto services = take(race::platform::make_services());
    allocation_bench("standard"); allocation_bench("tracked"); allocation_bench("arena");
    jobs_bench(*services.threads, 0); jobs_bench(*services.threads, 1); jobs_bench(*services.threads, 2); jobs_bench(*services.threads, 4);
    diagnostics_bench(*services.clock, false); diagnostics_bench(*services.clock, true);
    lifecycle_bench(services); populated_lifecycle_bench(services);
    std::cout << "{\"schema\":1,\"name\":\"workload_checksum\",\"value\":" << sink << "}\n";
    return 0;
  } catch (const std::exception& error) { std::fprintf(stderr, "benchmark failed: %s\n", error.what()); return 1; }
}
