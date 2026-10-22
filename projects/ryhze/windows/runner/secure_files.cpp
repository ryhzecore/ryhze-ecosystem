#include "secure_files.h"

#include <winternl.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iterator>
#include <new>
#include <set>
#include <utility>

namespace ryhze::secure_files {
namespace {
constexpr size_t kMaximumPath = 32700;
constexpr size_t kMaximumComponent = 255;
#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
thread_local bool fail_next_canonical_allocation = false;
thread_local uint64_t completed_readback_bytes = 0;
#endif

class Handle {
 public:
  Handle() = default;
  explicit Handle(HANDLE value) : value_(value) {}
  ~Handle() { Reset(); }
  Handle(Handle&& other) noexcept : value_(other.Release()) {}
  Handle& operator=(Handle&& other) noexcept {
    if (this != &other) { Reset(); value_ = other.Release(); }
    return *this;
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }
  explicit operator bool() const {
    return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
  }
 private:
  HANDLE Release() { return std::exchange(value_, INVALID_HANDLE_VALUE); }
  void Reset() { if (*this) CloseHandle(value_); value_ = INVALID_HANDLE_VALUE; }
  HANDLE value_ = INVALID_HANDLE_VALUE;
};

void Clear(Error* error) { if (error) *error = {}; }
bool Fail(Error* error, ErrorCode code, DWORD native = ERROR_SUCCESS) {
  if (error) *error = {code, native};
  return false;
}
bool NativeFailure(Error* error, DWORD native) {
  auto code = ErrorCode::kIo;
  if (native == ERROR_FILE_NOT_FOUND || native == ERROR_PATH_NOT_FOUND)
    code = ErrorCode::kNotFound;
  else if (native == ERROR_FILE_EXISTS || native == ERROR_ALREADY_EXISTS)
    code = ErrorCode::kAlreadyExists;
  else if (native == ERROR_SHARING_VIOLATION || native == ERROR_LOCK_VIOLATION)
    code = ErrorCode::kBusy;
  else if (native == ERROR_NOT_SUPPORTED || native == ERROR_INVALID_FUNCTION)
    code = ErrorCode::kUnsupported;
  return Fail(error, code, native);
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
  return a.size() == b.size() &&
      CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
                           static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}
std::wstring Append(const std::wstring& parent, const std::wstring& child) {
  return parent + (parent.back() == L'\\' ? L"" : L"\\") + child;
}

bool Component(const std::wstring& name) {
  if (name.empty() || name.size() > kMaximumComponent || name == L"." ||
      name == L".." || name.back() == L'.' || name.back() == L' ') return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const auto ch = static_cast<uint16_t>(name[i]);
    if (ch < 32 || (ch >= 127 && ch <= 159) ||
        std::wstring(L"<>:\"/\\|?*").find(name[i]) != std::wstring::npos) return false;
    if (ch >= 0xd800 && ch <= 0xdbff) {
      if (++i == name.size() || name[i] < 0xdc00 || name[i] > 0xdfff) return false;
    } else if (ch >= 0xdc00 && ch <= 0xdfff) {
      return false;
    }
  }
  auto stem = name.substr(0, name.find(L'.'));
  for (auto& ch : stem)
    if (ch >= L'a' && ch <= L'z') ch = static_cast<wchar_t>(ch - (L'a' - L'A'));
  if (stem == L"CON" || stem == L"PRN" || stem == L"AUX" || stem == L"NUL" ||
      stem == L"CLOCK$" || stem == L"CONIN$" || stem == L"CONOUT$") return false;
  if (stem.size() == 4 && (stem.substr(0, 3) == L"COM" || stem.substr(0, 3) == L"LPT")) {
    const auto digit = stem[3];
    if ((digit >= L'0' && digit <= L'9') || digit == 0xb9 || digit == 0xb2 || digit == 0xb3)
      return false;
  }
  return true;
}
bool Split(const std::wstring& path, std::vector<std::wstring>* parts,
           Error* error) {
  if (path.empty() || path.size() > kMaximumPath) return Fail(error, ErrorCode::kInvalidPath);
  size_t begin = 0;
  while (begin < path.size()) {
    const size_t end = path.find(L'\\', begin);
    auto part = path.substr(begin, end == std::wstring::npos ? end : end - begin);
    if (!Component(part)) return Fail(error, ErrorCode::kInvalidPath);
    parts->push_back(std::move(part));
    if (end == std::wstring::npos) return true;
    begin = end + 1;
  }
  return Fail(error, ErrorCode::kInvalidPath);
}

bool Query(HANDLE handle, FileInfo* info, std::wstring* canonical, Error* error) {
  if (GetFileType(handle) != FILE_TYPE_DISK) return Fail(error, ErrorCode::kUnsupported);
  FILE_ATTRIBUTE_TAG_INFO tag{};
  FILE_STANDARD_INFO standard{};
  FILE_ID_INFO identity{};
  if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
      !GetFileInformationByHandleEx(handle, FileStandardInfo, &standard, sizeof(standard)) ||
      !GetFileInformationByHandleEx(handle, FileIdInfo, &identity, sizeof(identity)))
    return NativeFailure(error, GetLastError());
  if ((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    return Fail(error, ErrorCode::kReparsePoint);
  if (standard.DeletePending || standard.EndOfFile.QuadPart < 0)
    return Fail(error, ErrorCode::kIdentityMismatch);
  const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_GUID;
  const DWORD required = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
  if (required == 0) return NativeFailure(error, GetLastError());
  if (required > kMaximumPath) return Fail(error, ErrorCode::kInvalidPath);
#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
  if (std::exchange(fail_next_canonical_allocation, false)) throw std::bad_alloc();
#endif
  std::wstring name(required, L'\0');
  const DWORD written = GetFinalPathNameByHandleW(handle, name.data(), required, flags);
  if (written == 0) return NativeFailure(error, GetLastError());
  if (written >= required) return Fail(error, ErrorCode::kIdentityMismatch);
  name.resize(written);
  info->identity.volume = identity.VolumeSerialNumber;
  std::copy(std::begin(identity.FileId.Identifier), std::end(identity.FileId.Identifier),
            info->identity.id.begin());
  info->size = static_cast<uint64_t>(standard.EndOfFile.QuadPart);
  info->is_directory = standard.Directory != FALSE;
  info->link_count = standard.NumberOfLinks;
  *canonical = std::move(name);
  return true;
}

// Discard must not remove unrecorded alternate data streams with a known file.
// Refuse unsupported/oversized enumeration rather than treating it as empty.
bool NoNamedStreams(HANDLE handle, Error* error) {
  alignas(FILE_STREAM_INFO) std::array<uint8_t, 65536> buffer{};
  if (!GetFileInformationByHandleEx(handle, FileStreamInfo, buffer.data(),
                                   static_cast<DWORD>(buffer.size()))) {
    const DWORD native = GetLastError();
    return native == ERROR_HANDLE_EOF || NativeFailure(error, native);
  }
  size_t offset = 0;
  bool default_seen = false;
  while (true) {
    constexpr size_t header = offsetof(FILE_STREAM_INFO, StreamName);
    if (offset > buffer.size() - header) return Fail(error, ErrorCode::kIdentityMismatch);
    const auto* entry = reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data() + offset);
    if (entry->StreamNameLength % sizeof(wchar_t) ||
        entry->StreamNameLength > buffer.size() - offset - header)
      return Fail(error, ErrorCode::kIdentityMismatch);
    const std::wstring name(entry->StreamName, entry->StreamNameLength / sizeof(wchar_t));
    if (name != L"::$DATA" || default_seen) return Fail(error, ErrorCode::kIdentityMismatch);
    default_seen = true;
    if (!entry->NextEntryOffset) return true;
    if (entry->NextEntryOffset < header || entry->NextEntryOffset % alignof(FILE_STREAM_INFO) ||
        entry->NextEntryOffset > buffer.size() - offset)
      return Fail(error, ErrorCode::kIdentityMismatch);
    offset += entry->NextEntryOffset;
  }
}

template <typename T> T Resolve(const char* name) {
  T result = nullptr;
  const auto address = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), name);
  static_assert(sizeof(result) == sizeof(address));
  std::memcpy(&result, &address, sizeof(result));
  return result;
}

// One component relative to a held directory, never a re-opened full path.
// NtCreateFile's documented RootDirectory and OPEN_REPARSE_POINT semantics
// provide the acquisition boundary; Win32 APIs operate on the returned handle.
Handle OpenChild(HANDLE parent, const std::wstring& name, ACCESS_MASK access,
                 ULONG share, ULONG disposition, ULONG type, Error* error) {
  using Create = decltype(&NtCreateFile);
  using ToError = ULONG(NTAPI*)(NTSTATUS);
  static const auto create = Resolve<Create>("NtCreateFile");
  static const auto to_error = Resolve<ToError>("RtlNtStatusToDosError");
  if (!create || !to_error) { Fail(error, ErrorCode::kUnsupported); return {}; }
  UNICODE_STRING object_name{};
  object_name.Buffer = const_cast<PWSTR>(name.data());
  object_name.Length = static_cast<USHORT>(name.size() * sizeof(wchar_t));
  object_name.MaximumLength = object_name.Length;
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  attributes.RootDirectory = parent;
  attributes.ObjectName = &object_name;
  attributes.Attributes = OBJ_CASE_INSENSITIVE;
  IO_STATUS_BLOCK status_block{};
  HANDLE handle = INVALID_HANDLE_VALUE;
  const NTSTATUS status = create(&handle, access | SYNCHRONIZE, &attributes,
      &status_block, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
      type | FILE_OPEN_REPARSE_POINT | FILE_SYNCHRONOUS_IO_NONALERT, nullptr, 0);
  if (status < 0) { NativeFailure(error, to_error(status)); return {}; }
  return Handle(handle);
}

struct PinnedDirectory {
  Handle handle;
  FileInfo info;
  std::wstring canonical;
};
bool CheckChild(HANDLE handle, const PinnedDirectory& parent,
                const std::wstring& name, FileInfo* info,
                std::wstring* canonical, Error* error) {
  if (!Query(handle, info, canonical, error)) return false;
  if (info->identity.volume != parent.info.identity.volume ||
      !SamePath(*canonical, Append(parent.canonical, name)))
    return Fail(error, ErrorCode::kOutsideRoot);
  return true;
}
bool PinChild(const PinnedDirectory& parent, const std::wstring& name,
              bool create, PinnedDirectory* child, Error* error) {
  child->handle = OpenChild(parent.handle.get(), name, FILE_READ_ATTRIBUTES | FILE_TRAVERSE,
      FILE_SHARE_READ, create ? FILE_OPEN_IF : FILE_OPEN, FILE_DIRECTORY_FILE, error);
  if (!child->handle || !CheckChild(child->handle.get(), parent, name,
                                    &child->info, &child->canonical, error)) return false;
  return child->info.is_directory || Fail(error, ErrorCode::kInvalidPath);
}
// CNG is already an application/native-stager dependency. The hash is allocated
// before exclusive creation, avoiding a new partial file if SHA-256 is absent.
class WriteHash {
 public:
  WriteHash() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM,
                                    nullptr, 0) >= 0)
      BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~WriteHash() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  WriteHash(const WriteHash&) = delete;
  WriteHash& operator=(const WriteHash&) = delete;
  bool ready() const { return hash_ != nullptr; }
  bool Add(const uint8_t* bytes, size_t size) {
    return ready() && size <= FileWriteLease::kMaximumChunkBytes &&
        BCryptHashData(hash_, const_cast<PUCHAR>(bytes),
                       static_cast<ULONG>(size), 0) >= 0;
  }
  bool Finish(FileDigest* digest) {
    return ready() && BCryptFinishHash(hash_, digest->data(),
                                       static_cast<ULONG>(digest->size()), 0) >= 0;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};

// Acquire before any callback. Failed acquisition must not alter the active
// operation's state, position, hash, byte count or cancellation observation.
class WriteOperation {
 public:
  explicit WriteOperation(std::atomic<bool>& busy) : busy_(busy),
      acquired_(!busy_.exchange(true, std::memory_order_acq_rel)) {}
  ~WriteOperation() {
    if (acquired_) busy_.store(false, std::memory_order_release);
  }
  WriteOperation(const WriteOperation&) = delete;
  WriteOperation& operator=(const WriteOperation&) = delete;
  explicit operator bool() const { return acquired_; }
 private:
  std::atomic<bool>& busy_;
  bool acquired_;
};

bool WriteCancelled(const std::function<bool()>& callback,
                    const std::atomic<bool>* cancelled = nullptr) {
  if (cancelled && cancelled->load(std::memory_order_acquire)) return true;
  try { if (callback && callback()) return true; }
  catch (...) { return true; }
  // The callback can synchronously cancel the lease and return false.
  return cancelled && cancelled->load(std::memory_order_acquire);
}

}  // namespace

struct Root::Impl {
  std::vector<PinnedDirectory> ancestors;
  bool Validate(Error* error) const {
    for (const auto& pinned : ancestors) {
      FileInfo current;
      std::wstring canonical;
      if (!Query(pinned.handle.get(), &current, &canonical, error)) return false;
      if (!current.is_directory || current.identity != pinned.info.identity ||
          !SamePath(canonical, pinned.canonical)) return Fail(error, ErrorCode::kIdentityMismatch);
    }
    return true;
  }
  const PinnedDirectory* Parents(const std::vector<std::wstring>& parts,
                                 std::vector<PinnedDirectory>* pins, Error* error) const {
    if (!Validate(error)) return nullptr;
    const auto* parent = &ancestors.back();
    for (size_t i = 0; i + 1 < parts.size(); ++i) {
      PinnedDirectory next;
      if (!PinChild(*parent, parts[i], false, &next, error)) return nullptr;
      pins->push_back(std::move(next));
      parent = &pins->back();
    }
    return parent;
  }
  Handle Target(const std::wstring& relative, ACCESS_MASK access, ULONG share,
                ULONG disposition, bool file_only, std::vector<PinnedDirectory>* pins,
                FileInfo* info, Error* error) const {
    std::vector<std::wstring> parts;
    if (!Split(relative, &parts, error)) return {};
    const auto* parent = Parents(parts, pins, error);
    if (!parent) return {};
    auto handle = OpenChild(parent->handle.get(), parts.back(), access, share,
                            disposition, file_only ? FILE_NON_DIRECTORY_FILE : 0, error);
    std::wstring canonical;
    if (!handle || !CheckChild(handle.get(), *parent, parts.back(), info, &canonical, error))
      return {};
    return handle;
  }
};
struct Lease::Impl {
  std::shared_ptr<void> owner;
  std::vector<PinnedDirectory> ancestors;
  Handle file;
};
struct FileLease::Impl {
  std::shared_ptr<void> owner;
  std::vector<PinnedDirectory> ancestors;
  Handle file;
  FileInfo info;
};
struct DirectoryLease::Impl {
  std::shared_ptr<void> owner;
  std::vector<PinnedDirectory> ancestors;
  PinnedDirectory directory;
  bool removable = false;
};
struct FileRemovalLease::Impl {
  std::shared_ptr<void> owner;
  Handle file;
  FileInfo info;
  std::wstring canonical;
};

struct FileWriteLease::Impl {
  enum class State { kOpen, kFailed, kFinished };
  std::shared_ptr<void> owner;
  Handle file;
  FileInfo created;
  std::wstring canonical;
  uint64_t expected_size = 0;
  FileDigest expected_digest{};
  std::atomic<uint64_t> written{0};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> busy{false};
  State state = State::kOpen;
  WriteHash hash;

  bool FailWrite(Error* error, ErrorCode code, DWORD native = ERROR_SUCCESS) {
    state = State::kFailed;
    return Fail(error, code, native);
  }
  bool FailNative(Error* error, DWORD native) {
    state = State::kFailed;
    return NativeFailure(error, native);
  }
  bool CheckCancelled(const std::function<bool()>& callback, Error* error) {
    if (WriteCancelled(callback, &cancelled)) {
      FailNative(error, ERROR_OPERATION_ABORTED);
      return true;
    }
    // A rejected nested call may have written through the same Error pointer.
    Clear(error);
    return false;
  }
  bool Validate(uint64_t expected_length, FileInfo* info, Error* error) {
    std::wstring current_name;
    if (!Query(file.get(), info, &current_name, error) ||
        !NoNamedStreams(file.get(), error)) {
      state = State::kFailed;
      return false;
    }
    if (info->is_directory || info->link_count != 1 ||
        info->identity != created.identity || info->size != expected_length ||
        !SamePath(current_name, canonical))
      return FailWrite(error, ErrorCode::kIdentityMismatch);
    return true;
  }
};

#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
void testing::FailNextCanonicalPathAllocation() noexcept {
  fail_next_canonical_allocation = true;
}
uint64_t testing::CompletedReadbackBytes() noexcept {
  return completed_readback_bytes;
}
#endif

bool FileIdentity::operator==(const FileIdentity& other) const {
  return volume == other.volume && id == other.id;
}
Root::Root(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
Root::~Root() = default;
Root::Root(Root&&) noexcept = default;
Root& Root::operator=(Root&&) noexcept = default;
Lease::Lease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Lease::~Lease() = default;
Lease::Lease(Lease&&) noexcept = default;
Lease& Lease::operator=(Lease&&) noexcept = default;
FileLease::FileLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileLease::~FileLease() = default;
FileLease::FileLease(FileLease&&) noexcept = default;
FileLease& FileLease::operator=(FileLease&&) noexcept = default;
DirectoryLease::DirectoryLease(std::shared_ptr<Impl> impl) : impl_(std::move(impl)) {}
DirectoryLease::~DirectoryLease() = default;
DirectoryLease::DirectoryLease(DirectoryLease&&) noexcept = default;
DirectoryLease& DirectoryLease::operator=(DirectoryLease&&) noexcept = default;
FileRemovalLease::FileRemovalLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileRemovalLease::~FileRemovalLease() = default;
FileRemovalLease::FileRemovalLease(FileRemovalLease&&) noexcept = default;
FileRemovalLease& FileRemovalLease::operator=(FileRemovalLease&&) noexcept = default;
HANDLE FileRemovalLease::handle() const { return impl_ ? impl_->file.get() : INVALID_HANDLE_VALUE; }
const FileInfo& FileRemovalLease::info() const {
  static const FileInfo empty;
  return impl_ ? impl_->info : empty;
}
bool FileRemovalLease::Remove(Error* error) {
  Clear(error);
  if (!impl_ || !impl_->file) return Fail(error, ErrorCode::kInvalidPath);
  FileInfo current;
  std::wstring canonical;
  if (!Query(impl_->file.get(), &current, &canonical, error)) return false;
  if (current.is_directory || current.link_count != 1 ||
      current.identity != impl_->info.identity || current.size != impl_->info.size ||
      !SamePath(canonical, impl_->canonical)) return Fail(error, ErrorCode::kIdentityMismatch);
  if (!NoNamedStreams(impl_->file.get(), error)) return false;
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(impl_->file.get(), FileDispositionInfo,
                                 &disposition, sizeof(disposition)))
    return NativeFailure(error, GetLastError());
  impl_->file = Handle();
  return true;
}
const FileInfo& DirectoryLease::info() const {
  static const FileInfo empty;
  return impl_ ? impl_->directory.info : empty;
}
bool DirectoryLease::ListNames(size_t maximum_entries,
                                std::vector<std::wstring>* names, Error* error) const {
  Clear(error);
  if (names) names->clear();
  if (!impl_ || !impl_->directory.handle || !names || maximum_entries > 8192)
    return Fail(error, ErrorCode::kInvalidPath);
  FileInfo current;
  std::wstring canonical;
  if (!Query(impl_->directory.handle.get(), &current, &canonical, error)) return false;
  if (current.identity != info().identity || !SamePath(canonical, impl_->directory.canonical))
    return Fail(error, ErrorCode::kIdentityMismatch);
  if (!NoNamedStreams(impl_->directory.handle.get(), error)) return false;
  alignas(FILE_ID_BOTH_DIR_INFO) std::array<uint8_t, 65536> buffer{};
  auto kind = FileIdBothDirectoryRestartInfo;
  std::vector<std::wstring> result;
  std::set<std::wstring> seen;
  while (true) {
    if (!GetFileInformationByHandleEx(impl_->directory.handle.get(), kind,
                                     buffer.data(), static_cast<DWORD>(buffer.size()))) {
      const DWORD native = GetLastError();
      if (native == ERROR_NO_MORE_FILES) break;
      return NativeFailure(error, native);
    }
    kind = FileIdBothDirectoryInfo;
    size_t offset = 0;
    while (true) {
      constexpr size_t header = offsetof(FILE_ID_BOTH_DIR_INFO, FileName);
      if (offset > buffer.size() - header) return Fail(error, ErrorCode::kIdentityMismatch);
      const auto* entry = reinterpret_cast<const FILE_ID_BOTH_DIR_INFO*>(buffer.data() + offset);
      if (entry->FileNameLength % sizeof(wchar_t) ||
          entry->FileNameLength > buffer.size() - offset - header)
        return Fail(error, ErrorCode::kIdentityMismatch);
      const std::wstring name(entry->FileName, entry->FileNameLength / sizeof(wchar_t));
      if (name != L"." && name != L"..") {
        if (!Component(name) || !seen.insert(name).second)
          return Fail(error, ErrorCode::kInvalidPath);
        if (result.size() >= maximum_entries) return Fail(error, ErrorCode::kTooLarge);
        result.push_back(name);
      }
      if (!entry->NextEntryOffset) break;
      if (entry->NextEntryOffset < header || entry->NextEntryOffset % alignof(FILE_ID_BOTH_DIR_INFO) ||
          entry->NextEntryOffset > buffer.size() - offset)
        return Fail(error, ErrorCode::kIdentityMismatch);
      offset += entry->NextEntryOffset;
    }
  }
  std::sort(result.begin(), result.end());
  *names = std::move(result);
  return true;
}
std::optional<FileLease> DirectoryLease::OpenRead(const std::wstring& child,
                                                 uint64_t limit, Error* error) const {
  Clear(error);
  if (!impl_ || !impl_->directory.handle || !Component(child)) {
    Fail(error, ErrorCode::kInvalidPath); return std::nullopt;
  }
  if (limit > Root::kMaximumInspectionBytes) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  auto lease = std::make_unique<FileLease::Impl>();
  lease->owner = impl_;
  lease->file = OpenChild(impl_->directory.handle.get(), child, GENERIC_READ,
                          FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, error);
  std::wstring canonical;
  if (!lease->file || !CheckChild(lease->file.get(), impl_->directory, child,
                                  &lease->info, &canonical, error)) return std::nullopt;
  if (lease->info.link_count != 1) { Fail(error, ErrorCode::kHardLink); return std::nullopt; }
  if (lease->info.size > limit) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  if (!NoNamedStreams(lease->file.get(), error)) return std::nullopt;
  return FileLease(std::move(lease));
}
std::optional<FileRemovalLease> DirectoryLease::OpenForRemoval(
    const std::wstring& child, uint64_t limit, Error* error) const {
  Clear(error);
  if (!impl_ || !impl_->directory.handle || !impl_->removable || !Component(child)) {
    Fail(error, ErrorCode::kInvalidPath); return std::nullopt;
  }
  if (limit > Root::kMaximumInspectionBytes) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  auto lease = std::make_unique<FileRemovalLease::Impl>();
  lease->owner = impl_;
  lease->file = OpenChild(impl_->directory.handle.get(), child, GENERIC_READ | DELETE,
                          FILE_SHARE_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE, error);
  if (!lease->file || !CheckChild(lease->file.get(), impl_->directory, child,
                                  &lease->info, &lease->canonical, error)) return std::nullopt;
  if (lease->info.link_count != 1) { Fail(error, ErrorCode::kHardLink); return std::nullopt; }
  if (lease->info.size > limit) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  if (!NoNamedStreams(lease->file.get(), error)) return std::nullopt;
  return FileRemovalLease(std::move(lease));
}
bool DirectoryLease::RemoveEmpty(Error* error) {
  Clear(error);
  if (!impl_ || !impl_->removable || impl_.use_count() != 1)
    return Fail(error, ErrorCode::kBusy);
  std::vector<std::wstring> names;
  if (!ListNames(0, &names, error)) return false;
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(impl_->directory.handle.get(), FileDispositionInfo,
                                 &disposition, sizeof(disposition)))
    return NativeFailure(error, GetLastError());
  impl_->directory.handle = Handle();
  return true;
}
bool DirectoryLease::CreateExclusive(const std::wstring& child,
    const std::vector<uint8_t>& bytes, FileInfo* info, Error* error) const {
  Clear(error);
  if (info) *info = {};
  if (!impl_ || !impl_->directory.handle || !info || !Component(child))
    return Fail(error, ErrorCode::kInvalidPath);
  if (bytes.size() > Root::kMaximumBytes) return Fail(error, ErrorCode::kTooLarge);
  FileInfo current;
  std::wstring canonical;
  if (!Query(impl_->directory.handle.get(), &current, &canonical, error) ||
      !NoNamedStreams(impl_->directory.handle.get(), error)) return false;
  if (current.identity != impl_->directory.info.identity ||
      !SamePath(canonical, impl_->directory.canonical)) return Fail(error, ErrorCode::kIdentityMismatch);
  auto handle = OpenChild(impl_->directory.handle.get(), child,
      GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, error);
  FileInfo created;
  if (!handle || !CheckChild(handle.get(), impl_->directory, child, &created, &canonical, error) ||
      !NoNamedStreams(handle.get(), error)) return false;
  if (created.link_count != 1) return Fail(error, ErrorCode::kHardLink);
  size_t offset = 0;
  while (offset < bytes.size()) {
    DWORD written = 0;
    const auto count = static_cast<DWORD>((std::min)(size_t{65536}, bytes.size() - offset));
    if (!WriteFile(handle.get(), bytes.data() + offset, count, &written, nullptr))
      return NativeFailure(error, GetLastError());
    if (!written) return Fail(error, ErrorCode::kIo, ERROR_WRITE_FAULT);
    offset += written;
  }
  if (!FlushFileBuffers(handle.get())) return NativeFailure(error, GetLastError());
  if (!Query(handle.get(), &current, &canonical, error) || !NoNamedStreams(handle.get(), error)) return false;
  if (current.identity != created.identity || current.size != bytes.size() || current.link_count != 1)
    return Fail(error, ErrorCode::kIdentityMismatch);
  *info = current;
  return true;
}
FileWriteLease::FileWriteLease(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
FileWriteLease::~FileWriteLease() = default;
FileWriteLease::FileWriteLease(FileWriteLease&&) noexcept = default;
FileWriteLease& FileWriteLease::operator=(FileWriteLease&&) noexcept = default;
uint64_t FileWriteLease::written() const {
  return impl_ ? impl_->written.load(std::memory_order_acquire) : 0;
}
const FileIdentity& FileWriteLease::created_identity() const {
  static const FileIdentity empty;
  return impl_ ? impl_->created.identity : empty;
}
void FileWriteLease::Cancel() noexcept {
  if (impl_) impl_->cancelled.store(true, std::memory_order_release);
}
bool FileWriteLease::Append(const uint8_t* bytes, size_t size, Error* error,
                           const std::function<bool()>& cancelled) {
  Clear(error);
  if (!impl_) return Fail(error, ErrorCode::kInvalidPath);
  auto& state = *impl_;
  WriteOperation operation(state.busy);
  if (!operation) return Fail(error, ErrorCode::kBusy);
  try {
    if (state.state != Impl::State::kOpen)
      return Fail(error, ErrorCode::kInvalidPath);
    if ((!bytes && size) || !state.file)
      return state.FailWrite(error, ErrorCode::kInvalidPath);
    const auto already_written = state.written.load(std::memory_order_acquire);
    if (size > kMaximumChunkBytes || already_written > state.expected_size ||
        size > state.expected_size - already_written)
      return state.FailWrite(error, ErrorCode::kTooLarge);
    if (state.CheckCancelled(cancelled, error)) return false;
    FileInfo observed;
    if (!state.Validate(already_written, &observed, error)) return false;
    size_t offset = 0;
    while (offset < size) {
      if (state.CheckCancelled(cancelled, error)) return false;
      const auto count = static_cast<DWORD>((std::min)(size_t{65536}, size - offset));
      DWORD written = 0;
      if (!WriteFile(state.file.get(), bytes + offset, count, &written, nullptr))
        return state.FailNative(error, GetLastError());
      if (!written || written > count)
        return state.FailWrite(error, ErrorCode::kIo, ERROR_WRITE_FAULT);
      offset += written;
      state.written.store(already_written + offset, std::memory_order_release);
    }
    if (state.CheckCancelled(cancelled, error)) return false;
    Clear(error);
    return true;
  } catch (const std::bad_alloc&) {
    // Keep the acquired file and prefix for exact recovery; never reopen writing.
    return state.FailWrite(error, ErrorCode::kIo, ERROR_NOT_ENOUGH_MEMORY);
  }
}
bool FileWriteLease::Finish(FileInfo* info, Error* error,
                           const std::function<bool()>& cancelled) {
  Clear(error);
  if (info) *info = {};
  if (!impl_) return Fail(error, ErrorCode::kInvalidPath);
  auto& state = *impl_;
  WriteOperation operation(state.busy);
  if (!operation) return Fail(error, ErrorCode::kBusy);
#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
  completed_readback_bytes = 0;
#endif
  try {
    if (state.state != Impl::State::kOpen)
      return Fail(error, ErrorCode::kInvalidPath);
    if (!info || !state.file)
      return state.FailWrite(error, ErrorCode::kInvalidPath);
    if (state.CheckCancelled(cancelled, error)) return false;
    if (state.written.load(std::memory_order_acquire) != state.expected_size)
      return state.FailWrite(error, ErrorCode::kIdentityMismatch);
    FileInfo observed;
    if (!state.Validate(state.expected_size, &observed, error)) return false;
    if (state.CheckCancelled(cancelled, error)) return false;
    if (!FlushFileBuffers(state.file.get())) return state.FailNative(error, GetLastError());
    if (state.CheckCancelled(cancelled, error)) return false;
    LARGE_INTEGER beginning{};
    if (!SetFilePointerEx(state.file.get(), beginning, nullptr, FILE_BEGIN))
      return state.FailNative(error, GetLastError());
    std::vector<uint8_t> buffer;
    buffer.resize(static_cast<size_t>((std::min)(
        uint64_t{kMaximumChunkBytes}, state.expected_size)));
    uint64_t verified = 0;
    while (verified < state.expected_size) {
      if (state.CheckCancelled(cancelled, error)) return false;
      const auto count = static_cast<DWORD>((std::min)(
          uint64_t{buffer.size()}, state.expected_size - verified));
      DWORD read = 0;
      if (!ReadFile(state.file.get(), buffer.data(), count, &read, nullptr))
        return state.FailNative(error, GetLastError());
      if (!read || read > count)
        return state.FailWrite(error, ErrorCode::kIdentityMismatch);
      if (!state.hash.Add(buffer.data(), read))
        return state.FailWrite(error, ErrorCode::kUnsupported);
      verified += read;
#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
      completed_readback_bytes = verified;
#endif
    }
    if (state.CheckCancelled(cancelled, error)) return false;
    FileDigest digest{};
    if (!state.hash.Finish(&digest))
      return state.FailWrite(error, ErrorCode::kUnsupported);
    if (digest != state.expected_digest)
      return state.FailWrite(error, ErrorCode::kIdentityMismatch, ERROR_CRC);
    if (!state.Validate(state.expected_size, &observed, error)) return false;
    if (state.CheckCancelled(cancelled, error)) return false;
    state.state = Impl::State::kFinished;
    *info = observed;
    Clear(error);
    return true;
  } catch (const std::bad_alloc&) {
    // Keep the acquired file and prefix for exact recovery; never reopen writing.
    return state.FailWrite(error, ErrorCode::kIo, ERROR_NOT_ENOUGH_MEMORY);
  }
}

std::optional<FileWriteLease> DirectoryLease::BeginWriteExclusive(
    const std::wstring& child, uint64_t expected_size,
    const FileDigest& expected_digest, Error* error,
    const std::function<bool()>& cancelled) const {
  Clear(error);
  // Retain pins locally before invoking a callback, including one that releases
  // the caller's original Root/DirectoryLease objects.
  const auto owner = impl_;
  if (!owner || !owner->directory.handle || !Component(child)) {
    Fail(error, ErrorCode::kInvalidPath); return std::nullopt;
  }
  // External callbacks may mutate the caller's arguments. Names/digests used
  // for creation must be the exact bounded values checked before the first
  // callback. Validate the borrowed name before making its bounded copy.
  const std::wstring name = child;
  const FileDigest digest = expected_digest;
  if (expected_size > FileWriteLease::kMaximumFileBytes) {
    Fail(error, ErrorCode::kTooLarge); return std::nullopt;
  }
  if (WriteCancelled(cancelled)) {
    NativeFailure(error, ERROR_OPERATION_ABORTED); return std::nullopt;
  }
  auto lease = std::make_unique<FileWriteLease::Impl>();
  if (!lease->hash.ready()) {
    Fail(error, ErrorCode::kUnsupported); return std::nullopt;
  }
  lease->owner = owner;
  lease->expected_size = expected_size;
  lease->expected_digest = digest;
  FileInfo current;
  std::wstring canonical;
  if (!Query(owner->directory.handle.get(), &current, &canonical, error) ||
      !NoNamedStreams(owner->directory.handle.get(), error)) return std::nullopt;
  if (!current.is_directory || current.identity != owner->directory.info.identity ||
      !SamePath(canonical, owner->directory.canonical)) {
    Fail(error, ErrorCode::kIdentityMismatch); return std::nullopt;
  }
  // Cancellation is checked immediately before the first filesystem mutation.
  // Cancellation observed after acquisition preserves the new partial file.
  // This synchronous syscall is awaited to actual settlement.
  if (WriteCancelled(cancelled)) {
    NativeFailure(error, ERROR_OPERATION_ABORTED); return std::nullopt;
  }
  lease->file = OpenChild(owner->directory.handle.get(), name,
      GENERIC_READ | GENERIC_WRITE, 0, FILE_CREATE, FILE_NON_DIRECTORY_FILE, error);
  if (!lease->file || !CheckChild(lease->file.get(), owner->directory, name,
      &lease->created, &lease->canonical, error) ||
      !NoNamedStreams(lease->file.get(), error)) return std::nullopt;
  if (lease->created.link_count != 1) {
    Fail(error, ErrorCode::kHardLink); return std::nullopt;
  }
  if (lease->created.is_directory || lease->created.size != 0) {
    Fail(error, ErrorCode::kIdentityMismatch); return std::nullopt;
  }
  if (WriteCancelled(cancelled)) {
    NativeFailure(error, ERROR_OPERATION_ABORTED); return std::nullopt;
  }
  Clear(error);
  return FileWriteLease(std::move(lease));
}

HANDLE FileLease::handle() const { return impl_ ? impl_->file.get() : INVALID_HANDLE_VALUE; }
const FileInfo& FileLease::info() const {
  static const FileInfo empty;
  return impl_ ? impl_->info : empty;
}

std::optional<Root> Root::Open(const std::wstring& path, bool create, Error* error) {
  Clear(error);
  if (path.size() < 4 || path.size() > kMaximumPath ||
      !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
      path[1] != L':' || path[2] != L'\\') {
    Fail(error, ErrorCode::kInvalidPath); return std::nullopt;
  }
  std::vector<std::wstring> parts;
  if (!Split(path.substr(3), &parts, error)) return std::nullopt;
  const auto drive = path.substr(0, 3);
  const auto drive_type = GetDriveTypeW(drive.c_str());
  if (drive_type != DRIVE_FIXED && drive_type != DRIVE_REMOVABLE) {
    Fail(error, ErrorCode::kUnsupported); return std::nullopt;
  }
  auto impl = std::make_shared<Impl>();
  PinnedDirectory volume;
  volume.handle = Handle(CreateFileW((L"\\\\?\\" + drive).c_str(),
      FILE_READ_ATTRIBUTES | FILE_TRAVERSE, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
  if (!volume.handle) { NativeFailure(error, GetLastError()); return std::nullopt; }
  if (!Query(volume.handle.get(), &volume.info, &volume.canonical, error)) return std::nullopt;
  // Reject SUBST/directory aliases: the drive must resolve to an actual volume
  // root so every filesystem ancestor is covered by the walk below.
  if (!volume.info.is_directory || volume.canonical.rfind(L"\\\\?\\Volume{", 0) != 0 ||
      volume.canonical.find(L"}\\") != volume.canonical.size() - 2) {
    Fail(error, ErrorCode::kOutsideRoot); return std::nullopt;
  }
  impl->ancestors.push_back(std::move(volume));
  for (size_t i = 0; i < parts.size(); ++i) {
    PinnedDirectory next;
    if (!PinChild(impl->ancestors.back(), parts[i], create && i + 1 == parts.size(),
                   &next, error)) return std::nullopt;
    impl->ancestors.push_back(std::move(next));
  }
  return Root(std::move(impl));
}

bool Root::EnsureDirectory(const std::wstring& relative, Error* error) const {
  Clear(error);
  if (!impl_) return Fail(error, ErrorCode::kInvalidPath);
  std::vector<std::wstring> parts;
  if (!Split(relative, &parts, error) || !impl_->Validate(error)) return false;
  std::vector<PinnedDirectory> pins;
  const auto* parent = &impl_->ancestors.back();
  for (const auto& part : parts) {
    PinnedDirectory next;
    if (!PinChild(*parent, part, true, &next, error)) return false;
    pins.push_back(std::move(next));
    parent = &pins.back();
  }
  return true;
}

std::optional<DirectoryLease> Root::CreateDirectoryExclusive(
    const std::wstring& relative, Error* error) const {
  Clear(error);
  if (!impl_) { Fail(error, ErrorCode::kInvalidPath); return std::nullopt; }
  std::vector<std::wstring> parts;
  if (!Split(relative, &parts, error)) return std::nullopt;
  auto lease = std::make_shared<DirectoryLease::Impl>();
  lease->owner = impl_;
  const auto* parent = impl_->Parents(parts, &lease->ancestors, error);
  if (!parent) return std::nullopt;
  auto& directory = lease->directory;
  directory.handle = OpenChild(parent->handle.get(), parts.back(),
      FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_LIST_DIRECTORY, FILE_SHARE_READ, FILE_CREATE,
      FILE_DIRECTORY_FILE, error);
  if (!directory.handle || !CheckChild(directory.handle.get(), *parent,
      parts.back(), &directory.info, &directory.canonical, error)) return std::nullopt;
  if (!directory.info.is_directory) {
    Fail(error, ErrorCode::kIdentityMismatch); return std::nullopt;
  }
  return DirectoryLease(std::move(lease));
}

std::optional<DirectoryLease> Root::OpenDirectory(const std::wstring& relative,
                                                  bool for_removal, Error* error) const {
  Clear(error);
  if (!impl_) { Fail(error, ErrorCode::kInvalidPath); return std::nullopt; }
  std::vector<std::wstring> parts;
  if (!Split(relative, &parts, error)) return std::nullopt;
  auto lease = std::make_shared<DirectoryLease::Impl>();
  lease->owner = impl_;
  const auto* parent = impl_->Parents(parts, &lease->ancestors, error);
  if (!parent) return std::nullopt;
  auto& directory = lease->directory;
  directory.handle = OpenChild(parent->handle.get(), parts.back(),
      FILE_READ_ATTRIBUTES | FILE_TRAVERSE | FILE_LIST_DIRECTORY | (for_removal ? DELETE : 0),
      FILE_SHARE_READ, FILE_OPEN, FILE_DIRECTORY_FILE, error);
  if (!directory.handle || !CheckChild(directory.handle.get(), *parent, parts.back(),
      &directory.info, &directory.canonical, error)) return std::nullopt;
  if (!directory.info.is_directory) { Fail(error, ErrorCode::kInvalidPath); return std::nullopt; }
  lease->removable = for_removal;
  return DirectoryLease(std::move(lease));
}

bool Root::Inspect(const std::wstring& relative, FileInfo* info, Error* error) const {
  Clear(error);
  if (info) *info = {};
  if (!impl_ || !info) return Fail(error, ErrorCode::kInvalidPath);
  FileInfo observed;
  std::vector<PinnedDirectory> pins;
  auto handle = impl_->Target(relative, FILE_READ_ATTRIBUTES, FILE_SHARE_READ, FILE_OPEN,
                              false, &pins, &observed, error);
  if (!handle) return false;
  *info = observed;
  return true;
}

bool Root::Read(const std::wstring& relative, size_t limit,
                std::vector<uint8_t>* bytes, FileInfo* info, Error* error) const {
  Clear(error);
  if (bytes) bytes->clear();
  if (info) *info = {};
  if (!impl_ || !bytes || !info) return Fail(error, ErrorCode::kInvalidPath);
  if (limit > kMaximumBytes) return Fail(error, ErrorCode::kTooLarge);
  auto lease = OpenRead(relative, limit, error);
  if (!lease) return false;
  const auto observed = lease->info();
  std::vector<uint8_t> result;
  try { result.resize(static_cast<size_t>(observed.size)); }
  catch (const std::bad_alloc&) { return Fail(error, ErrorCode::kIo, ERROR_NOT_ENOUGH_MEMORY); }
  size_t offset = 0;
  while (offset < result.size()) {
    const auto count = static_cast<DWORD>((std::min)(size_t{65536}, result.size() - offset));
    DWORD read = 0;
    if (!ReadFile(lease->handle(), result.data() + offset, count, &read, nullptr))
      return NativeFailure(error, GetLastError());
    if (read == 0) return Fail(error, ErrorCode::kIdentityMismatch);
    offset += read;
  }
  FileInfo final_info;
  std::wstring final_name;
  if (!Query(lease->handle(), &final_info, &final_name, error)) return false;
  if (final_info.identity != observed.identity || final_info.size != observed.size ||
      final_info.link_count != 1) return Fail(error, ErrorCode::kIdentityMismatch);
  *bytes = std::move(result);
  *info = final_info;
  return true;
}

std::optional<FileLease> Root::OpenRead(const std::wstring& relative,
                                       uint64_t limit, Error* error) const {
  Clear(error);
  if (!impl_) { Fail(error, ErrorCode::kInvalidPath); return std::nullopt; }
  if (limit > kMaximumInspectionBytes) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  auto lease = std::make_unique<FileLease::Impl>();
  lease->owner = impl_;
  lease->file = impl_->Target(relative, GENERIC_READ, FILE_SHARE_READ, FILE_OPEN, true,
                              &lease->ancestors, &lease->info, error);
  if (!lease->file) return std::nullopt;
  if (lease->info.link_count != 1) { Fail(error, ErrorCode::kHardLink); return std::nullopt; }
  if (lease->info.size > limit) { Fail(error, ErrorCode::kTooLarge); return std::nullopt; }
  return FileLease(std::move(lease));
}

bool Root::CreateExclusive(const std::wstring& relative, const std::vector<uint8_t>& bytes,
                           FileInfo* info, Error* error) const {
  Clear(error);
  if (info) *info = {};
  if (!impl_ || !info) return Fail(error, ErrorCode::kInvalidPath);
  if (bytes.size() > kMaximumBytes) return Fail(error, ErrorCode::kTooLarge);
  FileInfo created;
  std::vector<PinnedDirectory> pins;
  auto handle = impl_->Target(relative, GENERIC_WRITE | FILE_READ_ATTRIBUTES, 0, FILE_CREATE,
                              true, &pins, &created, error);
  if (!handle) return false;
  if (created.link_count != 1) return Fail(error, ErrorCode::kHardLink);
  size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = static_cast<DWORD>((std::min)(size_t{65536}, bytes.size() - offset));
    DWORD written = 0;
    if (!WriteFile(handle.get(), bytes.data() + offset, count, &written, nullptr))
      return NativeFailure(error, GetLastError());
    if (written == 0) return Fail(error, ErrorCode::kIo, ERROR_WRITE_FAULT);
    offset += written;
  }
  if (!FlushFileBuffers(handle.get())) return NativeFailure(error, GetLastError());
  FileInfo final_info;
  std::wstring canonical;
  if (!Query(handle.get(), &final_info, &canonical, error)) return false;
  if (created.identity != final_info.identity || final_info.size != bytes.size() ||
      final_info.link_count != 1) return Fail(error, ErrorCode::kIdentityMismatch);
  *info = final_info;
  return true;
}

bool Root::RemoveOwned(const std::wstring& relative, const FileIdentity& expected,
                       Error* error) const {
  Clear(error);
  if (!impl_) return Fail(error, ErrorCode::kInvalidPath);
  FileInfo observed;
  std::vector<PinnedDirectory> pins;
  auto handle = impl_->Target(relative, DELETE | FILE_READ_ATTRIBUTES, 0, FILE_OPEN, true,
                              &pins, &observed, error);
  if (!handle) return false;
  if (observed.link_count != 1) return Fail(error, ErrorCode::kHardLink);
  if (observed.identity != expected) return Fail(error, ErrorCode::kIdentityMismatch);
  FILE_DISPOSITION_INFO disposition{TRUE};
  if (!SetFileInformationByHandle(handle.get(), FileDispositionInfo, &disposition,
                                   sizeof(disposition))) return NativeFailure(error, GetLastError());
  return true;
}

std::optional<Lease> Root::Lock(const std::wstring& relative, Error* error) const {
  Clear(error);
  if (!impl_) { Fail(error, ErrorCode::kInvalidPath); return std::nullopt; }
  auto lease = std::make_unique<Lease::Impl>();
  lease->owner = impl_;
  FileInfo observed;
  lease->file = impl_->Target(relative, GENERIC_READ | GENERIC_WRITE, 0, FILE_OPEN_IF, true,
                              &lease->ancestors, &observed, error);
  if (!lease->file) return std::nullopt;
  if (observed.link_count != 1) { Fail(error, ErrorCode::kHardLink); return std::nullopt; }
  return Lease(std::move(lease));
}

}  // namespace ryhze::secure_files
