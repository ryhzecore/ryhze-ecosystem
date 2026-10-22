#include "race/core/jobs.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <new>

namespace race::core {
namespace {
std::atomic<std::uint64_t> next_scheduler{1};
thread_local const void* worker_scheduler = nullptr;
bool terminal(JobState state) { return state != JobState::Waiting && state != JobState::Running; }
}
bool JobContext::cancelled() const noexcept {
  return static_cast<const std::atomic<bool>*>(cancellation_)->load(std::memory_order_acquire);
}
struct Scheduler::Impl {
  struct Slot {
    bool used = false;
    bool retired = false;
    std::uint64_t generation = 1;
    std::uint64_t sequence = 0;
    GroupHandle group;
    JobFunction function = nullptr;
    std::shared_ptr<void> context;
    std::array<JobHandle, max_dependencies> dependencies{};
    std::size_t dependency_count = 0;
    std::size_t references = 0;
    std::array<ResourceAccess, max_accesses> accesses{};
    std::size_t access_count = 0;
    bool unknown = true;
    std::atomic<bool> cancel{false};
    JobState state = JobState::Waiting;
    Error error{ErrorCode::Invariant, "no error"};
  };
  struct Group {
    bool used = false;
    bool retired = false;
    bool closed = false;
    bool cancelled = false;
    std::uint64_t generation = 1;
    std::size_t pending = 0;
    GroupCompletion completion;
  };
  SchedulerConfig config;
  std::uint64_t identity;
  std::uint64_t next_sequence = 1;
  std::unique_ptr<Slot[]> slots;
  std::unique_ptr<Group[]> groups;
  std::vector<std::unique_ptr<platform::Thread>> workers;
  // Admission scratch is preallocated once and used under mutex.
  std::vector<std::size_t> selected, indegree, ready;
  mutable std::mutex mutex;
  std::mutex join_mutex;
  std::condition_variable changed;
  bool accepting = true;
  bool shutting_down = false;
  SchedulerStats statistics;
  Impl(SchedulerConfig cfg, std::uint64_t id)
      : config(cfg), identity(id), slots(std::make_unique<Slot[]>(cfg.capacity)),
        groups(std::make_unique<Group[]>(cfg.max_groups)), selected(cfg.capacity),
        indegree(cfg.capacity), ready(cfg.capacity) { workers.reserve(cfg.workers); }
  Slot* resolve(JobHandle h) const {
    if (h.scheduler != identity || h.slot >= config.capacity) return nullptr;
    auto& s = slots[h.slot];
    return s.used && s.generation == h.generation ? &s : nullptr;
  }
  Group* resolve(GroupHandle h) const {
    if (h.scheduler != identity || h.slot >= config.max_groups) return nullptr;
    auto& g = groups[h.slot];
    return g.used && g.generation == h.generation ? &g : nullptr;
  }
  void finish(Slot& slot, JobState state, Error error = {ErrorCode::Invariant, "no error"}) {
    if (terminal(slot.state)) std::abort();
    if (slot.state == JobState::Running) --statistics.running;
    slot.state = state;
    slot.error = error;
    --statistics.pending;
    ++statistics.completed;
    auto& group = *resolve(slot.group);
    --group.pending;
    if (state == JobState::Succeeded) ++group.completion.succeeded;
    else if (state == JobState::Cancelled) ++group.completion.cancelled;
    else ++group.completion.failed;
    for (std::size_t i = 0; i < slot.dependency_count; ++i) {
      auto* dependency = resolve(slot.dependencies[i]);
      if (!dependency || dependency->references == 0) std::abort();
      --dependency->references;
    }
  }
  bool conflicts(const Slot& a, const Slot& b) const {
    if (a.unknown || b.unknown) return true;
    for (std::size_t i = 0; i < a.access_count; ++i)
      for (std::size_t j = 0; j < b.access_count; ++j)
        if (a.accesses[i].key == b.accesses[j].key &&
            (a.accesses[i].mode == AccessMode::Write || b.accesses[j].mode == AccessMode::Write)) return true;
    return false;
  }
  // Propagate terminal failures until stable so no failed chain stays blocked.
  void settle() {
    bool again;
    do {
      again = false;
      for (std::size_t i = 0; i < config.capacity; ++i) {
        auto& slot = slots[i];
        if (!slot.used || slot.state != JobState::Waiting) continue;
        if (slot.cancel.load(std::memory_order_acquire)) {
          finish(slot, JobState::Cancelled, fail(ErrorCode::Cancelled, "job cancelled before execution"));
          again = true;
          continue;
        }
        for (std::size_t d = 0; d < slot.dependency_count; ++d) {
          auto* prerequisite = resolve(slot.dependencies[d]);
          if (!prerequisite) std::abort();
          if (terminal(prerequisite->state) && prerequisite->state != JobState::Succeeded) {
            finish(slot, JobState::FailedPrerequisite, fail(ErrorCode::FailedPrerequisite, "job prerequisite did not succeed"));
            again = true;
            break;
          }
        }
      }
    } while (again);
  }
  Slot* runnable() {
    Slot* best = nullptr;
    for (std::size_t i = 0; i < config.capacity; ++i) {
      auto& candidate = slots[i];
      if (!candidate.used || candidate.state != JobState::Waiting) continue;
      bool eligible = true;
      for (std::size_t d = 0; d < candidate.dependency_count; ++d) {
        auto* prerequisite = resolve(candidate.dependencies[d]);
        if (!prerequisite || prerequisite->state != JobState::Succeeded) { eligible = false; break; }
      }
      if (!eligible) continue;
      // A ready older writer/unknown job forms a barrier for younger conflicts.
      // Without this, a continuous stream of readers could prevent its turn.
      for (std::size_t r = 0; r < config.capacity && eligible; ++r) {
        const auto& older = slots[r];
        if (!older.used || older.state != JobState::Waiting || older.sequence >= candidate.sequence || !conflicts(candidate, older)) continue;
        bool older_ready = true;
        for (std::size_t d = 0; d < older.dependency_count; ++d) {
          auto* prerequisite = resolve(older.dependencies[d]);
          if (!prerequisite || prerequisite->state != JobState::Succeeded) { older_ready = false; break; }
        }
        if (older_ready) eligible = false;
      }
      if (!eligible) continue;
      for (std::size_t r = 0; r < config.capacity; ++r) {
        auto& other = slots[r];
        if (other.used && other.state == JobState::Running && conflicts(candidate, other)) { eligible = false; break; }
      }
      if (eligible && (!best || candidate.sequence < best->sequence)) best = &candidate;
    }
    return best;
  }
  void work() {
    worker_scheduler = this;
    std::unique_lock lock(mutex);
    for (;;) {
      const auto previous_completed = statistics.completed;
      settle();
      if (statistics.completed != previous_completed) changed.notify_all();
      if (shutting_down && statistics.pending == 0) break;
      Slot* slot = runnable();
      if (!slot) { changed.wait(lock); continue; }
      slot->state = JobState::Running;
      ++statistics.running;
      auto function = slot->function;
      JobContext context(slot->context.get(), &slot->cancel);
      lock.unlock();
      Result<void> result;
      try { result = function(context); }
      catch (const std::bad_alloc&) { result = fail(ErrorCode::Exhausted, "job allocation failed"); }
      catch (...) { result = fail(ErrorCode::PlatformFailure, "job callback threw"); }
      lock.lock();
      if (result) finish(*slot, JobState::Succeeded);
      else finish(*slot, result.error().code == ErrorCode::Cancelled ? JobState::Cancelled : JobState::Failed, result.error());
      changed.notify_all();
    }
    worker_scheduler = nullptr;
  }
};
Scheduler::Scheduler(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result<std::unique_ptr<Scheduler>> Scheduler::create(platform::Threads& threads, SchedulerConfig config) {
  if (config.capacity == 0 || config.capacity > 65536 || config.max_groups == 0 ||
      config.max_groups > config.capacity || config.workers == 0 || config.workers > 256)
    return fail(ErrorCode::InvalidInput, "invalid scheduler capacity or worker count");
  // Refuse wrap: an exhausted identity source remains exhausted permanently.
  auto id = next_scheduler.load(std::memory_order_relaxed);
  do {
    if (id == UINT64_MAX) return fail(ErrorCode::Exhausted, "scheduler identity space exhausted");
  } while (!next_scheduler.compare_exchange_weak(id, id + 1, std::memory_order_relaxed));
  try {
    auto scheduler = std::unique_ptr<Scheduler>(new Scheduler(std::make_unique<Impl>(config, id)));
    for (std::size_t i = 0; i < config.workers; ++i) {
      auto thread = threads.start([impl = scheduler->impl_.get()] { impl->work(); });
      if (!thread) return thread.error(); // Scheduler destructor joins already-started threads.
      scheduler->impl_->workers.push_back(std::move(thread).value());
    }
    return scheduler;
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "scheduler allocation failed"); }
    catch (...) { return fail(ErrorCode::PlatformFailure, "thread provider threw"); }
}
Scheduler::~Scheduler() {
  if (worker_scheduler == impl_.get()) std::abort(); // Destruction on an owned worker cannot join itself.
  auto result = shutdown(ShutdownMode::Cancel, std::chrono::milliseconds::max());
  if (!result) std::abort(); // A failed join is not permission to destroy a running owner.
  // User context destructors run while service synchronization still exists.
  for (std::size_t i = 0; i < impl_->config.capacity; ++i) impl_->slots[i].context.reset();
}
Result<GroupHandle> Scheduler::create_group() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->accepting) return fail(ErrorCode::Stopping, "scheduler admission stopped");
  for (std::size_t i = 0; i < impl_->config.max_groups; ++i) {
    auto& group = impl_->groups[i];
    if (!group.used && !group.retired) {
      group.used = true;
      group.closed = false;
      group.cancelled = false;
      group.pending = 0;
      group.completion = {};
      return GroupHandle{static_cast<std::uint32_t>(i), group.generation, impl_->identity};
    }
  }
  return fail(ErrorCode::QueueFull, "task group capacity full");
}
Result<JobHandle> Scheduler::submit(GroupHandle group, const JobDesc& desc) {
  auto result = submit_batch(group, std::span(&desc, 1));
  if (!result) return result.error();
  return result.value().front();
}
Result<std::vector<JobHandle>> Scheduler::submit_batch(GroupHandle handle, std::span<const JobDesc> jobs) {
  if (jobs.empty() || jobs.size() > impl_->config.capacity) return fail(ErrorCode::InvalidInput, "invalid batch size");
  std::vector<JobHandle> handles;
  try { handles.resize(jobs.size()); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "result storage allocation failed"); }
  std::lock_guard lock(impl_->mutex);
  if (!impl_->accepting) return fail(ErrorCode::Stopping, "scheduler admission stopped");
  auto* group = impl_->resolve(handle);
  if (!group) return fail(ErrorCode::StaleHandle, "invalid task group");
  if (group->closed) return fail(ErrorCode::Stopping, "task group submission closed");
  if (group->cancelled) return fail(ErrorCode::Cancelled, "task group cancelled");
  if (jobs.size() > UINT64_MAX - impl_->next_sequence) return fail(ErrorCode::Overflow, "job sequence exhausted");
  std::size_t available = 0;
  for (std::size_t i = 0; i < impl_->config.capacity && available < jobs.size(); ++i)
    if (!impl_->slots[i].used && !impl_->slots[i].retired) impl_->selected[available++] = i;
  if (available < jobs.size()) return fail(ErrorCode::QueueFull, "job records full; release completed groups");
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    const auto& job = jobs[j];
    if (!job.function || job.prerequisites.size() > max_dependencies ||
        job.batch_prerequisites.size() > max_dependencies - job.prerequisites.size() || job.accesses.size() > max_accesses)
      return fail(ErrorCode::InvalidInput, "job metadata exceeds contract");
    for (auto prerequisite : job.prerequisites)
      if (!impl_->resolve(prerequisite)) return fail(ErrorCode::InvalidDependency, "invalid job prerequisite");
    for (auto prerequisite : job.batch_prerequisites)
      if (prerequisite >= jobs.size()) return fail(ErrorCode::InvalidDependency, "invalid batch prerequisite");
    for (auto access : job.accesses)
      if (access.mode != AccessMode::Read && access.mode != AccessMode::Write)
        return fail(ErrorCode::InvalidInput, "invalid resource access mode");
    impl_->indegree[j] = job.batch_prerequisites.size();
  }
  std::size_t ready_count = 0, visited = 0;
  for (std::size_t j = 0; j < jobs.size(); ++j) if (!impl_->indegree[j]) impl_->ready[ready_count++] = j;
  while (visited < ready_count) {
    const auto current = impl_->ready[visited++];
    for (std::size_t j = 0; j < jobs.size(); ++j)
      for (auto predecessor : jobs[j].batch_prerequisites)
        if (predecessor == current && --impl_->indegree[j] == 0) impl_->ready[ready_count++] = j;
  }
  if (visited != jobs.size()) return fail(ErrorCode::Cycle, "cyclic job batch");
  // No failing/allocating operations beyond this point: admission commits atomically.
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    const auto index = impl_->selected[j];
    auto& slot = impl_->slots[index];
    handles[j] = {static_cast<std::uint32_t>(index), slot.generation, impl_->identity};
  }
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    auto& slot = impl_->slots[impl_->selected[j]];
    const auto& job = jobs[j];
    slot.used = true;
    slot.group = handle;
    slot.sequence = impl_->next_sequence++;
    slot.function = job.function;
    slot.context = job.owned_context;
    slot.dependency_count = 0;
    slot.references = 0;
    for (auto dependency : job.prerequisites) slot.dependencies[slot.dependency_count++] = dependency;
    for (auto dependency : job.batch_prerequisites) slot.dependencies[slot.dependency_count++] = handles[dependency];
    slot.access_count = job.accesses.size();
    std::copy(job.accesses.begin(), job.accesses.end(), slot.accesses.begin());
    slot.unknown = job.unknown_access;
    slot.cancel.store(false, std::memory_order_release);
    slot.state = JobState::Waiting;
    slot.error = {ErrorCode::Invariant, "no error"};
  }
  for (std::size_t j = 0; j < jobs.size(); ++j) {
    auto& slot = impl_->slots[impl_->selected[j]];
    for (std::size_t d = 0; d < slot.dependency_count; ++d) ++impl_->resolve(slot.dependencies[d])->references;
  }
  group->pending += jobs.size();
  impl_->statistics.occupied += jobs.size();
  impl_->statistics.pending += jobs.size();
  impl_->statistics.admitted += jobs.size();
  impl_->statistics.peak_pending = std::max(impl_->statistics.peak_pending, impl_->statistics.pending);
  impl_->changed.notify_all();
  return handles;
}
Result<void> Scheduler::close(GroupHandle handle) {
  std::lock_guard lock(impl_->mutex);
  auto* group = impl_->resolve(handle);
  if (!group) return fail(ErrorCode::StaleHandle, "invalid task group");
  group->closed = true;
  impl_->changed.notify_all();
  return {};
}
Result<void> Scheduler::cancel(GroupHandle handle) {
  std::lock_guard lock(impl_->mutex);
  auto* group = impl_->resolve(handle);
  if (!group) return fail(ErrorCode::StaleHandle, "invalid task group");
  group->cancelled = true;
  for (std::size_t i = 0; i < impl_->config.capacity; ++i)
    if (impl_->slots[i].used && impl_->slots[i].group == handle) impl_->slots[i].cancel.store(true, std::memory_order_release);
  impl_->settle();
  impl_->changed.notify_all();
  return {};
}
Result<GroupCompletion> Scheduler::wait(GroupHandle handle, std::chrono::milliseconds timeout) {
  if (worker_scheduler) return fail(ErrorCode::WrongThread, "CPU workers cannot wait for jobs");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidInput, "negative wait timeout");
  std::unique_lock lock(impl_->mutex);
  auto* group = impl_->resolve(handle);
  if (!group) return fail(ErrorCode::StaleHandle, "invalid task group");
  if (!group->closed) return fail(ErrorCode::Busy, "close submission before waiting");
  const auto done = [&] { return !impl_->resolve(handle) || group->pending == 0; };
  if (timeout == std::chrono::milliseconds::max()) impl_->changed.wait(lock, done);
  else if (!impl_->changed.wait_for(lock, timeout, done)) return fail(ErrorCode::Timeout, "task group still active");
  if (!impl_->resolve(handle)) return fail(ErrorCode::StaleHandle, "task group released during wait");
  return group->completion;
}
Result<JobCompletion> Scheduler::completion(JobHandle handle) const {
  std::lock_guard lock(impl_->mutex);
  auto* slot = impl_->resolve(handle);
  if (!slot) return fail(ErrorCode::StaleHandle, "invalid job handle");
  return JobCompletion{slot->state, slot->error};
}
Result<void> Scheduler::release(GroupHandle handle) {
  // Context destructors can be application code. Transfer owners out under lock, destroy without it.
  std::vector<std::shared_ptr<void>> owners;
  try { owners.reserve(impl_->config.capacity); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "release scratch allocation failed"); }
  {
    std::lock_guard lock(impl_->mutex);
    auto* group = impl_->resolve(handle);
    if (!group) return fail(ErrorCode::StaleHandle, "invalid task group");
    if (!group->closed || group->pending) return fail(ErrorCode::Busy, "task group is not quiescent");
    for (std::size_t i = 0; i < impl_->config.capacity; ++i) {
      auto& slot = impl_->slots[i];
      if (slot.used && slot.group == handle && slot.references)
        return fail(ErrorCode::Busy, "external jobs still reference task group");
    }
    for (std::size_t i = 0; i < impl_->config.capacity; ++i) {
      auto& slot = impl_->slots[i];
      if (!slot.used || slot.group != handle) continue;
      owners.push_back(std::move(slot.context));
      slot.used = false;
      --impl_->statistics.occupied;
      if (slot.generation == UINT64_MAX) slot.retired = true;
      else ++slot.generation;
    }
    group->used = false;
    if (group->generation == UINT64_MAX) group->retired = true;
    else ++group->generation;
    impl_->changed.notify_all();
  }
  return {};
}
void Scheduler::stop_admission() {
  std::lock_guard lock(impl_->mutex);
  impl_->accepting = false;
}
Result<void> Scheduler::shutdown(ShutdownMode mode, std::chrono::milliseconds timeout) {
  if (worker_scheduler) return fail(ErrorCode::WrongThread, "CPU workers cannot shut down schedulers");
  if (timeout.count() < 0) return fail(ErrorCode::InvalidInput, "negative shutdown timeout");
  {
    std::unique_lock lock(impl_->mutex);
    impl_->accepting = false;
    impl_->shutting_down = true;
    for (std::size_t i = 0; i < impl_->config.max_groups; ++i) if (impl_->groups[i].used) impl_->groups[i].closed = true;
    if (mode == ShutdownMode::Cancel) {
      for (std::size_t i = 0; i < impl_->config.capacity; ++i)
        if (impl_->slots[i].used) impl_->slots[i].cancel.store(true, std::memory_order_release);
    }
    impl_->settle();
    impl_->changed.notify_all();
    const auto done = [&] { return impl_->statistics.pending == 0; };
    if (timeout == std::chrono::milliseconds::max()) impl_->changed.wait(lock, done);
    else if (!impl_->changed.wait_for(lock, timeout, done)) return fail(ErrorCode::Timeout, "shutdown incomplete; owners retained");
  }
  std::lock_guard join_lock(impl_->join_mutex);
  for (auto& worker : impl_->workers) {
    if (!worker) continue;
    auto result = worker->join();
    if (!result) return result.error();
    worker.reset();
  }
  return {};
}
SchedulerStats Scheduler::stats() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->statistics;
}
}
