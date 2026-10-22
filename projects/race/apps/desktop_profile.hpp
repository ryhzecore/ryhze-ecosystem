#pragma once
#include <cstddef>
#include <cstdint>

namespace race::desktop::profile {
// Executable/static-library C++ allocation REQUESTS only. Includes failed and
// zero-byte requests; excludes malloc, frees/live ownership and allocations made
// inside Qt/SDL/driver DLLs. requested_bytes excludes allocator overhead/padding.
struct AllocationSnapshot {
  std::uint64_t requests{};
  std::uint64_t requested_bytes{};
  bool overflowed{}; // Either counter saturated; do not subtract saturated values.
};
bool allocation_counting_available() noexcept;
AllocationSnapshot allocation_snapshot() noexcept;
// Reset requires no active scopes. Returns false rather than clearing a live run.
bool reset_allocation_counters() noexcept;

// Process-wide gate, nesting/overlap safe across threads. Snapshots are coherent
// at the request-recording point. Start/stop do not imply application quiescence.
// With the feature disabled at compilation this API is inert; no allocator
// replacement is linked. Compare baseline/after with the SAME build feature.
class AllocationScope {
  bool active_{};
public:
  explicit AllocationScope(bool enable=true) noexcept;
  ~AllocationScope();
  AllocationScope(const AllocationScope&)=delete;
  AllocationScope& operator=(const AllocationScope&)=delete;
  AllocationScope(AllocationScope&&)=delete;
  AllocationScope& operator=(AllocationScope&&)=delete;
  bool active() const noexcept { return active_; }
  void stop() noexcept;
};

#if defined(RACE_DESKTOP_PROFILE_TESTING) && RACE_DESKTOP_PROFILE_TESTING
namespace testing {
// Thread-local deterministic allocator failure, only compiled into the test
// executable. Attempts include retries made after calling a new_handler.
void fail_next_allocation_attempts(std::size_t count) noexcept;
// Fail one attempt after this many successful attempts on the current thread.
// fail_next_allocation_attempts(0) also disarms this countdown.
void fail_allocation_after(std::size_t successful_attempts) noexcept;
bool seed_counters(std::uint64_t requests, std::uint64_t requested_bytes) noexcept;
}
#endif
} // namespace race::desktop::profile
