#pragma once
#include "race/platform/platform.hpp"
namespace race::platform {
// Native-adapter failure injection for contract tests, not a runtime feature.
Result<std::unique_ptr<Files>> linux_files_with_write_fault(std::size_t after_bytes);
enum class LinuxFileFault { WriteInterruptedOnce, Close, Rename };
Result<std::unique_ptr<Files>> linux_files_with_fault(LinuxFileFault fault);
}
