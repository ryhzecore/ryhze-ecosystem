#include "test.hpp"
#include "race/foundation/result.hpp"
#include "race/core/diagnostics.hpp"
#include <string_view>
void platform_tests(); void memory_tests(); void identity_tests(); void jobs_tests();
void time_tests(); void config_tests(); void diagnostics_tests(); void lifecycle_tests();
void mock_io_tests();
int main(int argc, char** argv) {
#ifdef _WIN32
  _set_error_mode(_OUT_TO_STDERR);
  _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#endif
  race::core::set_emergency_sink(race::platform::emergency_write);
  if (argc != 2) return 2;
  std::string_view suite(argv[1]);
  if (suite == "fatal") { race::Result<int> error(race::fail(race::ErrorCode::Invariant, "fixture")); return error.value(); }
  if (suite == "fatal_diagnostics") race::core::invariant_failure("controlled invariant fixture");
  struct Test { const char* name; void (*run)(); };
  const Test tests[]{{"platform",platform_tests},{"memory",memory_tests},{"identity",identity_tests},{"jobs",jobs_tests},{"time",time_tests},{"config",config_tests},{"diagnostics",diagnostics_tests},{"lifecycle",lifecycle_tests},{"mock_io",mock_io_tests}};
  for (auto test : tests) if (suite == test.name) { test.run(); std::printf("%s: passed\n",test.name); return 0; }
  return 2;
}
