#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "race/platform/directory_watch.hpp"

namespace race::platform::directory_watch::detail {
// Private fault seam. Production binds the native functions; each watch copies
// its immutable call table, with no global override or borrowed table lifetime.
struct Calls {
  decltype(&::ResetEvent) reset_event{::ResetEvent};
  decltype(&::ReadDirectoryChangesW) arm{::ReadDirectoryChangesW};
  decltype(&::CancelIoEx) cancel{::CancelIoEx};
};
Result<std::unique_ptr<Service>> make_with_calls(std::shared_ptr<RootedFiles>,
    std::uint64_t,const Limits&,Calls);
}
