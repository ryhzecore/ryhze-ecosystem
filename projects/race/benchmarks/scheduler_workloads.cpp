#include "race/build_identity.hpp"
#include "race/core/jobs.hpp"
#include "race/platform/platform.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

// Synthetic CPU workloads, composed with the Linux adapter. This is a measuring
// tool, not an engine module, throughput guarantee, or performance assertion.
namespace {
using namespace race;
using namespace race::core;
using Clock = std::chrono::steady_clock;
constexpr std::uint64_t seed = 0x52414345;
constexpr std::uint64_t golden = 0x9e3779b97f4a7c15ULL;
constexpr auto timeout = std::chrono::minutes(2);

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count());
}
void check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
template<class T> T take(Result<T> result) {
  if (!result) throw std::runtime_error(result.error().message);
  return std::move(result).value();
}
void require(Result<void> result) {
  if (!result) throw std::runtime_error(result.error().message);
}
struct Options {
  std::string kernel = "integer", pattern = "independent", mode = "paired";
  std::size_t work = 1024, jobs = 1024, batch = 64, capacity = 64, workers = 2;
  std::size_t warmups = 2, trials = 7;
  bool latency = false;
};
std::size_t number(std::string_view text) {
  std::size_t value = 0;
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  check(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(), "invalid unsigned integer argument");
  return value;
}
Options parse(int argc, char** argv) {
  Options o;
  std::set<std::string_view> seen;
  for (int i = 1; i < argc; i += 2) {
    const std::string_view name = argv[i];
    check(i + 1 < argc, "option requires a value");
    check(seen.insert(name).second, "duplicate option");
    const std::string_view value = argv[i + 1];
    if (name == "--kernel") o.kernel = value;
    else if (name == "--pattern") o.pattern = value;
    else if (name == "--mode") o.mode = value;
    else if (name == "--work") o.work = number(value);
    else if (name == "--jobs") o.jobs = number(value);
    else if (name == "--batch") o.batch = number(value);
    else if (name == "--capacity") o.capacity = number(value);
    else if (name == "--workers") o.workers = number(value);
    else if (name == "--warmups") o.warmups = number(value);
    else if (name == "--trials") o.trials = number(value);
    else if (name == "--latency") { check(value == "0" || value == "1", "latency must be 0 or 1"); o.latency = value == "1"; }
    else throw std::runtime_error("unknown option");
  }
  check(o.kernel == "integer" || o.kernel == "buffer", "kernel must be integer or buffer");
  check(o.pattern == "independent" || o.pattern == "chain" || o.pattern == "fork_join" || o.pattern == "conflict", "unknown dependency pattern");
  check(o.mode == "paired" || o.mode == "inline" || o.mode == "scheduled", "mode must be paired, inline or scheduled");
  check(o.work > 0 && o.work <= 16777216, "work must be in 1..16777216");
  check(o.jobs > 0 && o.jobs <= 1048576, "jobs must be in 1..1048576");
  check(o.batch > 0 && o.batch <= 65536 && o.batch <= o.jobs, "batch must be in 1..min(jobs,65536)");
  check(o.capacity >= o.batch && o.capacity <= 65536, "capacity must be in batch..65536");
  check(o.workers > 0 && o.workers <= 256, "workers must be in 1..256");
  check(o.warmups <= 100 && o.trials > 0 && o.trials <= 100, "warmups must be 0..100 and trials 1..100");
  check(o.pattern != "fork_join" || o.batch >= 3, "fork_join needs batch >= 3");
  check(o.kernel != "buffer" || o.batch <= (512ULL * 1024 * 1024) / (o.work * sizeof(std::uint64_t)), "active buffer payload exceeds 512 MiB bound");
  const auto latency_modes = o.mode == "paired" ? 2ULL : 1ULL;
  check(!o.latency || o.jobs <= (256ULL * 1024 * 1024) / ((o.warmups + o.trials) * 3 * sizeof(double) * latency_modes), "retained latency samples exceed 256 MiB bound");
  return o;
}
struct Dependencies {
  std::array<std::size_t, Scheduler::max_dependencies> indices{};
  std::size_t count = 0;
  void add(std::size_t index) { check(count < indices.size(), "dependency bound exceeded"); indices[count++] = index; }
};
using Graph = std::vector<Dependencies>;
Graph graph_for(const Options& o, std::size_t count) {
  Graph graph(count);
  if (o.pattern == "chain") {
    for (std::size_t i = 1; i < count; ++i) graph[i].add(i - 1);
  } else if (o.pattern == "fork_join") {
    // Each full diamond has one root, sixteen leaves and one join. A short
    // tail forms a smaller diamond, or a singleton/two-node chain.
    for (std::size_t root = 0; root < count;) {
      const auto length = std::min(count - root, Scheduler::max_dependencies + 2);
      if (length == 2) graph[root + 1].add(root);
      else if (length > 2) {
        for (std::size_t i = root + 1; i < root + length - 1; ++i) {
          graph[i].add(root);
          graph[root + length - 1].add(i);
        }
      }
      root += length;
    }
  }
  return graph;
}
std::uint64_t job_seed(std::size_t index) { return seed ^ (static_cast<std::uint64_t>(index) * golden); }
std::uint64_t integer_kernel(std::uint64_t value, std::size_t rounds) {
  for (std::size_t i = 0; i < rounds; ++i) {
    value ^= value >> 12; value ^= value << 25; value ^= value >> 27;
    value *= 2685821657736338717ULL;
  }
  return value;
}
std::uint64_t initial_element(std::uint64_t initial, std::size_t i) {
  return initial ^ (static_cast<std::uint64_t>(i + 1) * golden);
}
std::uint64_t transformed_element(std::uint64_t value, std::uint64_t input) {
  return std::rotl(value ^ input, 17) * 0xd6e8feb86659fd93ULL + 0xa5a5a5a5a5a5a5a5ULL;
}
std::uint64_t fold_dependency(std::uint64_t input, std::uint64_t output, std::size_t ordinal) {
  return std::rotl(input ^ output, static_cast<int>(ordinal + 1)) * golden;
}
struct Reference { std::vector<std::uint64_t> inputs, outputs; };
Reference make_reference(const Options& o, const Graph& full, const Graph& tail) {
  Reference reference{std::vector<std::uint64_t>(o.jobs), std::vector<std::uint64_t>(o.jobs)};
  for (std::size_t base = 0; base < o.jobs; base += o.batch) {
    const auto count = std::min(o.batch, o.jobs - base);
    const auto& graph = count == o.batch ? full : tail;
    for (std::size_t i = 0; i < count; ++i) {
      auto input = job_seed(base + i);
      for (std::size_t d = 0; d < graph[i].count; ++d)
        input = fold_dependency(input, reference.outputs[base + graph[i].indices[d]], d);
      reference.inputs[base + i] = input;
      if (o.kernel == "integer") reference.outputs[base + i] = integer_kernel(input, o.work);
      else {
        std::uint64_t sum = 0;
        for (std::size_t element = 0; element < o.work; ++element)
          sum += transformed_element(initial_element(job_seed(base + i), element), input) ^ (static_cast<std::uint64_t>(element) * golden);
        reference.outputs[base + i] = sum;
      }
    }
  }
  return reference;
}
struct Sample {
  std::uint64_t initial = 0, output = 0, submitted = 0, started = 0, finished = 0;
  std::size_t work = 0, executions = 0;
  bool buffer_kernel = false, latency = false;
  const Dependencies* dependencies = nullptr;
  Sample* siblings = nullptr;
  std::uint64_t* serialized_counter = nullptr;
  std::vector<std::uint64_t> buffer;
};
struct Batch { std::vector<Sample> samples; std::uint64_t serialized_counter = 0; };
Result<void> workload(const JobContext& context) {
  auto& sample = *static_cast<Sample*>(context.context());
  if (sample.latency) sample.started = now_ns();
  auto input = sample.initial;
  for (std::size_t d = 0; d < sample.dependencies->count; ++d)
    input = fold_dependency(input, sample.siblings[sample.dependencies->indices[d]].output, d);
  if (!sample.buffer_kernel) sample.output = integer_kernel(input, sample.work);
  else {
    std::uint64_t sum = 0;
    for (std::size_t i = 0; i < sample.buffer.size(); ++i) {
      sample.buffer[i] = transformed_element(sample.buffer[i], input);
      sum += sample.buffer[i] ^ (static_cast<std::uint64_t>(i) * golden);
    }
    sample.output = sum;
  }
  if (sample.serialized_counter) ++*sample.serialized_counter;
  ++sample.executions;
  if (sample.latency) sample.finished = now_ns();
  return {};
}
struct Memory { std::uint64_t resident = 0, peak = 0; };
Memory memory() {
#ifdef _WIN32
  const auto info = take(platform::memory_info());
  return {info.resident_bytes, info.peak_resident_bytes};
#else
  std::ifstream status("/proc/self/status");
  check(status.good(), "cannot read Linux process memory observations");
  Memory result;
  for (std::string line; std::getline(status, line);) {
    std::istringstream fields(line);
    std::string key, unit;
    std::uint64_t value = 0;
    if (fields >> key >> value >> unit) {
      if (key == "VmRSS:" && unit == "kB") result.resident = value * 1024;
      if (key == "VmHWM:" && unit == "kB") result.peak = value * 1024;
    }
  }
  check(result.resident > 0 && result.peak > 0, "Linux RSS fields unavailable");
  return result;
#endif
}
struct Trial {
  std::uint64_t setup = 0, group_create = 0, admission = 0, close = 0, wait = 0;
  std::uint64_t dispatch_wait = 0, verify = 0, release = 0, total = 0, checksum = 0;
  std::uint64_t callbacks = 0;
  std::vector<double> queue_latency, callback_latency, finish_latency;
  Memory process_memory;
};
Trial run_trial(const Options& o, const Graph& full, const Graph& tail, const Reference& reference, Scheduler* scheduler) {
  Trial trial;
  const auto total_start = now_ns();
  if (o.latency) {
    trial.queue_latency.reserve(o.jobs); trial.callback_latency.reserve(o.jobs); trial.finish_latency.reserve(o.jobs);
  }
  const auto stats_before = scheduler ? scheduler->stats() : SchedulerStats{};
  for (std::size_t base = 0; base < o.jobs; base += o.batch) {
    const auto count = std::min(o.batch, o.jobs - base);
    const auto& graph = count == o.batch ? full : tail;
    auto phase = now_ns();
    auto owner = std::make_shared<Batch>();
    owner->samples.resize(count);
    std::vector<JobDesc> descriptors(count);
    const ResourceAccess conflict{1, AccessMode::Write};
    for (std::size_t i = 0; i < count; ++i) {
      auto& sample = owner->samples[i];
      sample.initial = job_seed(base + i); sample.work = o.work;
      sample.buffer_kernel = o.kernel == "buffer"; sample.latency = o.latency;
      sample.dependencies = &graph[i]; sample.siblings = owner->samples.data();
      if (o.pattern == "conflict") sample.serialized_counter = &owner->serialized_counter;
      if (sample.buffer_kernel) {
        sample.buffer.resize(o.work);
        for (std::size_t element = 0; element < o.work; ++element)
          sample.buffer[element] = initial_element(sample.initial, element);
      }
      descriptors[i] = JobDesc{workload, std::shared_ptr<void>(owner, &sample)};
      descriptors[i].unknown_access = false;
      descriptors[i].batch_prerequisites = std::span(graph[i].indices.data(), graph[i].count);
      if (o.pattern == "conflict") descriptors[i].accesses = std::span(&conflict, 1);
    }
    trial.setup += now_ns() - phase;
    GroupHandle group;
    if (scheduler) { phase = now_ns(); group = take(scheduler->create_group()); trial.group_create += now_ns() - phase; }
    std::vector<JobHandle> handles;
    const auto dispatch_start = now_ns();
    // The public API does not expose atomic-admission or terminal-bookkeeping
    // timestamps. This marker precedes timestamp assignment and submit_batch.
    if (o.latency) for (auto& sample : owner->samples) sample.submitted = dispatch_start;
    if (scheduler) {
      phase = now_ns(); handles = take(scheduler->submit_batch(group, descriptors)); trial.admission += now_ns() - phase;
      phase = now_ns(); require(scheduler->close(group)); trial.close += now_ns() - phase;
      phase = now_ns(); const auto completion = take(scheduler->wait(group, timeout)); trial.wait += now_ns() - phase;
      trial.dispatch_wait += now_ns() - dispatch_start;
      check(completion.succeeded == count && completion.failed == 0 && completion.cancelled == 0, "group did not complete all jobs successfully");
    } else {
      std::atomic<bool> cancellation{false};
      for (auto& sample : owner->samples) require(workload(JobContext(&sample, &cancellation)));
      trial.dispatch_wait += now_ns() - dispatch_start;
    }
    phase = now_ns();
    if (scheduler) {
      check(handles.size() == count, "admitted handle count mismatch");
      for (const auto handle : handles) {
        const auto completion = take(scheduler->completion(handle));
        check(completion.terminal() && completion.state == JobState::Succeeded, "job did not finish successfully");
      }
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto& sample = owner->samples[i];
      check(sample.executions == 1 && sample.output == reference.outputs[base + i], "job result or execution count mismatch");
      if (sample.buffer_kernel) {
        for (std::size_t element = 0; element < sample.buffer.size(); ++element)
          check(sample.buffer[element] == transformed_element(initial_element(sample.initial, element), reference.inputs[base + i]), "buffer element mismatch");
      }
      trial.checksum ^= std::rotl(sample.output, static_cast<int>((base + i) % 64));
      ++trial.callbacks;
      if (o.latency) {
        check(sample.started >= sample.submitted && sample.finished >= sample.started, "invalid callback timestamps");
        trial.queue_latency.push_back(static_cast<double>(sample.started - sample.submitted));
        trial.callback_latency.push_back(static_cast<double>(sample.finished - sample.started));
        trial.finish_latency.push_back(static_cast<double>(sample.finished - sample.submitted));
      }
    }
    check(o.pattern != "conflict" || owner->serialized_counter == count, "serialized counter mismatch");
    trial.verify += now_ns() - phase;
    if (scheduler) {
      phase = now_ns(); require(scheduler->release(group)); trial.release += now_ns() - phase;
      const auto stats = scheduler->stats();
      check(stats.occupied == 0 && stats.pending == 0 && stats.running == 0, "released scheduler retains work");
    }
    // All aliases, transitive borrows and buffers remain alive until release.
  }
  trial.total = now_ns() - total_start;
  check(trial.callbacks == o.jobs, "trial callback count mismatch");
  if (scheduler) {
    const auto after = scheduler->stats();
    check(after.admitted - stats_before.admitted == o.jobs && after.completed - stats_before.completed == o.jobs, "scheduler trial statistics mismatch");
  }
  trial.process_memory = memory();
  return trial;
}
void distribution(const std::vector<double>& input) {
  check(!input.empty(), "empty measured distribution");
  auto values = input;
  std::sort(values.begin(), values.end());
  double sum = 0; for (const auto value : values) sum += value;
  const double mean = sum / static_cast<double>(values.size());
  double variance = 0; for (const auto value : values) variance += (value - mean) * (value - mean);
  const auto percentile = [&](double p) { return values[static_cast<std::size_t>(std::ceil(p * static_cast<double>(values.size()))) - 1]; };
  std::cout << "{\"count\":" << values.size() << ",\"min\":" << values.front() << ",\"p50\":" << percentile(0.50)
            << ",\"p95\":" << percentile(0.95) << ",\"p99\":" << percentile(0.99) << ",\"max\":" << values.back()
            << ",\"mean\":" << mean << ",\"population_stddev\":" << std::sqrt(variance / static_cast<double>(values.size())) << '}';
}
void print_trial(const Trial& t, const Options& o) {
  std::cout << "{\"setup_ns\":" << t.setup << ",\"group_create_ns\":" << t.group_create << ",\"admission_ns\":" << t.admission
            << ",\"close_ns\":" << t.close << ",\"wait_ns\":" << t.wait << ",\"dispatch_wait_ns\":" << t.dispatch_wait
            << ",\"verification_ns\":" << t.verify << ",\"release_ns\":" << t.release
            << ",\"group_cycle_ns\":" << t.group_create + t.dispatch_wait + t.release << ",\"total_ns\":" << t.total
            << ",\"dispatch_jobs_per_second\":" << static_cast<double>(o.jobs) * 1e9 / static_cast<double>(t.dispatch_wait)
            << ",\"jobs_per_second\":" << static_cast<double>(o.jobs) * 1e9 / static_cast<double>(t.group_create + t.dispatch_wait + t.release)
            << ",\"verified_callbacks\":" << t.callbacks << ",\"output_checksum\":" << t.checksum
            << ",\"process_resident_bytes\":" << t.process_memory.resident << ",\"process_peak_rss_bytes\":" << t.process_memory.peak
            << ",\"process_memory_counters_coherent\":" << (t.process_memory.peak>=t.process_memory.resident?"true":"false");
  if (o.latency) {
    std::cout << ",\"batch_marker_to_callback_start_ns\":"; distribution(t.queue_latency);
    std::cout << ",\"callback_duration_ns\":"; distribution(t.callback_latency);
    std::cout << ",\"batch_marker_to_callback_finish_ns\":"; distribution(t.finish_latency);
  }
  std::cout << '}';
}
void print_summary(const std::vector<Trial>& trials, const Options& o) {
  std::vector<double> dispatch, cycle, throughput, cycle_throughput, total;
  for (std::size_t i = o.warmups; i < trials.size(); ++i) {
    const auto& t = trials[i];
    dispatch.push_back(static_cast<double>(t.dispatch_wait));
    cycle.push_back(static_cast<double>(t.group_create + t.dispatch_wait + t.release));
    throughput.push_back(static_cast<double>(o.jobs) * 1e9 / dispatch.back());
    cycle_throughput.push_back(static_cast<double>(o.jobs) * 1e9 / cycle.back());
    total.push_back(static_cast<double>(t.total));
  }
  std::cout << "{\"dispatch_wait_ns\":"; distribution(dispatch);
  std::cout << ",\"group_cycle_ns\":"; distribution(cycle);
  std::cout << ",\"dispatch_jobs_per_second\":"; distribution(throughput);
  std::cout << ",\"jobs_per_second\":"; distribution(cycle_throughput);
  std::cout << ",\"total_ns\":"; distribution(total); std::cout << '}';
}
void run(const Options& o) {
  const auto reference_start = now_ns();
  const auto full = graph_for(o, o.batch);
  const auto tail = graph_for(o, o.jobs % o.batch);
  const auto reference = make_reference(o, full, tail);
  const auto reference_ns = now_ns() - reference_start;
  auto services = take(platform::make_services());
  const auto creation_start = now_ns();
  auto scheduler = o.mode == "inline" ? std::unique_ptr<Scheduler>{} : take(Scheduler::create(*services.threads, {o.capacity, o.workers, 1}));
  const auto creation_ns = now_ns() - creation_start;
  std::vector<Trial> inline_trials, scheduled_trials;
  inline_trials.reserve(o.warmups + o.trials); scheduled_trials.reserve(o.warmups + o.trials);
  for (std::size_t trial = 0; trial < o.warmups + o.trials; ++trial) {
    const auto inline_run = [&] { if (o.mode != "scheduled") inline_trials.push_back(run_trial(o, full, tail, reference, nullptr)); };
    const auto scheduled_run = [&] { if (scheduler) scheduled_trials.push_back(run_trial(o, full, tail, reference, scheduler.get())); };
    if (trial % 2 == 0) { inline_run(); scheduled_run(); }
    else { scheduled_run(); inline_run(); }
    if (o.mode == "paired") check(inline_trials.back().checksum == scheduled_trials.back().checksum, "paired checksum mismatch");
  }
  const auto stats = scheduler ? scheduler->stats() : SchedulerStats{};
  check(!scheduler || (stats.admitted == (o.warmups + o.trials) * o.jobs && stats.completed == stats.admitted && stats.occupied == 0 && stats.pending == 0 && stats.running == 0), "final scheduler statistics mismatch");
  const auto shutdown_start = now_ns();
  if (scheduler) require(scheduler->shutdown(ShutdownMode::Drain, timeout));
  const auto shutdown_ns = now_ns() - shutdown_start;
  const auto final_memory = memory();
  std::cout << std::setprecision(12)
            << "{\"schema\":1,\"kind\":\"race-scheduler-workloads\",\"source_commit\":\"" << build::source_commit
            << "\",\"authored_input_sha256\":\"" << build::authored_input_sha256 << "\",\"configuration\":\"" << build::configuration
            << "\",\"sanitizer\":\"" << build::sanitizer << "\",\"compiler\":\"" << build::compiler_version
            << "\",\"seed\":" << seed << ",\"kernel\":\"" << o.kernel << "\",\"pattern\":\"" << o.pattern
            << "\",\"mode\":\"" << o.mode << "\",\"work\":" << o.work << ",\"work_unit\":\"" << (o.kernel == "integer" ? "mix_rounds" : "uint64_elements")
            << "\",\"jobs\":" << o.jobs << ",\"batch\":" << o.batch << ",\"capacity\":" << o.capacity << ",\"workers\":" << o.workers
            << ",\"warmups\":" << o.warmups << ",\"measured_trials\":" << o.trials << ",\"latency_enabled\":" << (o.latency ? "true" : "false")
            << ",\"hardware_concurrency_hint\":" << std::thread::hardware_concurrency()
            << ",\"active_buffer_payload_bytes\":" << (o.kernel == "buffer" ? o.batch * o.work * sizeof(std::uint64_t) : 0)
            << ",\"reference_preparation_ns\":" << reference_ns << ",\"scheduler_creation_ns\":" << creation_ns << ",\"scheduler_shutdown_ns\":" << shutdown_ns
            << ",\"timing_notes\":{\"dispatch_wait\":\"Sum per batch: marker before submit through successful group wait, or identical inline callback loop; excludes setup, group creation, verification and release\","
               "\"group_cycle\":\"Group creation plus dispatch_wait plus release; excludes setup and verification\","
               "\"verification_effect\":\"Excluded per-handle correctness queries run between wait and release and may warm slot caches; group_cycle is a sum of measured phases, not contiguous wall latency\","
               "\"primary_throughput\":\"jobs_per_second and paired_speedup use complete group_cycle, including mandatory release; dispatch-only results are secondary\","
               "\"total\":\"Trial wall includes context and buffer setup, graph metadata, all phases, output verification and context destruction; excludes reusable reference preparation and scheduler creation/shutdown\","
               "\"latency\":\"Batch marker precedes timestamp assignment and submit; callback timestamps omit terminal bookkeeping; actual atomic admission timestamp unavailable; inline shares batch marker\","
               "\"memory\":\"Raw OS resident/peak (Linux RSS/VmHWM or Windows working set) includes both modes, reference, retained samples and runtime; peak is cumulative, not scheduler-only. Counters are unmodified; coherence explicitly reports peak>=resident. Incoherent pairs cannot certify a memory bound\","
               "\"dependencies\":\"Graphs restart each batch; callbacks consume prerequisite results; fork_join uses disjoint diamonds of up to 16 leaves, with smaller tails\","
               "\"clock_overhead\":\"Phase clocks remain in all runs; only latency-enabled callbacks use per-job clocks\"},\"trials\":[";
  for (std::size_t trial = 0; trial < o.warmups + o.trials; ++trial) {
    if (trial) std::cout << ',';
    std::cout << "{\"index\":" << trial << ",\"warmup\":" << (trial < o.warmups ? "true" : "false")
              << ",\"first_mode\":\"" << (o.mode == "paired" ? (trial % 2 == 0 ? "inline" : "scheduled") : o.mode) << '"';
    if (!inline_trials.empty()) { std::cout << ",\"inline\":"; print_trial(inline_trials[trial], o); }
    if (!scheduled_trials.empty()) { std::cout << ",\"scheduled\":"; print_trial(scheduled_trials[trial], o); }
    std::cout << '}';
  }
  std::cout << ']';
  if (!inline_trials.empty()) { std::cout << ",\"inline_summary\":"; print_summary(inline_trials, o); }
  if (!scheduled_trials.empty()) { std::cout << ",\"scheduled_summary\":"; print_summary(scheduled_trials, o); }
  if (o.mode == "paired") {
    std::vector<double> speedup, cycle_speedup;
    for (std::size_t i = o.warmups; i < inline_trials.size(); ++i) {
      speedup.push_back(static_cast<double>(inline_trials[i].dispatch_wait) / static_cast<double>(scheduled_trials[i].dispatch_wait));
      cycle_speedup.push_back(static_cast<double>(inline_trials[i].dispatch_wait) / static_cast<double>(scheduled_trials[i].group_create + scheduled_trials[i].dispatch_wait + scheduled_trials[i].release));
    }
    std::cout << ",\"paired_dispatch_speedup\":"; distribution(speedup);
    std::cout << ",\"paired_speedup\":"; distribution(cycle_speedup);
    std::cout << ",\"paired_group_cycle_speedup\":"; distribution(cycle_speedup);
  }
  std::cout << ",\"scheduler_stats\":{\"admitted\":" << stats.admitted << ",\"completed\":" << stats.completed
            << ",\"occupied\":" << stats.occupied << ",\"pending\":" << stats.pending << ",\"running\":" << stats.running
            << ",\"peak_pending_including_warmups\":" << stats.peak_pending << "},\"final_process_resident_bytes\":" << final_memory.resident
            << ",\"final_process_peak_rss_bytes\":" << final_memory.peak
            << ",\"final_process_memory_counters_coherent\":" << (final_memory.peak>=final_memory.resident?"true":"false")
            << ",\"all_outputs_verified\":true}\n";
}
}
int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--help") {
    std::cout << "Synthetic RACE scheduler CPU workloads, JSONL output\n"
                 "--kernel integer|buffer --work N --jobs N --batch N --capacity N\n"
                 "--workers N --pattern independent|chain|fork_join|conflict\n"
                 "--mode paired|inline|scheduled --warmups N --trials N --latency 0|1\n"
                 "Graphs are per batch; fork_join is disjoint diamonds with <=16 leaves.\n"
                 "work is integer mix rounds or uint64 buffer elements per job.\n"
                 "Primary throughput includes group creation/admission/close/wait/release; setup and verification are separately recorded.\n";
    return 0;
  }
  try { run(parse(argc, argv)); return 0; }
  catch (const std::exception& error) { std::cerr << "scheduler workload failed: " << error.what() << '\n'; return 1; }
}
