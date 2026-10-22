#pragma once
#include "race/platform/rooted_files.hpp"
#include <string>

namespace race::platform {
// Private adapter composition. Caller retains the capability while using its
// handle-resolved normalized DOS path; unsupported foreign RootedFiles are rejected.
Result<std::wstring> windows_pinned_root_path(const std::shared_ptr<RootedFiles>&);
}
