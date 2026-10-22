#include "race/core/diagnostics.hpp"
#include "test.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>

namespace {
class DiagnosticClock final : public race::platform::Clock {
public:
  std::atomic<std::uint64_t> ticks{0};
  race::platform::MonotonicTime now() const noexcept override { return {ticks.load(), 13}; }
};
std::atomic<std::size_t> emergency_bytes{0};
void fixture_sink(std::string_view text) noexcept { emergency_bytes += text.size(); }
}
void diagnostics_tests() {
  using namespace race; using namespace race::core;
  DiagnosticClock clock;
  Diagnostics diagnostics(clock, 2);
  CHECK(diagnostics.log(LogLevel::Info, "one"));
  CHECK(diagnostics.log(LogLevel::Error, std::string(200, 'x')));
  CHECK(!diagnostics.log(LogLevel::Warning, "overflow"));
  auto counters = diagnostics.counters(); CHECK(counters.log_attempts == 3); CHECK(counters.dropped_logs == 1);
  CHECK(counters.truncated_logs == 1);
  std::array<LogRecord, 2> records{};
  CHECK(diagnostics.drain(records) == 2); CHECK(std::strcmp(records[0].message.data(), "one") == 0);
  CHECK(records[1].truncated); CHECK(std::strlen(records[1].message.data()) == 159);
  CHECK(records[0].sequence == 1); CHECK(records[1].sequence == 2); CHECK(diagnostics.drain(records) == 0);
  CHECK(diagnostics.log(LogLevel::Info, "four")); CHECK(diagnostics.drain(std::span(records).first(1)) == 1);
  CHECK(records[0].sequence == 4);
  { TimingScope timing(diagnostics, clock); clock.ticks = 10; }
  CHECK(diagnostics.counters().timing_samples == 0);
  diagnostics.enable_detailed_capture(true);
  { TimingScope timing(diagnostics, clock); clock.ticks = 25; }
  CHECK(diagnostics.counters().timing_samples == 1); CHECK(diagnostics.counters().timing_total_ns == 15);
  diagnostics.record_timing({7, 1}, {8, 2}); diagnostics.record_timing({9, 1}, {8, 1});
  CHECK(diagnostics.counters().timing_errors == 2);
  diagnostics.record_timing({0, 1}, {~std::uint64_t{0}, 1});
  CHECK(diagnostics.counters().timing_total_ns == ~std::uint64_t{0});
  Diagnostics concurrent(clock, 128);
  std::array<std::thread, 4> workers;
  for (auto& worker : workers) worker = std::thread([&] {
    for (int i = 0; i < 1000; ++i) { concurrent.count_operation(); (void)concurrent.log(LogLevel::Info, "thread fixture"); }
  });
  for (auto& worker : workers) worker.join();
  counters = concurrent.counters(); CHECK(counters.operations == 4000); CHECK(counters.log_attempts == 4000);
  CHECK(counters.dropped_logs == 4000 - 128);
  std::array<LogRecord, 128> concurrent_records{}; CHECK(concurrent.drain(concurrent_records) == 128);
  for (std::size_t i = 1; i < concurrent_records.size(); ++i) CHECK(concurrent_records[i].sequence > concurrent_records[i - 1].sequence);
  const auto failures = emergency_failure_count();
  set_emergency_sink(fixture_sink); emergency_failure("fixture allocation failure");
  CHECK(emergency_bytes == 26); CHECK(emergency_failure_count() == failures + 1);
  set_emergency_sink(nullptr); emergency_failure("fixture without sink");
  CHECK(emergency_failure_count() == failures + 2);
}
