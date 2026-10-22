#ifndef RUNNER_SECURE_FILES_H_
#define RUNNER_SECURE_FILES_H_

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ryhze::secure_files {

enum class ErrorCode {
  kNone,
  kInvalidPath,
  kNotFound,
  kAlreadyExists,
  kReparsePoint,
  kHardLink,
  kIdentityMismatch,
  kOutsideRoot,
  kTooLarge,
  kBusy,
  kUnsupported,
  kIo,
};

// Closed diagnostic vocabulary. No input path or file contents are retained.
struct Error {
  ErrorCode code = ErrorCode::kNone;
  DWORD win32 = ERROR_SUCCESS;
};

struct FileIdentity {
  uint64_t volume = 0;
  std::array<uint8_t, 16> id{};
  bool operator==(const FileIdentity& other) const;
  bool operator!=(const FileIdentity& other) const { return !(*this == other); }
};

struct FileInfo {
  FileIdentity identity;
  uint64_t size = 0;
  bool is_directory = false;
  uint32_t link_count = 0;
};

// An exclusive, non-truncating lock-file handle. Cooperating processes must use
// the same relative lock name. This is not a same-user-code trust boundary.
class Lease {
 public:
  ~Lease();
  Lease(Lease&&) noexcept;
  Lease& operator=(Lease&&) noexcept;
  Lease(const Lease&) = delete;
  Lease& operator=(const Lease&) = delete;

 private:
  struct Impl;
  explicit Lease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class Root;
};

// Borrowed read-only handle and its identity remain valid until this lease is
// destroyed. Do not close the handle or share its mutable seek position between
// concurrent readers. Parent pins and no-write/no-delete sharing remain held.
class FileLease {
 public:
  ~FileLease();
  FileLease(FileLease&&) noexcept;
  FileLease& operator=(FileLease&&) noexcept;
  FileLease(const FileLease&) = delete;
  FileLease& operator=(const FileLease&) = delete;
  HANDLE handle() const;
  const FileInfo& info() const;

 private:
  struct Impl;
  explicit FileLease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class Root;
  friend class DirectoryLease;
};

using FileDigest = std::array<uint8_t, 32>;

#if defined(RYHZE_SECURE_FILES_NATIVE_TEST)
// Test-target-only instrumentation. No production symbols or channel controls.
// Allocation failure and readback observations belong to the calling thread.
namespace testing {
void FailNextCanonicalPathAllocation() noexcept;
uint64_t CompletedReadbackBytes() noexcept;
}
#endif

// Exclusive bounded writer for one new direct child of a pinned directory.
// Finish flushes and re-reads the SAME handle before reporting exact identity,
// length and SHA-256. These observations do not establish content authority or
// installed/launch state. No operation here overwrites, deletes or activates.
//
// Cancel is safe concurrently and never closes a handle or reports settlement.
// Append/Finish are synchronous; the owner must await their actual return before
// destroying/moving the lease. In particular, a cancellation callback must not
// destroy/move it. Nested/concurrent Append/Finish fail Busy without affecting
// the active call. Other failures after acquisition permanently close writing;
// destruction only releases pins, preserving complete and partial files alike.
class FileWriteLease {
 public:
  static constexpr size_t kMaximumChunkBytes = 256 * 1024;
  static constexpr uint64_t kMaximumFileBytes = 1024ull * 1024 * 1024;
  ~FileWriteLease();
  FileWriteLease(FileWriteLease&&) noexcept;
  FileWriteLease& operator=(FileWriteLease&&) noexcept;
  FileWriteLease(const FileWriteLease&) = delete;
  FileWriteLease& operator=(const FileWriteLease&) = delete;

  uint64_t written() const;
  // Immutable identity captured on the exclusively created handle. Persist it
  // before admitting further writes; it is ownership evidence, not a verified
  // length/digest or content authority. Owner-thread access while lease is live.
  const FileIdentity& created_identity() const;
  void Cancel() noexcept;
  // Caller retains bytes unchanged through return. A zero-byte append is a
  // checked no-op. A cancelled/failed writer cannot be resumed or retried here.
  bool Append(const uint8_t* bytes, size_t size, Error* error,
              const std::function<bool()>& cancelled = {});
  // Empty files are supported. Outputs are cleared on every failed attempt.
  // Success leaves the exclusive handle pinned until this lease is released.
  bool Finish(FileInfo* info, Error* error,
              const std::function<bool()>& cancelled = {});

 private:
  struct Impl;
  explicit FileWriteLease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class DirectoryLease;
};

// A regular single-link file pinned for bounded verification and deletion on
// that exact handle. No writes, replacement, attributes or recursive operations.
class FileRemovalLease {
 public:
  ~FileRemovalLease();
  FileRemovalLease(FileRemovalLease&&) noexcept;
  FileRemovalLease& operator=(FileRemovalLease&&) noexcept;
  FileRemovalLease(const FileRemovalLease&) = delete;
  FileRemovalLease& operator=(const FileRemovalLease&) = delete;
  HANDLE handle() const;
  const FileInfo& info() const;
  // Rechecks identity/link count, marks deletion and closes the same handle.
  bool Remove(Error* error);
 private:
  struct Impl;
  explicit FileRemovalLease(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
  friend class DirectoryLease;
};

// Lifetime pin for an exact directory and its ancestors. Only a lease explicitly
// opened for removal can acquire child deletion handles or remove itself empty.
// Destruction alone never deletes anything. Direct child names only.
class DirectoryLease {
 public:
  ~DirectoryLease();
  DirectoryLease(DirectoryLease&&) noexcept;
  DirectoryLease& operator=(DirectoryLease&&) noexcept;
  DirectoryLease(const DirectoryLease&) = delete;
  DirectoryLease& operator=(const DirectoryLease&) = delete;
  const FileInfo& info() const;
  bool ListNames(size_t maximum_entries, std::vector<std::wstring>* names, Error* error) const;
  std::optional<FileLease> OpenRead(const std::wstring& child, uint64_t byte_limit,
                                   Error* error) const;
  std::optional<FileRemovalLease> OpenForRemoval(const std::wstring& child,
                                                uint64_t byte_limit, Error* error) const;
  // Create through the pinned directory handle, never overwrite an existing
  // child. Failed writes preserve their partial file for explicit recovery.
  bool CreateExclusive(const std::wstring& child, const std::vector<uint8_t>& bytes,
                       FileInfo* info, Error* error) const;
  // New bounded regular file only. SHA-256 compares caller-provided expected
  // bytes; it does not authenticate the caller's manifest or grant rights.
  std::optional<FileWriteLease> BeginWriteExclusive(
      const std::wstring& child, uint64_t expected_size,
      const FileDigest& expected_digest, Error* error,
      const std::function<bool()>& cancelled = {}) const;
  bool RemoveEmpty(Error* error);

 private:
  struct Impl;
  explicit DirectoryLease(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
  friend class Root;
};

// Local drive-absolute roots only; relative names use backslashes. No UNC,
// device namespace, alternate streams, dot segments, or reparse traversal.
// Ancestor handles remain pinned for the Root lifetime and child ancestors for
// each operation. Read/Inspect results are observations, never execution rights.
class Root {
 public:
  static constexpr size_t kMaximumBytes = 16 * 1024 * 1024;
  static constexpr uint64_t kMaximumInspectionBytes = 1024ull * 1024 * 1024;
  static std::optional<Root> Open(const std::wstring& absolute_root,
                                  bool create_root_if_missing, Error* error);
  ~Root();
  Root(Root&&) noexcept;
  Root& operator=(Root&&) noexcept;
  Root(const Root&) = delete;
  Root& operator=(const Root&) = delete;

  bool EnsureDirectory(const std::wstring& relative, Error* error) const;
  // Creates only the final component; existing ancestors must be safe. Fails if
  // the final name already exists, including a directory. The returned lease
  // pins the exact new directory against rename/replacement until released.
  std::optional<DirectoryLease> CreateDirectoryExclusive(
      const std::wstring& relative, Error* error) const;
  // Existing directory only; no recreation. A removal lease must be identity
  // checked by its owner before use. Acquisitions use the held directory handle.
  std::optional<DirectoryLease> OpenDirectory(const std::wstring& relative,
                                             bool for_removal, Error* error) const;
  bool Inspect(const std::wstring& relative, FileInfo* info, Error* error) const;
  std::optional<FileLease> OpenRead(const std::wstring& relative,
                                   uint64_t byte_limit, Error* error) const;
  // Outputs are cleared on failure; bytes and identity come from one handle.
  bool Read(const std::wstring& relative, size_t byte_limit,
            std::vector<uint8_t>* bytes, FileInfo* info, Error* error) const;
  // Creates a new regular single-link file; never truncates an existing name.
  // A failed write leaves its newly-created partial file for explicit recovery.
  bool CreateExclusive(const std::wstring& relative,
                       const std::vector<uint8_t>& bytes, FileInfo* info,
                       Error* error) const;
  // Deletes only a regular single-link file matching the observed identity.
  // No directory/recursive deletion, attribute clearing or blind overwrite.
  bool RemoveOwned(const std::wstring& relative,
                   const FileIdentity& expected, Error* error) const;
  std::optional<Lease> Lock(const std::wstring& relative, Error* error) const;

 private:
  struct Impl;
  explicit Root(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace ryhze::secure_files

#endif  // RUNNER_SECURE_FILES_H_
