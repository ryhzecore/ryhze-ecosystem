#pragma once
#include "race/platform/platform.hpp"
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <string_view>

namespace race::core {
enum class LogLevel : std::uint8_t { Info, Warning, Error };
struct LogRecord {
  std::uint64_t sequence = 0, timestamp_ns = 0;
  LogLevel level = LogLevel::Info;
  std::array<char, 160> message{};
  bool truncated = false;
};
struct DiagnosticCounters {
  std::uint64_t log_attempts, dropped_logs, truncated_logs;
  std::uint64_t operations, timing_samples, timing_total_ns, timing_errors;
};
// Fixed storage, thread-safe, non-owning clock must outlive the service. Records
// stay until drained; full storage rejects the newest message with an error.
class Diagnostics {
  static constexpr std::size_t maximum_records = 128;
  const platform::Clock& clock_;
  const std::size_t capacity_;
  std::array<LogRecord, maximum_records> records_{};
  std::size_t head_ = 0, size_ = 0;
  std::mutex mutex_;
  std::atomic<std::uint64_t> attempts_{0}, dropped_{0}, truncated_{0}, operations_{0};
  std::atomic<std::uint64_t> samples_{0}, duration_{0}, timing_errors_{0};
  std::atomic<bool> capture_{false};
public:
  explicit Diagnostics(const platform::Clock&, std::size_t capacity = maximum_records);
  Result<void> log(LogLevel, std::string_view) noexcept;
  std::size_t drain(std::span<LogRecord>) noexcept;
  void count_operation() noexcept;
  void enable_detailed_capture(bool enabled) noexcept;
  bool detailed_capture_enabled() const noexcept;
  void record_timing(platform::MonotonicTime start, platform::MonotonicTime end) noexcept;
  DiagnosticCounters counters() const noexcept;
};
class TimingScope {
  Diagnostics& diagnostics_;
  const platform::Clock& clock_;
  platform::MonotonicTime start_{};
  bool enabled_;
public:
  TimingScope(Diagnostics&, const platform::Clock&) noexcept;
  ~TimingScope();
  TimingScope(const TimingScope&) = delete;
  TimingScope& operator=(const TimingScope&) = delete;
};
// No diagnostic heap or storage dependency. Static literals only; no user data.
using EmergencySink = void (*)(std::string_view) noexcept;
void set_emergency_sink(EmergencySink) noexcept;
std::uint64_t emergency_failure_count() noexcept;
void emergency_failure(const char* static_message) noexcept;
[[noreturn]] void invariant_failure(const char* static_message) noexcept;
}
