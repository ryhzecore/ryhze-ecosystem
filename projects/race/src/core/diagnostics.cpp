#include "race/core/diagnostics.hpp"
#include <algorithm>
#include <cstdlib>
#include <limits>

namespace race::core {
namespace {
std::atomic<EmergencySink> emergency_sink{nullptr};
std::atomic<std::uint64_t> emergency_count{0};
// Saturation makes wrap impossible; captures remain meaningful at long uptime.
void add_saturated(std::atomic<std::uint64_t>& counter, std::uint64_t delta) noexcept {
  auto value = counter.load(std::memory_order_relaxed);
  for (;;) {
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto next = delta > maximum - value ? maximum : value + delta;
    if (counter.compare_exchange_weak(value, next, std::memory_order_relaxed)) return;
  }
}
}
Diagnostics::Diagnostics(const platform::Clock& clock, std::size_t capacity)
  : clock_(clock), capacity_(capacity) {
  if (capacity == 0 || capacity > maximum_records) invariant_failure("invalid diagnostic capacity");
}
Result<void> Diagnostics::log(LogLevel level, std::string_view message) noexcept {
  const auto timestamp = clock_.now().nanoseconds;
  std::lock_guard lock(mutex_);
  add_saturated(attempts_, 1);
  if (size_ == capacity_) {
    add_saturated(dropped_, 1);
    return fail(ErrorCode::QueueFull, "diagnostic record capacity exhausted");
  }
  auto& record = records_[(head_ + size_) % capacity_];
  record = {};
  record.sequence = attempts_.load(std::memory_order_relaxed);
  record.timestamp_ns = timestamp; record.level = level;
  const auto count = std::min(message.size(), record.message.size() - 1);
  std::copy_n(message.begin(), count, record.message.begin());
  record.truncated = count != message.size();
  if (record.truncated) add_saturated(truncated_, 1);
  ++size_;
  return {};
}
std::size_t Diagnostics::drain(std::span<LogRecord> output) noexcept {
  std::lock_guard lock(mutex_);
  const auto count = std::min(output.size(), size_);
  for (std::size_t i = 0; i < count; ++i) output[i] = records_[(head_ + i) % capacity_];
  head_ = (head_ + count) % capacity_; size_ -= count;
  return count;
}
void Diagnostics::count_operation() noexcept { add_saturated(operations_, 1); }
void Diagnostics::enable_detailed_capture(bool enabled) noexcept { capture_.store(enabled, std::memory_order_relaxed); }
bool Diagnostics::detailed_capture_enabled() const noexcept { return capture_.load(std::memory_order_relaxed); }
void Diagnostics::record_timing(platform::MonotonicTime start, platform::MonotonicTime end) noexcept {
  if (start.domain != end.domain || end.nanoseconds < start.nanoseconds) { add_saturated(timing_errors_, 1); return; }
  add_saturated(samples_, 1); add_saturated(duration_, end.nanoseconds - start.nanoseconds);
}
DiagnosticCounters Diagnostics::counters() const noexcept {
  return {attempts_.load(std::memory_order_relaxed), dropped_.load(std::memory_order_relaxed),
          truncated_.load(std::memory_order_relaxed), operations_.load(std::memory_order_relaxed),
          samples_.load(std::memory_order_relaxed), duration_.load(std::memory_order_relaxed),
          timing_errors_.load(std::memory_order_relaxed)};
}
TimingScope::TimingScope(Diagnostics& diagnostics, const platform::Clock& clock) noexcept
  : diagnostics_(diagnostics), clock_(clock), enabled_(diagnostics.detailed_capture_enabled()) {
  if (enabled_) start_ = clock_.now();
}
TimingScope::~TimingScope() { if (enabled_) diagnostics_.record_timing(start_, clock_.now()); }
void set_emergency_sink(EmergencySink sink) noexcept { emergency_sink.store(sink, std::memory_order_release); }
std::uint64_t emergency_failure_count() noexcept { return emergency_count.load(std::memory_order_relaxed); }
void emergency_failure(const char* message) noexcept {
  add_saturated(emergency_count, 1);
  if (const auto sink = emergency_sink.load(std::memory_order_acquire)) {
    // Bounded scan avoids unbounded diagnostic work. The public precondition is a
    // valid static literal; this path cannot validate an arbitrary corrupt pointer.
    std::size_t size = 0;
    while (size < 160 && message[size] != '\0') ++size;
    sink(std::string_view(message, size));
  }
}
[[noreturn]] void invariant_failure(const char* message) noexcept {
  emergency_failure(message);
  std::abort();
}
}
