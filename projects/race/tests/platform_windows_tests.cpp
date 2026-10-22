#include "test.hpp"
#include "race/platform/rooted_files.hpp"
#include "race/platform/directory_watch.hpp"
#include <array>
#include <algorithm>
#include <iterator>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winioctl.h>
#include <cstddef>
using namespace race;
namespace {
struct JunctionResult {
  bool created{};
  DWORD error{};
};
JunctionResult set_junction(const std::filesystem::path &directory, const std::filesystem::path &target) {
  // Native mount-point reparse data: byte offsets are relative to PathBuffer.
  struct MountPoint {
    DWORD tag;
    WORD data_length, reserved;
    WORD substitute_offset, substitute_length, print_offset, print_length;
    wchar_t path[1024];
  } buffer{};
  static_assert(offsetof(MountPoint, path) == 16);
  const std::wstring print = target.wstring();
  const std::wstring substitute = L"\\??\\" + print;
  CHECK(substitute.size() + print.size() + 2 < std::size(buffer.path));
  buffer.tag = IO_REPARSE_TAG_MOUNT_POINT;
  buffer.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  buffer.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
  buffer.print_length = static_cast<WORD>(print.size() * sizeof(wchar_t));
  std::copy(substitute.begin(), substitute.end(), buffer.path);
  std::copy(print.begin(), print.end(), buffer.path + substitute.size() + 1);
  buffer.data_length = static_cast<WORD>(8 + (substitute.size() + print.size() + 2) * sizeof(wchar_t));
  HANDLE handle =
      CreateFileW(directory.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                  nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (handle == INVALID_HANDLE_VALUE)
    return {false, GetLastError()};
  DWORD returned{};
  const bool made =
      DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &buffer, static_cast<DWORD>(8 + buffer.data_length),
                      nullptr, 0, &returned, nullptr) != FALSE;
  const DWORD error = made ? ERROR_SUCCESS : GetLastError();
  CHECK(CloseHandle(handle));
  return {made, error};
}
void empty_root_confinement(const std::filesystem::path &fixture) {
  const auto root = fixture / "empty-root", target = fixture / "junction-target";
  CHECK(std::filesystem::create_directory(root));
  CHECK(std::filesystem::create_directory(target));
  const std::string marker = "junction target remains untouched";
  {
    std::ofstream out(target / "marker.txt", std::ios::binary);
    out << marker;
    CHECK(out);
  }
  CHECK(std::filesystem::is_empty(root));
  auto capability = platform::make_rooted_files(root.string());
  CHECK(capability);
  std::vector<std::filesystem::path> sentinels;
  for (const auto &entry : std::filesystem::directory_iterator(root))
    sentinels.push_back(entry.path());
  CHECK(sentinels.size() == 1);
  const auto &sentinel = sentinels.front();
  CHECK(sentinel.filename().string().starts_with(".race-root-pin-"));
  const DWORD attributes = GetFileAttributesW(sentinel.c_str());
  CHECK(attributes != INVALID_FILE_ATTRIBUTES);
  CHECK((attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
  CHECK(!DeleteFileW(sentinel.c_str()));
  CHECK_ERROR(capability.value()->read(sentinel.filename().string(), 1), ErrorCode::InvalidInput);
  std::error_code rename_error;
  std::filesystem::rename(root, fixture / "empty-root-moved", rename_error);
  CHECK(rename_error);
  const auto blocked = set_junction(root, target);
  // On an unexpected success remove the junction itself before failing, never
  // recursively enumerate its target. Dropping the capability releases its pins.
  if (blocked.created) {
    capability.value().reset();
    CHECK(RemoveDirectoryW(root.c_str()));
  }
  CHECK(!blocked.created);
  CHECK(blocked.error == ERROR_DIR_NOT_EMPTY);
  capability.value().reset();
  CHECK(std::filesystem::is_empty(root));
  std::filesystem::rename(root, fixture / "empty-root-moved", rename_error);
  CHECK(!rename_error);
  std::filesystem::rename(fixture / "empty-root-moved", root, rename_error);
  CHECK(!rename_error);
  // Positive control proves the denied operation above was not a privilege or
  // unsupported-environment failure. This fixture is on the adapter's NTFS lane.
  const auto created = set_junction(root, target);
  if (!created.created)
    std::fprintf(stderr, "junction positive control failed: %lu\n", created.error);
  CHECK(created.created);
  CHECK((GetFileAttributesW(root.c_str()) & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
  auto rejected = platform::make_rooted_files(root.string());
  const bool was_rejected = !rejected;
  if (rejected)
    rejected.value().reset();
  // Explicit native unlink of the junction precedes all recursive cleanup.
  CHECK(RemoveDirectoryW(root.c_str()));
  CHECK(was_rejected);
  std::ifstream input(target / "marker.txt", std::ios::binary);
  const std::string observed((std::istreambuf_iterator<char>(input)), {});
  CHECK(observed == marker);
}
std::filesystem::path long_fixture_path(std::filesystem::path directory) {
  // The temp root and timestamp length vary. Bound the components, not the
  // assumed prefix length, while retaining the >350 UTF-16-unit test case.
  for (unsigned i = 0; i < 8 && directory.native().size() <= 350; ++i)
    directory /= L"\u03bb-" + std::wstring(42, L'x') + std::to_wstring(i);
  return directory;
}
void long_path_operations(const std::filesystem::path &fixture, platform::Services &services) {
  namespace fs = std::filesystem;
  namespace watch = platform::directory_watch;
  const auto owned = fs::canonical(fixture);
  CHECK(owned.is_absolute());
  const auto top = owned / "long-content";
  CHECK(top.parent_path() == owned && top.filename() == "long-content");
  const fs::path short_prefix(L"C:\\x");
  const auto boundary = short_prefix / std::wstring(170, L'x') / std::wstring(174, L'y');
  CHECK(boundary.native().size() == 350);
  for (const auto &prefix : {short_prefix, boundary}) {
    const auto bounded = long_fixture_path(prefix);
    CHECK(bounded.native().size() > 350 && bounded.native().size() <= 396);
  }
  const auto directory = long_fixture_path(top);
  CHECK(directory.wstring().size() > 350);
  const fs::path extended(L"\\\\?\\" + directory.wstring());
  CHECK(fs::create_directories(extended));
  const auto encoded = directory.u8string();
  const std::string dos(reinterpret_cast<const char *>(encoded.data()), encoded.size());
  auto root = platform::make_rooted_files(dos);
  CHECK(root);
  const std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}},
      second{std::byte{4}, std::byte{5}, std::byte{6}};
  const auto leaf = std::string(89, 'a') + ".rasset";
  CHECK(leaf.size() == 96);
  CHECK(root.value()->publish_immutable(leaf, first));
  CHECK(root.value()->publish_immutable(leaf, first));
  CHECK_ERROR(root.value()->publish_immutable(leaf, second), ErrorCode::Duplicate);
  CHECK(root.value()->read(leaf, 3).value() == std::vector<std::byte>(first.begin(), first.end()));
  CHECK_ERROR(root.value()->read(std::string(97, 'a'), 3), ErrorCode::InvalidInput);
  auto made = watch::make_windows_service(root.value(), 1001);
  CHECK(made);
  auto watcher = std::move(made).value();
  auto before = watcher->poll();
  CHECK(before && !before.value().failure);
  // The initial observation is already dirty; a later completed batch must
  // still be observable after real publication below.
  CHECK(root.value()->replace("catalog.bin", second, platform::Durability::AtomicVisibility));
  CHECK(services.files->replace(dos + "/settings.bin", first, platform::Durability::AtomicVisibility));
  CHECK(services.files->read(dos + "/settings.bin", 3).value() ==
        std::vector<std::byte>(first.begin(), first.end()));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    auto now = watcher->poll();
    CHECK(now && !now.value().failure);
    if (now.value().completed_batches > before.value().completed_batches)
      break;
    CHECK(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  CHECK(watcher->request_stop());
  CHECK(watcher->wait_stopped(5000));
  watcher.reset();
  root.value().reset();
  for (const auto &item : fs::directory_iterator(extended))
    CHECK(!item.path().filename().wstring().starts_with(L".race-stage-"));
  // Only this newly created, named fixture subtree is removed, through its
  // extended absolute path. The canonical parent and fixed leaf were checked.
  CHECK(top.parent_path() == owned && top.filename() == "long-content");
  CHECK(fs::remove_all(fs::path(L"\\\\?\\" + top.wstring())) > 0);
}
} // namespace
void platform_tests() {
  auto created = platform::make_services();
  CHECK(created);
  auto services = std::move(created).value();
  const auto a = services.clock->now(), b = services.clock->now();
  CHECK(a.domain == b.domain && b.nanoseconds >= a.nanoseconds);
  std::atomic<int> value{0};
  auto worker = services.threads->start([&] { value = 42; });
  CHECK(worker);
  CHECK(worker.value()->join());
  CHECK(worker.value()->join());
  CHECK(value == 42);
  CHECK_ERROR(services.threads->start({}), ErrorCode::InvalidInput);
  const auto root =
      std::filesystem::temp_directory_path() /
      std::filesystem::path("race-windows-" +
                            std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  CHECK(std::filesystem::create_directory(root));
  CHECK(std::filesystem::create_directory(root / "content"));
  const auto path = (root / "settings.txt").string();
  const std::string data = "atomic settings";
  auto bytes = std::as_bytes(std::span(data));
  CHECK(services.files->replace(path, bytes, platform::Durability::AtomicVisibility));
  auto read = services.files->read(path, 128);
  CHECK(read && read.value() == std::vector<std::byte>(bytes.begin(), bytes.end()));
  CHECK_ERROR(services.files->read(path, 2), ErrorCode::Exhausted);
  CHECK_ERROR(services.files->read("relative", 128), ErrorCode::InvalidInput);
  CHECK_ERROR(services.files->read(path + std::string("\0tail", 5), 128), ErrorCode::InvalidInput);
  CHECK_ERROR(services.files->replace(path, {}, platform::Durability::Durable), ErrorCode::Unsupported);
  CHECK_ERROR(services.files->replace(path, {}, static_cast<platform::Durability>(99)),
              ErrorCode::InvalidInput);
  CHECK(!services.files->replace((root / "missing" / "x").string(), bytes,
                                 platform::Durability::AtomicVisibility));
  CHECK(services.files->read(path, 128).value() == read.value());
  CHECK(!services.files->read(root.string(), 128));
  for (const auto *name : {"CON", "nul.txt", "COM1.bin", "bad?name"})
    CHECK_ERROR(services.files->read((root / name).string(), 128), ErrorCode::InvalidInput);
  auto content = platform::make_rooted_files((root / "content").string());
  CHECK(content);
  const std::array<std::byte, 3> first{std::byte{1}, std::byte{2}, std::byte{3}};
  CHECK(content.value()->publish_immutable("one.bin", first));
  CHECK(content.value()->publish_immutable("one.bin", first));
  const std::array<std::byte, 3> second{std::byte{4}, std::byte{5}, std::byte{6}};
  CHECK_ERROR(content.value()->publish_immutable("one.bin", second), ErrorCode::Duplicate);
  CHECK(content.value()->read("one.bin", 3).value() == std::vector<std::byte>(first.begin(), first.end()));
  CHECK_ERROR(content.value()->read("one.bin", 2), ErrorCode::Exhausted);
  for (const auto *leaf : {"../one.bin", "x/y", "one:stream", "con", "nul.txt", "lpt1.bin", "file."})
    CHECK(!content.value()->read(leaf, 128));
  CHECK(content.value()->replace("catalog.bin", first, platform::Durability::AtomicVisibility));
  CHECK(content.value()->replace("catalog.bin", second, platform::Durability::AtomicVisibility));
  std::error_code error;
  std::filesystem::rename(root / "content", root / "moved", error);
  CHECK(error);
  CHECK(content.value()->read("catalog.bin", 3).value() ==
        std::vector<std::byte>(second.begin(), second.end()));
  CHECK_ERROR(content.value()->replace("catalog.bin", {}, platform::Durability::Durable),
              ErrorCode::Unsupported);
  content.value().reset();
  std::filesystem::rename(root / "content", root / "moved");
  for (const auto &f : std::filesystem::directory_iterator(root / "moved"))
    CHECK(!f.path().filename().string().starts_with(".race-stage-"));
  CHECK(services.files->replace(path, {}, platform::Durability::AtomicVisibility));
  CHECK(services.files->read(path, 128).value().empty());
  empty_root_confinement(root);
  long_path_operations(root, services);
  const auto resolved_root = std::filesystem::canonical(root);
  CHECK(resolved_root.is_absolute() &&
        resolved_root.parent_path() == std::filesystem::canonical(std::filesystem::temp_directory_path()));
  CHECK(resolved_root.filename().string().starts_with("race-windows-"));
  std::filesystem::remove_all(resolved_root);
  auto memory = platform::memory_info();
  CHECK(memory && memory.value().physical_bytes > 0 && memory.value().resident_bytes > 0);
}
