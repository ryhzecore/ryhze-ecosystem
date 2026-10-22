#pragma once
#include "race/platform/platform.hpp"

namespace race::platform {
// A pinned content-root capability. Names are canonical lowercase single leaves;
// no directory traversal, parent symlinks, mount lookup or implicit path joining.
// Test doubles must uphold this contract. Runtime borrows are shared ownership.
class RootedFiles : public Files {
public:
  // No clobber: an existing name succeeds only when its bytes are identical.
  virtual Result<void> publish_immutable(std::string_view, std::span<const std::byte>) = 0;
};
bool valid_content_leaf(std::string_view) noexcept;
Result<std::shared_ptr<RootedFiles>> make_linux_rooted_files(std::string_view absolute_root);
Result<std::shared_ptr<RootedFiles>> make_windows_rooted_files(std::string_view absolute_root);
inline Result<std::shared_ptr<RootedFiles>> make_rooted_files(std::string_view root) {
#ifdef _WIN32
  return make_windows_rooted_files(root);
#else
  return make_linux_rooted_files(root);
#endif
}
} // namespace race::platform
