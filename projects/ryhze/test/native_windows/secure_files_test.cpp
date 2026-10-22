#include "secure_files.h"

#include <winioctl.h>

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <utility>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace ryhze::secure_files;
int skipped = 0;

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(std::string(message) +
                                      " (Win32=" + std::to_string(GetLastError()) + ")");
}
std::wstring Join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }
void Directory(const std::wstring& path) { Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "fixture mkdir"); }
void RawCreate(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "fixture create");
  DWORD count = 0;
  const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
                    count == bytes.size() && FlushFileBuffers(file);
  CloseHandle(file);
  Check(okay, "fixture write");
}
std::vector<uint8_t> RawRead(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "fixture read open");
  LARGE_INTEGER length{};
  Check(GetFileSizeEx(file, &length) && length.QuadPart >= 0 && length.QuadPart < 1024 * 1024,
        "fixture read bound");
  std::vector<uint8_t> bytes(static_cast<size_t>(length.QuadPart));
  DWORD count = 0;
  const bool okay = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
                    count == bytes.size();
  CloseHandle(file);
  Check(okay, "fixture read");
  return bytes;
}

struct Fixture {
  std::wstring base;
  std::wstring root;
  std::wstring outside;
  Fixture() {
    wchar_t temp[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    Check(length > 0 && length < std::size(temp), "OS temporary path");
    static std::atomic<unsigned> sequence{0};
    std::wstring created;
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      created = std::wstring(temp) + L"ryhze-secure-files-" + std::to_wstring(GetCurrentProcessId()) +
                L"-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
      if (CreateDirectoryW(created.c_str(), nullptr)) break;
      Check(GetLastError() == ERROR_ALREADY_EXISTS && attempt < 15, "unique fixture directory");
    }
    // Resolve only the OS temporary base/fixture before exercising app-root
    // rejection. Tests never operate on existing application or credential data.
    HANDLE handle = CreateFileW(created.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "fixture canonical handle");
    wchar_t canonical[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(handle, canonical,
        static_cast<DWORD>(std::size(canonical)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    Check(count > 4 && count < std::size(canonical), "fixture canonical path");
    base = std::wstring(canonical, count);
    Check(base.rfind(L"\\\\?\\", 0) == 0, "local fixture namespace");
    base.erase(0, 4);
    root = Join(base, L"app");
    outside = Join(base, L"outside");
    Directory(root);
    Directory(outside);
    std::wcout << L"FIXTURE retained: " << base << L"\n";
  }
  // Deliberately retain successful and failed bounded fixtures for inspection.
};

Root Open(const std::wstring& path) {
  Error error;
  auto root = Root::Open(path, false, &error);
  if (!root) throw std::runtime_error("Root::Open code=" + std::to_string(static_cast<int>(error.code)) +
                                      " win32=" + std::to_string(error.win32));
  return std::move(*root);
}

bool Junction(const std::wstring& link, const std::wstring& target) {
  Directory(link);
  HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Check(handle != INVALID_HANDLE_VALUE, "junction fixture handle");
  struct Data {
    DWORD tag;
    WORD length;
    WORD reserved;
    WORD substitute_offset;
    WORD substitute_length;
    WORD print_offset;
    WORD print_length;
    wchar_t names[2048];
  } data{};
  const auto substitute = L"\\??\\" + target;
  Check(substitute.size() + target.size() + 2 < std::size(data.names), "junction fixture bound");
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
  data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
  std::copy(substitute.begin(), substitute.end(), data.names);
  std::copy(target.begin(), target.end(), data.names + substitute.size() + 1);
  data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
  DWORD returned = 0;
  const bool okay = DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, &data,
      static_cast<DWORD>(8 + data.length), nullptr, 0, &returned, nullptr) != FALSE;
  const DWORD error = GetLastError();
  CloseHandle(handle);
  if (!okay && (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_ACCESS_DENIED)) {
    ++skipped;
    std::cout << "SKIP junction fixture: Win32=" << error << "\n";
    return false;
  }
  SetLastError(error);
  Check(okay, "junction fixture reparse creation");
  return true;
}

void GrammarAndBounds() {
  Fixture fixture;
  Error error;
  for (const auto& path : {L"relative", L"C:relative", L"C:\\", L"\\\\server\\share\\dir",
                           L"\\\\?\\C:\\dir", L"\\\\.\\C:\\dir", L"C:/dir", L"C:\\NUL.txt"}) {
    Check(!Root::Open(path, true, &error), "reject absolute grammar");
  }
  auto root = Open(fixture.root);
  const std::vector<std::wstring> invalid = {L"", L".", L"..", L"..\\outside\\x", L"a\\..\\x",
      L"a/../x", L"C:\\x", L"\\x", L"a\\\\x", L"a\\", L"a:stream", L"a.", L"a ",
      L"NUL", L"con.txt", L"COM1", L"LPT9.bin", L"COM\u00b9", L"CONIN$", L"x\ny",
      std::wstring(L"a\0b", 3), std::wstring(256, L'a'), std::wstring(1, wchar_t{0xd800})};
  FileInfo info;
  std::vector<uint8_t> bytes{42};
  for (const auto& name : invalid) {
    Check(!root.CreateExclusive(name, {1}, &info, &error), "reject relative create grammar");
    Check(!root.EnsureDirectory(name, &error), "reject relative mkdir grammar");
    Check(!root.Read(name, 32, &bytes, &info, &error) && bytes.empty(), "reject relative read grammar");
  }
  Check(root.EnsureDirectory(L"scope\\child", &error), "safe nested mkdir");
  Check(root.CreateExclusive(L"scope\\child\\payload.bin", {1, 2, 3}, &info, &error), "exclusive creation");
  const auto identity = info.identity;
  Check(!root.CreateExclusive(L"scope\\child\\payload.bin", {9}, &info, &error) &&
        error.code == ErrorCode::kAlreadyExists, "collision refuses overwrite");
  bytes = {99};
  Check(!root.Read(L"scope\\child\\payload.bin", 2, &bytes, &info, &error) &&
        error.code == ErrorCode::kTooLarge && bytes.empty(), "bounded read clears output");
  Check(root.Read(L"scope\\child\\payload.bin", 3, &bytes, &info, &error) &&
        bytes == std::vector<uint8_t>({1, 2, 3}) && info.identity == identity, "same-handle bytes and identity");
  Check(!root.Read(L"scope\\child\\payload.bin", Root::kMaximumBytes + 1, &bytes, &info, &error),
        "maximum read budget");
  Check(root.CreateExclusive(L"empty", {}, &info, &error) &&
        root.Read(L"empty", 0, &bytes, &info, &error) && bytes.empty(), "empty file roundtrip");
  const auto new_root = Join(fixture.base, L"new-root");
  Check(Root::Open(new_root, true, &error).has_value(), "create final root component");
  Check(!Root::Open(Join(fixture.base, L"missing\\nested"), true, &error), "do not create root ancestors");
}

void IdentityAndHardlinks() {
  Fixture fixture;
  const auto external = Join(fixture.outside, L"sentinel");
  RawCreate(external, {7, 8});
  Check(CreateHardLinkW(Join(fixture.root, L"hardlink").c_str(), external.c_str(), nullptr) != FALSE,
        "hardlink fixture");
  auto root = Open(fixture.root);
  Error error;
  FileInfo info;
  Check(root.CreateExclusive(L"owned", {1}, &info, &error), "owned fixture");
  const auto original = info.identity;
  auto wrong = original;
  wrong.id[0] ^= 1;
  Check(!root.RemoveOwned(L"owned", wrong, &error) && error.code == ErrorCode::kIdentityMismatch,
        "wrong file ID refused");
  wrong = original;
  wrong.volume ^= 1;
  Check(!root.RemoveOwned(L"owned", wrong, &error), "wrong volume refused");
  Check(root.RemoveOwned(L"owned", original, &error), "handle-based owned removal");
  Check(root.CreateExclusive(L"owned", {2}, &info, &error), "replacement fixture");
  Check(!root.RemoveOwned(L"owned", original, &error), "stale identity cannot remove replacement");
  Check(RawRead(Join(fixture.root, L"owned")) == std::vector<uint8_t>({2}), "replacement survives");
  Check(root.EnsureDirectory(L"directory", &error) && root.Inspect(L"directory", &info, &error), "directory inspection");
  Check(!root.RemoveOwned(L"directory", info.identity, &error), "directory deletion absent");
  Check(root.Inspect(L"hardlink", &info, &error) && info.link_count >= 2, "hardlink inspection");
  Check(!root.RemoveOwned(L"hardlink", info.identity, &error) && error.code == ErrorCode::kHardLink,
        "hardlink mutation refused");
  std::vector<uint8_t> bytes;
  Check(!root.Read(L"hardlink", 32, &bytes, &info, &error), "hardlink content read refused");
  Check(!root.CreateExclusive(L"hardlink", {9}, &info, &error), "hardlink overwrite refused");
  Check(RawRead(external) == std::vector<uint8_t>({7, 8}), "external hardlink target preserved");
}

void ReparseBoundaries() {
  Fixture fixture;
  RawCreate(Join(fixture.outside, L"sentinel"), {4, 5});
  const bool has_junction = Junction(Join(fixture.root, L"junction"), fixture.outside);
  const auto link = Join(fixture.root, L"file-link");
  const bool has_symlink = CreateSymbolicLinkW(link.c_str(), Join(fixture.outside, L"sentinel").c_str(),
                         SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != FALSE;
  if (!has_symlink) {
    const DWORD native = GetLastError();
    Check(native == ERROR_PRIVILEGE_NOT_HELD || native == ERROR_NOT_SUPPORTED || native == ERROR_INVALID_PARAMETER,
          "symlink fixture failure is known unavailable capability");
    ++skipped;
    std::cout << "SKIP file symlink fixture: Win32=" << native << "\n";
  }
  auto root = Open(fixture.root);
  Error error;
  FileInfo info;
  if (has_junction) {
    Check(!root.EnsureDirectory(L"junction\\escaped", &error), "junction mkdir refused");
    Check(!root.CreateExclusive(L"junction\\escaped.bin", {9}, &info, &error), "junction write refused");
    Check(!Root::Open(Join(fixture.root, L"junction\\child"), true, &error), "ancestor junction refused");
    Check(GetFileAttributesW(Join(fixture.outside, L"escaped.bin").c_str()) == INVALID_FILE_ATTRIBUTES,
          "outside write absent");
    Check(GetFileAttributesW(Join(fixture.outside, L"escaped").c_str()) == INVALID_FILE_ATTRIBUTES,
          "outside mkdir absent");
  }
  if (has_symlink) {
    std::vector<uint8_t> bytes;
    Check(!root.Read(L"file-link", 32, &bytes, &info, &error), "file reparse read refused");
    Check(!root.Inspect(L"file-link", &info, &error), "file reparse inspection refused");
    Check(!root.CreateExclusive(L"file-link", {9}, &info, &error), "file reparse overwrite refused");
  }
  Check(RawRead(Join(fixture.outside, L"sentinel")) == std::vector<uint8_t>({4, 5}), "reparse target preserved");
}

void PinningAndLeases() {
  Fixture fixture;
  auto root = std::make_optional(Open(fixture.root));
  auto second = std::make_optional(Open(fixture.root));
  Error error;
  Check(root->EnsureDirectory(L"scope", &error), "lease parent");
  auto lease = root->Lock(L"scope\\lease", &error);
  Check(lease.has_value(), "lease acquired");
  Check(!second->Lock(L"scope\\lease", &error) && error.code == ErrorCode::kBusy,
        "second owner blocked");
  std::atomic<bool> root_renamed{false};
  std::atomic<bool> scope_renamed{false};
  std::thread attacker([&] {
    root_renamed = MoveFileExW(fixture.root.c_str(), Join(fixture.base, L"moved-app").c_str(), 0) != FALSE;
    scope_renamed = MoveFileExW(Join(fixture.root, L"scope").c_str(), Join(fixture.root, L"moved-scope").c_str(), 0) != FALSE;
  });
  attacker.join();
  Check(!root_renamed && !scope_renamed, "held root and child ancestors resist rename");
  lease.reset();
  auto next = second->Lock(L"scope\\lease", &error);
  Check(next.has_value(), "lease available after release");
  next.reset();
  // Win32's path-based rename also requests parent-directory rights. Release
  // both root owners before asserting the namespace is mutable again.
  root.reset();
  second.reset();
  Check(MoveFileExW(Join(fixture.root, L"scope").c_str(), Join(fixture.root, L"moved-scope").c_str(), 0) != FALSE,
        "namespace pins released after every owner");
  auto reopened = Open(fixture.root);
  FileInfo info;
  Check(!reopened.CreateExclusive(L"scope\\stale", {1}, &info, &error), "stale child path not followed");
  Check(GetFileAttributesW(Join(fixture.root, L"moved-scope\\stale").c_str()) == INVALID_FILE_ATTRIBUTES,
        "no write through stale authority");
}

void PinnedReadHandle() {
  Fixture fixture;
  Error error;
  auto root = Root::Open(fixture.root, false, &error);
  Check(root.has_value(), "read root");
  FileInfo created;
  Check(root->EnsureDirectory(L"scope", &error) &&
        root->CreateExclusive(L"scope\\signed.bin", {1, 3, 5}, &created, &error), "read lease fixture");
  Check(!root->OpenRead(L"scope\\signed.bin", 2, &error), "read lease budget");
  auto lease = root->OpenRead(L"scope\\signed.bin", 3, &error);
  Check(lease.has_value() && lease->info().identity == created.identity, "read handle identity");
  root.reset();
  const auto path = Join(fixture.root, L"scope\\signed.bin");
  HANDLE writer = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
  Check(writer == INVALID_HANDLE_VALUE, "read lease prevents writer");
  Check(!MoveFileExW(path.c_str(), Join(fixture.root, L"scope\\changed.bin").c_str(), 0),
        "read lease prevents file rename");
  Check(!MoveFileExW(fixture.root.c_str(), Join(fixture.base, L"moved").c_str(), 0),
        "read lease owns root lifetime");
  uint8_t bytes[3]{};
  DWORD count = 0;
  Check(ReadFile(lease->handle(), bytes, sizeof(bytes), &count, nullptr) && count == 3 &&
        bytes[0] == 1 && bytes[1] == 3 && bytes[2] == 5, "borrowed handle exact bytes");
  lease.reset();
  Check(MoveFileExW(path.c_str(), Join(fixture.root, L"scope\\changed.bin").c_str(), 0) != FALSE,
        "read pins released");
}

void ExactRemovalLeases() {
  Fixture fixture;
  const auto slot_path = Join(fixture.root, L"slot");
  Directory(slot_path);
  RawCreate(Join(slot_path, L"owned.bin"), {1, 2, 3});
  RawCreate(Join(slot_path, L"unknown.bin"), {9});
  RawCreate(Join(fixture.outside, L"sentinel"), {8});
  Check(CreateHardLinkW(Join(slot_path, L"hardlink.bin").c_str(),
                       Join(fixture.outside, L"sentinel").c_str(), nullptr) != FALSE, "removal hardlink fixture");
  auto root = std::make_optional(Open(fixture.root));
  Error error;
  auto read_only = root->OpenDirectory(L"slot", false, &error);
  Check(read_only.has_value(), "read-only pinned directory");
  std::vector<std::wstring> names;
  Check(read_only->ListNames(3, &names, &error) && names.size() == 3, "bounded handle enumeration");
  Check(!read_only->ListNames(2, &names, &error) && names.empty(), "enumeration limit preserves outputs");
  Check(!read_only->OpenForRemoval(L"owned.bin", 3, &error) && !read_only->RemoveEmpty(&error),
        "read-only lease cannot delete");
  Check(!root->OpenDirectory(L"slot", true, &error), "read pin excludes removal authority");
  read_only.reset();
  auto slot = root->OpenDirectory(L"slot", true, &error);
  Check(slot.has_value(), "explicit removal directory pin");
  const auto identity = slot->info().identity;
  Check(!slot->OpenForRemoval(L"..\\outside\\sentinel", 3, &error) &&
        !slot->OpenForRemoval(L"hardlink.bin", 3, &error) &&
        !slot->OpenForRemoval(L"owned.bin", 2, &error), "remove acquisition enforces direct child hardlink and bound");
  auto file = slot->OpenForRemoval(L"owned.bin", 3, &error);
  Check(file.has_value(), "same-handle verification and removal");
  uint8_t bytes[3]{}; DWORD read = 0;
  Check(ReadFile(file->handle(), bytes, sizeof(bytes), &read, nullptr) && read == 3 && bytes[1] == 2,
        "delete handle reads exact contents");
  HANDLE writer = CreateFileW(Join(slot_path, L"owned.bin").c_str(), GENERIC_WRITE,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
  Check(writer == INVALID_HANDLE_VALUE, "delete verification pin excludes writer");
  Check(!MoveFileW(slot_path.c_str(), Join(fixture.root, L"replaced-slot").c_str()), "delete directory cannot be substituted");
  Check(!slot->RemoveEmpty(&error), "live child pin prevents directory removal");
  Check(file->Remove(&error) && !file->Remove(&error), "one exact file removed once");
  file.reset();
  Check(!slot->RemoveEmpty(&error), "unknown remaining children preserved");
  Check(slot->info().identity == identity && RawRead(Join(slot_path, L"unknown.bin")) == std::vector<uint8_t>({9}) &&
        RawRead(Join(fixture.outside, L"sentinel")) == std::vector<uint8_t>({8}), "unknown and outside bytes preserved");
  slot.reset();
  Check(root->EnsureDirectory(L"empty-slot", &error), "empty fixture");
  slot = root->OpenDirectory(L"empty-slot", true, &error);
  Check(slot && slot->RemoveEmpty(&error) &&
        GetFileAttributesW(Join(fixture.root, L"empty-slot").c_str()) == INVALID_FILE_ATTRIBUTES,
        "same empty directory removed without recursive API");
  slot.reset();
  Check(root->EnsureDirectory(L"stream-slot", &error), "stream fixture");
  root.reset();
  RawCreate(Join(fixture.root, L"stream-slot\\file.bin"), {4});
  RawCreate(Join(fixture.root, L"stream-slot\\file.bin:unknown"), {5});
  root = std::make_optional(Open(fixture.root));
  slot = root->OpenDirectory(L"stream-slot", true, &error);
  Check(slot && !slot->OpenForRemoval(L"file.bin", 1, &error) && !slot->OpenRead(L"file.bin", 1, &error),
        "unrecorded stream prevents plan or deletion");
  Check(RawRead(Join(fixture.root, L"stream-slot\\file.bin:unknown")) == std::vector<uint8_t>({5}),
        "alternate stream preserved");
}
// INERT REVIEW DRAFT. Not compiled, formatted, linked or executed.
// Proposed additions to test/native_windows/secure_files_test.cpp, inside its
// existing anonymous namespace. Reuses Fixture, Check, Join, RawCreate, RawRead,
// Directory and Open. Add <chrono>, <condition_variable>, <mutex>, <optional>
// and <utility> to that test's includes; its existing includes supply the rest.
//
// Add these main() invocations and update the reported suite count from 6 to 13:
//   StreamingKnownDigests();
//   StreamingBoundsAndPoison();
//   StreamingFailuresPreserveBytes();
//   StreamingOwnerPinsAndCollisions();
//   StreamingReentrantCalls();
//   StreamingFinishCancellation();
//   StreamingConcurrentCancellation();
//
// If secure_files.cpp now calls CNG, explicitly add bcrypt to secure_files_test,
// steam_inventory_test and host_installations_test in native CMake. Other
// current consumers and the app runner already link bcrypt. Do not rely on
// dead-section stripping to hide unresolved references.

// Independent published SHA-256 test vectors; no production helper computes
// the expected answers. These are integrity fixtures, not signature authority.
const FileDigest kStreamingAbcSha256 = {
    0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
    0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
    0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
    0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
const FileDigest kStreamingEmptySha256 = {
    0xe3, 0xb0, 0xc4, 0x42, 0x98, 0xfc, 0x1c, 0x14,
    0x9a, 0xfb, 0xf4, 0xc8, 0x99, 0x6f, 0xb9, 0x24,
    0x27, 0xae, 0x41, 0xe4, 0x64, 0x9b, 0x93, 0x4c,
    0xa4, 0x95, 0x99, 0x1b, 0x78, 0x52, 0xb8, 0x55};
// NIST SHA-256 vector: exactly 1,000,000 repetitions of ASCII 'a'.
const FileDigest kStreamingMillionASha256 = {
    0xcd, 0xc7, 0x6e, 0x5c, 0x99, 0x14, 0xfb, 0x92,
    0x81, 0xa1, 0xc7, 0xe2, 0x84, 0xd7, 0x3e, 0x67,
    0xf1, 0x80, 0x9a, 0x48, 0xa4, 0x97, 0x20, 0x0e,
    0x04, 0x6d, 0x39, 0xcc, 0xc7, 0x11, 0x2c, 0xd0};
const std::array<uint8_t, 3> kStreamingAbc = {'a', 'b', 'c'};

void StreamingKnownDigests() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "streaming directory fixture");
  auto writer = directory->BeginWriteExclusive(L"abc.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->written() == 0, "new streaming writer");
  const auto created_identity = writer->created_identity();
  Check(writer->Append(kStreamingAbc.data(), 1, &error) &&
        writer->Append(kStreamingAbc.data() + 1, 2, &error) &&
        writer->written() == 3, "split appends retain exact length");
  FileInfo verified;
  Check(writer->Finish(&verified, &error) && verified.size == 3 &&
        !verified.is_directory && verified.link_count == 1 &&
        verified.identity == created_identity,
        "same-handle finish accepts independent abc digest");
  const auto identity = verified.identity;
  Check(!writer->Append(kStreamingAbc.data(), 1, &error) &&
        error.code == ErrorCode::kInvalidPath, "finished writer rejects append");
  Check(!writer->Finish(&verified, &error) &&
        error.code == ErrorCode::kInvalidPath, "finish cannot be replayed");
  writer.reset();
  std::vector<uint8_t> observed;
  Check(root.Read(L"stream\\abc.bin", 3, &observed, &verified, &error) &&
        observed == std::vector<uint8_t>({'a', 'b', 'c'}) &&
        verified.identity == identity, "finished file bytes and identity remain exact");

  writer = directory->BeginWriteExclusive(L"empty.bin", 0,
                                          kStreamingEmptySha256, &error);
  Check(writer.has_value() && writer->Append(nullptr, 0, &error) &&
        writer->written() == 0 && writer->Finish(&verified, &error) &&
        verified.size == 0, "empty file uses independent empty digest");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\empty.bin")).empty(),
        "empty verified file retained");

  writer = directory->BeginWriteExclusive(L"moved.bin", 3,
                                          kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 3, &error),
        "movable streaming writer fixture");
  {
    auto moved = std::move(*writer);
    Check(!writer->Append(kStreamingAbc.data(), 1, &error) &&
          error.code == ErrorCode::kInvalidPath, "moved-from writer is inert");
    Check(moved.written() == 3 && moved.Finish(&verified, &error),
          "move retains exact pending write ownership");
  }
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\moved.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "moved writer preserves bytes");

  std::wstring mutable_child = L"snapshot.bin";
  auto mutable_digest = kStreamingAbcSha256;
  bool mutated = false;
  writer = directory->BeginWriteExclusive(mutable_child, 3, mutable_digest, &error, [&] {
    if (!mutated) {
      mutated = true;
      mutable_child = L"..\\..\\outside\\redirected.bin";
      mutable_digest = kStreamingEmptySha256;
    }
    return false;
  });
  Check(GetFileAttributesW(Join(fixture.outside, L"redirected.bin").c_str()) ==
        INVALID_FILE_ATTRIBUTES, "callback mutation cannot redirect creation outside root");
  Check(mutated && writer.has_value() &&
        writer->Append(kStreamingAbc.data(), 3, &error) &&
        writer->Finish(&verified, &error),
        "creation snapshots child and expected digest before invoking callback");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\snapshot.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "original child and digest retain their binding");

  constexpr uint64_t million = 1000000;
  writer = directory->BeginWriteExclusive(L"million-a.bin", million,
                                          kStreamingMillionASha256, &error);
  Check(writer.has_value(), "independent multi-read SHA-256 fixture");
  const std::vector<uint8_t> repeated(65536, 'a');
  for (uint64_t offset = 0; offset < million;) {
    const auto count = static_cast<size_t>((std::min)(
        uint64_t{repeated.size()}, million - offset));
    Check(writer->Append(repeated.data(), count, &error),
          "million-a vector streams through bounded appends");
    offset += count;
  }
  Check(writer->written() == million && writer->Finish(&verified, &error) &&
        verified.size == million,
        "known million-a digest covers multiple bounded readback chunks");
  writer.reset();
  const auto million_bytes = RawRead(Join(fixture.root, L"stream\\million-a.bin"));
  Check(million_bytes.size() == million &&
        std::all_of(million_bytes.begin(), million_bytes.end(),
                    [](uint8_t value) { return value == 'a'; }),
        "multi-chunk verified file retains exact independent vector bytes");
}

void StreamingBoundsAndPoison() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "bounded streaming directory");
  FileDigest unused_digest{};
  Check(!directory->BeginWriteExclusive(L"over-file-limit.bin",
      FileWriteLease::kMaximumFileBytes + uint64_t{1}, unused_digest, &error) &&
      error.code == ErrorCode::kTooLarge, "declared file size limit checked before creation");
  FileInfo info;
  Check(!root.Inspect(L"stream\\over-file-limit.bin", &info, &error) &&
        error.code == ErrorCode::kNotFound, "oversized declaration leaves no child");
  auto writer = directory->BeginWriteExclusive(L"maximum-declaration.bin",
      FileWriteLease::kMaximumFileBytes, unused_digest, &error);
  Check(writer.has_value() && writer->written() == 0,
        "one GiB declaration needs no one GiB allocation or write");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\maximum-declaration.bin")).empty(),
        "unfinished declaration retains its empty partial");

  std::vector<uint8_t> chunk(FileWriteLease::kMaximumChunkBytes + 1, 0);
  writer = directory->BeginWriteExclusive(L"chunk-boundary.bin",
      static_cast<uint64_t>(chunk.size()), unused_digest, &error);
  Check(writer.has_value() &&
        writer->Append(chunk.data(), FileWriteLease::kMaximumChunkBytes, &error) &&
        writer->Append(chunk.data(), 1, &error) && writer->written() == chunk.size(),
        "maximum bounded chunk and final one-byte append accepted");
  writer->Cancel();
  Check(!writer->Finish(&info, &error) && error.win32 == ERROR_OPERATION_ABORTED,
        "cancelled maximum-chunk fixture cannot finalize");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\chunk-boundary.bin")) == chunk,
        "cancellation preserves every previously written byte");

  writer = directory->BeginWriteExclusive(L"oversized-chunk.bin",
      static_cast<uint64_t>(chunk.size()), unused_digest, &error);
  Check(writer.has_value() && !writer->Append(chunk.data(), chunk.size(), &error) &&
        error.code == ErrorCode::kTooLarge && writer->written() == 0,
        "oversized chunk rejected before any write");
  Check(!writer->Append(chunk.data(), 1, &error), "invalid chunk poisons writer");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\oversized-chunk.bin")).empty(),
        "rejected chunk leaves exact empty partial");

  writer = directory->BeginWriteExclusive(L"declared-overflow.bin", 2,
                                          kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 1, &error),
        "declared-size overflow prefix");
  Check(!writer->Append(kStreamingAbc.data() + 1, 2, &error) &&
        error.code == ErrorCode::kTooLarge && writer->written() == 1,
        "remaining-size check rejects whole overflowing append");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\declared-overflow.bin")) ==
        std::vector<uint8_t>({'a'}), "overflow preserves only prior prefix");

  writer = directory->BeginWriteExclusive(L"null-buffer.bin", 1,
                                          unused_digest, &error);
  Check(writer.has_value() && !writer->Append(nullptr, 1, &error) &&
        writer->written() == 0, "nonempty null buffer rejected without dereference");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\null-buffer.bin")).empty(),
        "invalid buffer never deletes its partial");
}

void StreamingFailuresPreserveBytes() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "failed streaming directory");
  auto writer = directory->BeginWriteExclusive(L"short.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 1, &error),
        "short finalize fixture");
  FileInfo info;
  info.size = 999;
  Check(!writer->Finish(&info, &error) &&
        error.code == ErrorCode::kIdentityMismatch && info.size == 0,
        "short finish returns no verified observation");
  Check(!writer->Append(kStreamingAbc.data() + 1, 2, &error),
        "short finalize cannot resume through same lease");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\short.bin")) ==
        std::vector<uint8_t>({'a'}), "short file remains for explicit recovery");

  auto wrong = kStreamingAbcSha256;
  wrong[0] ^= 1;
  writer = directory->BeginWriteExclusive(L"wrong-hash.bin", 3, wrong, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 3, &error),
        "wrong digest fixture");
  info.size = 999;
  Check(!writer->Finish(&info, &error) &&
        error.code == ErrorCode::kIdentityMismatch && info.size == 0,
        "reread digest mismatch cannot publish success");
  Check(!writer->Finish(&info, &error), "failed digest cannot be retried as success");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\wrong-hash.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "wrong digest does not erase file");

  Check(!directory->BeginWriteExclusive(L"cancel-before.bin", 3,
      kStreamingAbcSha256, &error, [] { return true; }) &&
      error.win32 == ERROR_OPERATION_ABORTED, "pre-cancelled create refuses effects");
  Check(!root.Inspect(L"stream\\cancel-before.bin", &info, &error) &&
        error.code == ErrorCode::kNotFound, "pre-cancelled child absent");
  writer = directory->BeginWriteExclusive(L"cancel-prefix.bin", 3,
                                          kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 1, &error),
        "cancel prefix fixture");
  Check(!writer->Append(kStreamingAbc.data() + 1, 2, &error, [] { return true; }) &&
        error.win32 == ERROR_OPERATION_ABORTED && writer->written() == 1,
        "callback cancellation preserves prior prefix without next write");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\cancel-prefix.bin")) ==
        std::vector<uint8_t>({'a'}), "cancelled prefix retained");
  Check(!directory->BeginWriteExclusive(L"cancel-prefix.bin", 3,
      kStreamingAbcSha256, &error) && error.code == ErrorCode::kAlreadyExists,
      "new writer cannot replace retained partial");
}

void StreamingOwnerPinsAndCollisions() {
  Fixture fixture;
  const auto path = Join(fixture.root, L"stream");
  Directory(path);
  RawCreate(Join(path, L"unknown.bin"), {7, 8});
  RawCreate(Join(fixture.outside, L"sentinel"), {9});
  Check(CreateHardLinkW(Join(path, L"hardlink.bin").c_str(),
      Join(fixture.outside, L"sentinel").c_str(), nullptr) != FALSE,
      "streaming hardlink collision fixture");
  auto root = std::make_optional(Open(fixture.root));
  Error error;
  auto directory = root->OpenDirectory(L"stream", false, &error);
  Check(directory.has_value(), "pinned streaming directory");
  for (const auto* name : {L"unknown.bin", L"hardlink.bin"}) {
    Check(!directory->BeginWriteExclusive(name, 3, kStreamingAbcSha256, &error),
          "streaming create never replaces an existing child");
  }
  for (const auto* name : {L"..\\outside\\escaped", L"sub\\child", L"a:stream", L"CON"}) {
    Check(!directory->BeginWriteExclusive(name, 3, kStreamingAbcSha256, &error) &&
          error.code == ErrorCode::kInvalidPath, "streaming requires one safe component");
  }
  auto writer = directory->BeginWriteExclusive(L"owned.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value(), "streaming pin fixture");
  root.reset();
  directory.reset();
  const auto owned = Join(path, L"owned.bin");
  for (DWORD access : {DWORD{GENERIC_READ}, DWORD{GENERIC_WRITE}, DWORD{DELETE}}) {
    HANDLE competing = CreateFileW(owned.c_str(), access,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (competing != INVALID_HANDLE_VALUE) CloseHandle(competing);
    Check(competing == INVALID_HANDLE_VALUE, "writer excludes competing file handles");
  }
  Check(!MoveFileW(owned.c_str(), Join(path, L"replaced.bin").c_str()),
        "writer pins exact file namespace");
  Check(!MoveFileW(path.c_str(), Join(fixture.root, L"moved-stream").c_str()),
        "writer retains directory owner after external lease release");
  Check(!MoveFileW(fixture.root.c_str(), Join(fixture.base, L"moved-app").c_str()),
        "writer retains root ancestor pins");
  Check(writer->Append(kStreamingAbc.data(), 3, &error), "writer survives owner wrapper release");
  FileInfo verified;
  Check(writer->Finish(&verified, &error), "finish still uses retained same handle");
  writer.reset();
  Check(RawRead(Join(path, L"unknown.bin")) == std::vector<uint8_t>({7, 8}) &&
        RawRead(Join(fixture.outside, L"sentinel")) == std::vector<uint8_t>({9}),
        "unknown and external hardlink data unchanged");
  Check(MoveFileW(owned.c_str(), Join(path, L"released.bin").c_str()) != FALSE,
        "writer destruction releases file pins");
  Check(MoveFileW(path.c_str(), Join(fixture.root, L"moved-stream").c_str()) != FALSE,
        "writer destruction releases directory pins");
}

void StreamingReentrantCalls() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "reentrant streaming directory");
  auto writer = directory->BeginWriteExclusive(L"owned.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value(), "reentrant streaming writer");
  bool entered = false, nested_append_busy = false, nested_finish_busy = false;
  auto reenter = [&] {
    if (!entered) {
      entered = true;
      Error& nested = error;
      nested_append_busy = !writer->Append(kStreamingAbc.data(), 1, &nested) &&
          nested.code == ErrorCode::kBusy;
      FileInfo ignored;
      nested_finish_busy = !writer->Finish(&ignored, &nested) &&
          nested.code == ErrorCode::kBusy;
    }
    return false;
  };
  const bool appended = writer->Append(kStreamingAbc.data(), 3, &error, reenter);
  Check(entered && nested_append_busy && nested_finish_busy && appended &&
        writer->written() == 3 && error.code == ErrorCode::kNone,
        "nested calls are Busy and outer append clears aliased diagnostic output");
  entered = nested_append_busy = nested_finish_busy = false;
  FileInfo info;
  const bool finished = writer->Finish(&info, &error, reenter);
  Check(entered && nested_append_busy && nested_finish_busy && finished &&
        info.size == 3 && error.code == ErrorCode::kNone,
        "nested calls are Busy and outer finalize clears aliased diagnostic output");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\owned.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "reentry causes no extra writes");
}

void StreamingFinishCancellation() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "finish cancellation directory");
  auto writer = directory->BeginWriteExclusive(L"finish-cancel.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 3, &error),
        "fully written cooperative finish cancellation fixture");
  FileInfo info;
  info.size = 999;
  bool called = false;
  Check(!writer->Finish(&info, &error, [&] {
          called = true;
          writer->Cancel();
          return false;
        }) && called && error.code == ErrorCode::kIo &&
        error.win32 == ERROR_OPERATION_ABORTED && info.size == 0,
        "Finish rechecks synchronous Cancel even when callback returns false");
  Check(!writer->Finish(&info, &error), "cooperatively cancelled finish stays terminal");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\finish-cancel.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "cancelled finish retains all written bytes");

  writer = directory->BeginWriteExclusive(L"finish-throws.bin", 3,
                                          kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 3, &error),
        "finish callback exception fixture");
  info.size = 999;
  Check(!writer->Finish(&info, &error, []() -> bool {
          throw std::runtime_error("synthetic cancellation callback exception");
        }) && error.code == ErrorCode::kIo &&
        error.win32 == ERROR_OPERATION_ABORTED && info.size == 0,
        "Finish callback exception cancels without a verified observation");
  Check(!writer->Append(nullptr, 0, &error), "checked no-op cannot revive failed writer");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\finish-throws.bin")) ==
        std::vector<uint8_t>({'a', 'b', 'c'}), "callback exception preserves full partial");

  writer = directory->BeginWriteExclusive(L"append-cancel.bin", 3,
                                          kStreamingAbcSha256, &error);
  Check(writer.has_value(), "synchronous append cancellation fixture");
  called = false;
  Check(!writer->Append(kStreamingAbc.data(), 3, &error, [&] {
          called = true;
          writer->Cancel();
          return false;
        }) && called && error.code == ErrorCode::kIo &&
        error.win32 == ERROR_OPERATION_ABORTED && writer->written() == 0,
        "Append rechecks synchronous Cancel before writing");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\append-cancel.bin")).empty(),
        "synchronous append cancellation preserves its empty partial");
  // These callbacks cancel cooperatively during Finish/Append. They do not
  // establish cancellation of an active disk syscall or a specific readback phase.
}

// Callback gates are bounded and always released before joining, including
// assertion failure. They do not pretend to interrupt an active OS syscall.
struct StreamingGate {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false, released = false;
  bool Hold() {
    std::unique_lock<std::mutex> lock(mutex);
    entered = true;
    condition.notify_all();
    return condition.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
  }
  bool WaitEntered() {
    std::unique_lock<std::mutex> lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
  }
  void Release() {
    std::lock_guard<std::mutex> lock(mutex);
    released = true;
    condition.notify_all();
  }
};
struct StreamingJoin {
  std::thread& worker;
  StreamingGate& gate;
  ~StreamingJoin() { gate.Release(); if (worker.joinable()) worker.join(); }
  void Join() { gate.Release(); if (worker.joinable()) worker.join(); }
};

void StreamingConcurrentCancellation() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "concurrent streaming directory");
  auto writer = directory->BeginWriteExclusive(L"partial.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value(), "concurrent streaming writer");
  StreamingGate gate;
  std::atomic<bool> gated{false}, timed_out{false}, worker_returned{false};
  bool appended = true;
  Error worker_error;
  std::thread worker([&] {
    appended = writer->Append(kStreamingAbc.data(), 3, &worker_error, [&] {
      if (!gated.exchange(true) && !gate.Hold()) timed_out = true;
      return false;  // Only cross-thread Cancel may cancel this operation.
    });
    worker_returned.store(true, std::memory_order_release);
  });
  StreamingJoin join{worker, gate};
  Check(gate.WaitEntered(), "worker reached bounded cancellation gate");
  writer->Cancel();
  Check(!worker_returned.load(std::memory_order_acquire),
        "Cancel signals intent without reporting held worker settlement");
  join.Join();
  Check(worker_returned.load(std::memory_order_acquire) && !timed_out &&
        !appended && worker_error.win32 == ERROR_OPERATION_ABORTED &&
        writer->written() == 0, "Cancel observed after callback before first native write");
  FileInfo info;
  Check(!writer->Finish(&info, &error), "cancelled worker cannot publish verified file");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\partial.bin")).empty(),
        "cross-thread cancellation retains exact empty partial");
}

void StreamingAllocationFailures() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "allocation failure streaming directory");
  auto writer = directory->BeginWriteExclusive(L"append-oom.bin", 3,
                                               kStreamingAbcSha256, &error);
  Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 1, &error),
        "allocation failure append prefix");
  testing::FailNextCanonicalPathAllocation();
  Check(!writer->Append(kStreamingAbc.data() + 1, 2, &error) &&
        error.code == ErrorCode::kIo && error.win32 == ERROR_NOT_ENOUGH_MEMORY &&
        writer->written() == 1, "Append maps canonical-path allocation failure");
  Check(!writer->Append(nullptr, 0, &error) && error.code == ErrorCode::kInvalidPath,
        "allocation failure poisons Append even for a checked no-op");
  FileInfo info;
  info.size = 999;
  Check(!writer->Finish(&info, &error) && info.size == 0,
        "allocation-failed append cannot finalize");
  writer.reset();
  Check(RawRead(Join(fixture.root, L"stream\\append-oom.bin")) ==
        std::vector<uint8_t>({'a'}), "allocation-failed append retains exact prefix");

  for (const bool after_readback : {false, true}) {
    const auto name = after_readback ? L"finish-final-oom.bin" : L"finish-initial-oom.bin";
    writer = directory->BeginWriteExclusive(name, 3, kStreamingAbcSha256, &error);
    Check(writer.has_value() && writer->Append(kStreamingAbc.data(), 3, &error),
          "allocation failure finish fixture");
    bool injected = !after_readback;
    if (!after_readback) testing::FailNextCanonicalPathAllocation();
    info.size = 999;
    Check(!writer->Finish(&info, &error, [&] {
            if (after_readback && !injected && testing::CompletedReadbackBytes() == 3) {
              injected = true;
              testing::FailNextCanonicalPathAllocation();
            }
            return false;
          }) && injected && error.code == ErrorCode::kIo &&
          error.win32 == ERROR_NOT_ENOUGH_MEMORY && info.size == 0,
          "Finish maps initial and final canonical-path allocation failures");
    Check(testing::CompletedReadbackBytes() == (after_readback ? 3u : 0u),
          "allocation failure occurs at the selected native validation phase");
    Check(!writer->Append(nullptr, 0, &error) && error.code == ErrorCode::kInvalidPath,
          "failed finalization cannot reopen writing after seek or hash changes");
    Check(!writer->Finish(&info, &error) && info.size == 0,
          "allocation-failed finalization cannot be retried as success");
    writer.reset();
    Check(RawRead(Join(fixture.root, Join(L"stream", name))) ==
          std::vector<uint8_t>({'a', 'b', 'c'}), "allocation-failed finish retains full partial");
  }
}

void StreamingConcurrentBusy() {
  for (const bool finishing : {false, true}) {
    Fixture fixture;
    auto root = Open(fixture.root);
    Error error;
    auto directory = root.CreateDirectoryExclusive(L"stream", &error);
    Check(directory.has_value(), "simultaneous Busy directory");
    auto writer = directory->BeginWriteExclusive(L"owned.bin", 3,
                                                 kStreamingAbcSha256, &error);
    Check(writer.has_value(), "simultaneous Busy writer");
    if (finishing) Check(writer->Append(kStreamingAbc.data(), 3, &error),
                         "simultaneous Finish prefix");
    StreamingGate gate;
    std::atomic<bool> gated{false}, timed_out{false}, worker_returned{false};
    bool succeeded = false;
    Error worker_error;
    FileInfo worker_info;
    std::thread worker([&] {
      auto hold = [&] {
        if (!gated.exchange(true) && !gate.Hold()) timed_out = true;
        return false;
      };
      succeeded = finishing ? writer->Finish(&worker_info, &worker_error, hold)
          : writer->Append(kStreamingAbc.data(), 3, &worker_error, hold);
      worker_returned.store(true, std::memory_order_release);
    });
    StreamingJoin join{worker, gate};
    Check(gate.WaitEntered(), "worker holds exclusive operation before callbacks return");
    // Each concurrent invocation owns distinct diagnostics; the worker's
    // non-atomic outputs are inspected only after actual thread settlement.
    Error append_error, finish_error;
    FileInfo rejected_info;
    rejected_info.size = 999;
    Check(!writer->Append(nullptr, 0, &append_error) &&
          append_error.code == ErrorCode::kBusy,
          "simultaneous Append refuses without poisoning held operation");
    Check(!writer->Finish(&rejected_info, &finish_error) &&
          finish_error.code == ErrorCode::kBusy && rejected_info.size == 0,
          "simultaneous Finish refuses with separate diagnostics and cleared output");
    Check(!worker_returned.load(std::memory_order_acquire),
          "Busy callers do not settle or release the held operation");
    join.Join();
    Check(!timed_out && succeeded && worker_error.code == ErrorCode::kNone &&
          writer->written() == 3, "Busy refusals leave the active operation successful");
    if (!finishing) Check(writer->Finish(&worker_info, &error),
                          "writer still finalizes after simultaneous Busy refusals");
    Check(worker_info.size == 3, "only the actual owner publishes verified length");
    writer.reset();
    Check(RawRead(Join(fixture.root, L"stream\\owned.bin")) ==
          std::vector<uint8_t>({'a', 'b', 'c'}), "simultaneous calls preserve exact bytes");
  }
}

void StreamingReadbackCancellation() {
  Fixture fixture;
  auto root = Open(fixture.root);
  Error error;
  auto directory = root.CreateDirectoryExclusive(L"stream", &error);
  Check(directory.has_value(), "readback cancellation directory");
  constexpr uint64_t size = 1000000;
  auto writer = directory->BeginWriteExclusive(L"readback-cancel.bin", size,
                                               kStreamingMillionASha256, &error);
  Check(writer.has_value(), "readback cancellation writer");
  const std::vector<uint8_t> chunk(65536, 'a');
  for (uint64_t offset = 0; offset < size;) {
    const auto count = static_cast<size_t>((std::min)(uint64_t{chunk.size()}, size - offset));
    Check(writer->Append(chunk.data(), count, &error), "readback cancellation fixture append");
    offset += count;
  }
  StreamingGate gate;
  std::atomic<bool> gated{false}, timed_out{false}, worker_returned{false};
  std::atomic<uint64_t> readback_at_gate{0};
  bool finished = true;
  Error worker_error;
  FileInfo worker_info;
  worker_info.size = 999;
  std::thread worker([&] {
    finished = writer->Finish(&worker_info, &worker_error, [&] {
      const auto readback = testing::CompletedReadbackBytes();
      if (readback >= FileWriteLease::kMaximumChunkBytes && !gated.exchange(true)) {
        readback_at_gate.store(readback, std::memory_order_release);
        if (!gate.Hold()) timed_out = true;
      }
      return false;
    });
    worker_returned.store(true, std::memory_order_release);
  });
  StreamingJoin join{worker, gate};
  Check(gate.WaitEntered(), "cancellation gate reached after successful native readback");
  const auto readback = readback_at_gate.load(std::memory_order_acquire);
  Check(readback >= FileWriteLease::kMaximumChunkBytes && readback < size,
        "at least one bounded chunk was read and hashed before cancellation");
  writer->Cancel();
  Check(!worker_returned.load(std::memory_order_acquire),
        "post-read cancellation signal does not claim held worker settlement");
  join.Join();
  Check(!timed_out && !finished && worker_error.code == ErrorCode::kIo &&
        worker_error.win32 == ERROR_OPERATION_ABORTED && worker_info.size == 0 &&
        writer->written() == size, "post-read cancellation refuses verified output");
  Check(!writer->Append(nullptr, 0, &error) && error.code == ErrorCode::kInvalidPath,
        "cancelled readback permanently closes writing");
  Check(!writer->Finish(&worker_info, &error), "cancelled readback cannot replay finalization");
  writer.reset();
  const auto retained = RawRead(Join(fixture.root, L"stream\\readback-cancel.bin"));
  Check(retained.size() == size && std::all_of(retained.begin(), retained.end(),
        [](uint8_t value) { return value == 'a'; }),
        "post-read cancellation preserves all bytes for separate exact recovery");
}

}  // namespace

int main() {
  try {
    GrammarAndBounds();
    IdentityAndHardlinks();
    ReparseBoundaries();
    PinningAndLeases();
    PinnedReadHandle();
    ExactRemovalLeases();
    StreamingKnownDigests();
    StreamingBoundsAndPoison();
    StreamingFailuresPreserveBytes();
    StreamingOwnerPinsAndCollisions();
    StreamingReentrantCalls();
    StreamingFinishCancellation();
    StreamingConcurrentCancellation();
    StreamingAllocationFailures();
    StreamingConcurrentBusy();
    StreamingReadbackCancellation();
    std::cout << "PASS: 16 secure-files suites; skipped native fixtures=" << skipped << "\n";
    return skipped == 0 ? 0 : 2;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\nFixtures retained; no cleanup attempted.\n";
    return 1;
  }
}
