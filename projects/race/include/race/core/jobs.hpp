#pragma once
#include "race/foundation/result.hpp"
#include "race/platform/platform.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace race::core {
struct JobHandle {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  std::uint64_t scheduler = 0;
  friend bool operator==(const JobHandle&, const JobHandle&) = default;
};
struct GroupHandle {
  std::uint32_t slot = UINT32_MAX;
  std::uint64_t generation = 0;
  std::uint64_t scheduler = 0;
  friend bool operator==(const GroupHandle&, const GroupHandle&) = default;
};
enum class AccessMode { Read, Write };
struct ResourceAccess { std::uint64_t key; AccessMode mode; };
class JobContext {
  void* context_;
  const void* cancellation_;
public:
  JobContext(void* context, const void* cancellation) : context_(context), cancellation_(cancellation) {}
  void* context() const noexcept { return context_; }
  bool cancelled() const noexcept;
};
using JobFunction = Result<void> (*)(const JobContext&);
struct JobDesc {
  JobDesc() = default;
  JobDesc(JobFunction fn, std::shared_ptr<void> context = {}) : function(fn), owned_context(std::move(context)) {}
  JobFunction function = nullptr;
  std::shared_ptr<void> owned_context;
  std::span<const JobHandle> prerequisites;
  // Indices in this submit_batch, validated as a DAG before any dispatch.
  std::span<const std::size_t> batch_prerequisites;
  std::span<const ResourceAccess> accesses;
  bool unknown_access = true;
};
enum class JobState { Waiting, Running, Succeeded, Failed, Cancelled, FailedPrerequisite };
struct JobCompletion {
  JobState state;
  Error error{ErrorCode::Invariant, "no error"}; // Valid only for failed/cancelled terminal states.
  bool terminal() const noexcept { return state != JobState::Waiting && state != JobState::Running; }
};
struct GroupCompletion {
  std::uint64_t succeeded = 0;
  std::uint64_t failed = 0;
  std::uint64_t cancelled = 0;
};
struct SchedulerConfig {
  std::size_t capacity = 128;
  std::size_t workers = 2;
  std::size_t max_groups = 16;
};
struct SchedulerStats {
  std::size_t occupied = 0;
  std::size_t pending = 0;
  std::size_t running = 0;
  std::size_t peak_pending = 0;
  std::uint64_t admitted = 0;
  std::uint64_t completed = 0;
};
enum class ShutdownMode { Drain, Cancel };
class Scheduler {
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit Scheduler(std::unique_ptr<Impl>);
public:
  static constexpr std::size_t max_dependencies = 16;
  static constexpr std::size_t max_accesses = 16;
  static Result<std::unique_ptr<Scheduler>> create(platform::Threads&, SchedulerConfig);
  ~Scheduler(); // Cancels, waits for quiescence, then joins. Never detaches/free-on-timeout.
  Scheduler(const Scheduler&) = delete;
  Scheduler& operator=(const Scheduler&) = delete;
  Result<GroupHandle> create_group();
  Result<JobHandle> submit(GroupHandle, const JobDesc&);
  Result<std::vector<JobHandle>> submit_batch(GroupHandle, std::span<const JobDesc>);
  Result<void> close(GroupHandle);
  Result<void> cancel(GroupHandle);
  // Timeout is the nominal completion-wait-stage duration after locking, not a
  // total call deadline. Locking/relocking and scheduling may extend the call.
  // Zero requests no intentional CV delay; max() waits without a timeout;
  // negative durations are invalid. A released group returns StaleHandle.
  Result<GroupCompletion> wait(GroupHandle, std::chrono::milliseconds timeout);
  Result<JobCompletion> completion(JobHandle) const;
  // Releases all terminal records in a closed group atomically. External references return Busy.
  Result<void> release(GroupHandle);
  void stop_admission();
  // Uses wait's timeout semantics after stop/close/cancel/settle preparation.
  // Join-lock acquisition and platform joins are untimed. Timeout retains all
  // live owners; it never authorizes freeing them or detaching workers.
  Result<void> shutdown(ShutdownMode, std::chrono::milliseconds timeout);
  SchedulerStats stats() const;
};
}
