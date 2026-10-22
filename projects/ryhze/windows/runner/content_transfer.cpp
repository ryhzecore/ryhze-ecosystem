#include "content_transfer.h"
#include <bcrypt.h>
#include <algorithm>
#include <utility>

namespace ryhze {
namespace {
namespace fs = secure_files;
bool Hex(const std::string& value, size_t size) {
  return value.size() == size && std::all_of(value.begin(), value.end(),
    [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
bool Identity(const std::string& value) {
  return value.empty() || (value.size() == 49 && value[16] == ':' &&
    Hex(value.substr(0, 16), 16) && Hex(value.substr(17), 32));
}
std::string HexDigest(const fs::FileDigest& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string out;
  for (auto byte : value) { out += hex[byte >> 4]; out += hex[byte & 15]; }
  return out;
}
std::wstring Directory(const ContentTransferRequest& request) {
  return L"incoming\\transfer-" + std::to_wstring(request.slot);
}
std::wstring Child(const ContentTransferRequest& request) {
  return std::wstring(request.operation_id.begin(), request.operation_id.end()) +
      L"-" + std::to_wstring(request.attempt) + L".bin";
}
std::string Relative(const ContentTransferRequest& request) {
  return "transfer-" + std::to_string(request.slot) + "/" + request.operation_id +
      "-" + std::to_string(request.attempt) + ".bin";
}
std::string Failure(const fs::Error& error) {
  if (error.win32 == ERROR_OPERATION_ABORTED) return "cancelled";
  if (error.win32 == ERROR_DISK_FULL || error.win32 == ERROR_HANDLE_DISK_FULL ||
      error.win32 == ERROR_DISK_QUOTA_EXCEEDED || error.code == fs::ErrorCode::kTooLarge) return "storage";
  if (error.win32 == ERROR_SHARING_VIOLATION || error.win32 == ERROR_LOCK_VIOLATION ||
      error.code == fs::ErrorCode::kAlreadyExists || error.code == fs::ErrorCode::kBusy) return "conflict";
  if (error.code == fs::ErrorCode::kInvalidPath) return "invalidData";
  return "recovery";
}
bool Missing(const fs::Error& error) {
  return error.code == fs::ErrorCode::kNotFound || error.win32 == ERROR_FILE_NOT_FOUND ||
      error.win32 == ERROR_PATH_NOT_FOUND;
}
class Hash {
 public:
  Hash() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
      BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Hash() { if (hash_) BCryptDestroyHash(hash_); if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0); }
  bool Add(const std::vector<uint8_t>& bytes) {
    return hash_ && bytes.size() <= ContentTransfer::kChunkBytes &&
        BCryptHashData(hash_, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0;
  }
  bool Finish(std::string* value) {
    fs::FileDigest digest{};
    if (!hash_ || BCryptFinishHash(hash_, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    *value = HexDigest(digest); return true;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};
bool ReadAt(HANDLE handle, uint64_t offset, size_t size, std::vector<uint8_t>* bytes) {
  LARGE_INTEGER position{}; position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) return false;
  bytes->resize(size);
  size_t total = 0;
  while (total < size) {
    DWORD count = 0;
    if (!ReadFile(handle, bytes->data() + total, static_cast<DWORD>(size - total), &count, nullptr) || !count) return false;
    total += count;
  }
  return true;
}
bool Same(const ContentTransferRequest& a, const ContentTransferRequest& b) {
  return a.slot == b.slot && a.attempt == b.attempt && a.operation_id == b.operation_id &&
    a.expected_size == b.expected_size && a.expected_sha256 == b.expected_sha256;
}
}  // namespace

std::string ContentTransferIdentity(const fs::FileIdentity& value) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (int shift = 60; shift >= 0; shift -= 4) result += hex[(value.volume >> shift) & 15];
  result += ':';
  for (auto byte : value.id) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
bool ValidContentTransferRequest(const ContentTransferRequest& r) {
  return r.slot < ContentTransfer::kSlotCount && r.attempt < 2 && Hex(r.operation_id, 64) &&
    r.expected_size > 0 && r.expected_size <= ContentTransfer::kMaximumBytes &&
    r.observed_bytes <= r.expected_size && Identity(r.root_identity) &&
    Identity(r.directory_identity) && Identity(r.source_identity) &&
    (r.observed_sha256.empty() || Hex(r.observed_sha256, 64));
}

struct ContentTransfer::Impl {
  explicit Impl(std::wstring path) : private_root(std::move(path)) {}
  std::wstring private_root;
  std::atomic<bool> cancelled{false};
  std::optional<fs::Root> root;
  std::optional<fs::Lease> lock;
  std::optional<fs::DirectoryLease> directory;
  std::optional<fs::FileWriteLease> writer;
  ContentTransferRequest active;
  ContentTransferResult evidence;

  void Release() { writer.reset(); directory.reset(); lock.reset(); root.reset(); }
  ContentTransferResult Fail(std::string error, bool release = true) {
    if (release) Release();
    ContentTransferResult out; out.error = std::move(error); return out;
  }
  std::string Open(const ContentTransferRequest& request, bool create, bool removal) {
    fs::Error error;
    root = fs::Root::Open(private_root, false, &error);
    if (!root) return Failure(error);
    fs::FileInfo info;
    if (!root->Inspect(L"content-staging", &info, &error) || !info.is_directory) return "recovery";
    evidence = {}; evidence.root_identity = ContentTransferIdentity(info.identity);
    evidence.source_relative = Relative(request);
    if (!request.root_identity.empty() && request.root_identity != evidence.root_identity) return "recovery";
    // Mutations hold the narrow cross-process lease. Read-only inspection must
    // not create even a coordination file; its directory/file handles pin the
    // observed identities and exclude concurrent writes to the inspected file.
    if (create || removal) {
      lock = root->Lock(L"incoming\\transfer-" + std::to_wstring(request.slot) + L".lock", &error);
      if (!lock) return Failure(error);
    }
    if (create && request.directory_identity.empty()) {
      directory = root->CreateDirectoryExclusive(Directory(request), &error);
    } else {
      directory = root->OpenDirectory(Directory(request), removal, &error);
    }
    if (!directory) return !create && Missing(error) ? "missing" : Failure(error);
    evidence.directory_identity = ContentTransferIdentity(directory->info().identity);
    if (!request.directory_identity.empty() && request.directory_identity != evidence.directory_identity) return "recovery";
    std::vector<std::wstring> names;
    if (!directory->ListNames(2, &names, &error)) return "recovery";
    auto other = request; other.attempt = 1 - request.attempt;
    for (const auto& name : names) if (name != Child(request) && name != Child(other)) return "recovery";
    return {};
  }
  std::string Observe(HANDLE handle, const fs::FileInfo& info) {
    if (info.size > kMaximumBytes) return "recovery";
    Hash hash;
    std::vector<uint8_t> bytes;
    for (uint64_t offset = 0; offset < info.size;) {
      if (cancelled.load()) return "cancelled";
      const auto count = static_cast<size_t>((std::min)(uint64_t{kChunkBytes}, info.size - offset));
      if (!ReadAt(handle, offset, count, &bytes)) return cancelled.load() ? "cancelled" : "recovery";
      if (!hash.Add(bytes)) return "unsupported";
      offset += count;
    }
    if (cancelled.load()) return "cancelled";
    if (!hash.Finish(&evidence.observed_sha256)) return "unsupported";
    evidence.observed_bytes = info.size;
    evidence.source_identity = ContentTransferIdentity(info.identity);
    return {};
  }
  bool ExactObservation(const ContentTransferRequest& r) const {
    return !r.directory_identity.empty() && !r.source_identity.empty() && !r.observed_sha256.empty() &&
      r.directory_identity == evidence.directory_identity && r.source_identity == evidence.source_identity &&
      r.observed_bytes == evidence.observed_bytes && r.observed_sha256 == evidence.observed_sha256;
  }
  bool Active(const ContentTransferRequest& request) const {
    return writer && Same(request, active) && request.root_identity == evidence.root_identity &&
      request.directory_identity == evidence.directory_identity &&
      request.source_identity == evidence.source_identity && !request.source_identity.empty() &&
      request.observed_sha256.empty() && request.observed_bytes == writer->written();
  }
};

ContentTransfer::ContentTransfer(std::wstring root) : impl_(std::make_unique<Impl>(std::move(root))) {}
ContentTransfer::~ContentTransfer() = default;
void ContentTransfer::Cancel() noexcept { impl_->cancelled.store(true); }
void ContentTransfer::Drain() { impl_->Release(); }
void ContentTransfer::ResetCancellation() {
  // Cancellation of an existing writer cannot be cleared by a later append.
  // Only a settled, released writer permits a fresh operation generation.
  if (!impl_->writer) impl_->cancelled.store(false);
}

ContentTransferResult ContentTransfer::Begin(const ContentTransferRequest& request,
                                           const std::optional<ContentTransferRequest>& resume) {
  auto& p = *impl_;
  if (p.writer) return p.Fail("conflict", false);
  if (!ValidContentTransferRequest(request) || !request.source_identity.empty() ||
      request.observed_bytes != 0 || !request.observed_sha256.empty()) return p.Fail("invalidData");
  if (p.cancelled.load()) return p.Fail("cancelled");
  if (resume && (!ValidContentTransferRequest(*resume) || resume->slot != request.slot ||
      resume->attempt == request.attempt || resume->operation_id != request.operation_id ||
      resume->expected_size != request.expected_size || resume->expected_sha256 != request.expected_sha256 ||
      resume->directory_identity.empty() || resume->directory_identity != request.directory_identity ||
      resume->root_identity != request.root_identity || resume->observed_bytes >= request.expected_size)) return p.Fail("invalidData");
  // Capacity includes the full new file while any prior prefix remains pinned.
  ULARGE_INTEGER available{};
  if (!GetDiskFreeSpaceExW(p.private_root.c_str(), &available, nullptr, nullptr) ||
      available.QuadPart < request.expected_size + 64ull * 1024 * 1024) return p.Fail("storage");
  const auto failure = p.Open(request, true, false);
  if (!failure.empty()) return p.Fail(failure);
  std::optional<fs::FileLease> prefix;
  fs::Error error;
  if (resume) {
    prefix = p.directory->OpenRead(Child(*resume), kMaximumBytes, &error);
    if (!prefix) return p.Fail(Failure(error));
    if (resume->source_identity != ContentTransferIdentity(prefix->info().identity) ||
        resume->observed_bytes != prefix->info().size) return p.Fail("recovery");
    const auto observed = p.Observe(prefix->handle(), prefix->info());
    if (!observed.empty()) return p.Fail(observed);
    if (!p.ExactObservation(*resume)) return p.Fail("recovery");
  }
  p.writer = p.directory->BeginWriteExclusive(Child(request), request.expected_size,
      request.expected_sha256, &error, [&p] { return p.cancelled.load(); });
  if (!p.writer) return p.Fail(Failure(error));
  if (prefix) {
    std::vector<uint8_t> bytes;
    for (uint64_t offset = 0; offset < prefix->info().size;) {
      const auto count = static_cast<size_t>((std::min)(uint64_t{kChunkBytes}, prefix->info().size - offset));
      if (p.cancelled.load()) return p.Fail("cancelled");
      if (!ReadAt(prefix->handle(), offset, count, &bytes)) return p.Fail("recovery");
      if (!p.writer->Append(bytes.data(), bytes.size(), &error, [&p] { return p.cancelled.load(); })) return p.Fail(Failure(error));
      offset += count;
    }
  }
  p.active = request;
  p.evidence.state = "writing";
  p.evidence.source_identity = ContentTransferIdentity(p.writer->created_identity());
  p.evidence.observed_sha256.clear();
  p.evidence.observed_bytes = p.writer->written();
  return p.evidence;
}
ContentTransferResult ContentTransfer::Append(const ContentTransferRequest& request,
                                            const std::vector<uint8_t>& bytes) {
  auto& p = *impl_;
  if (!ValidContentTransferRequest(request) || bytes.empty() || bytes.size() > kChunkBytes) return p.Fail("invalidData", false);
  if (!p.Active(request)) return p.Fail("conflict", false);
  fs::Error error;
  if (!p.writer->Append(bytes.data(), bytes.size(), &error, [&p] { return p.cancelled.load(); })) return p.Fail(Failure(error));
  p.evidence.observed_bytes = p.writer->written();
  return p.evidence;
}
ContentTransferResult ContentTransfer::Finish(const ContentTransferRequest& request) {
  auto& p = *impl_;
  if (!ValidContentTransferRequest(request)) return p.Fail("invalidData", false);
  if (!p.Active(request)) return p.Fail("conflict", false);
  fs::Error error; fs::FileInfo info;
  if (!p.writer->Finish(&info, &error, [&p] { return p.cancelled.load(); })) return p.Fail(Failure(error));
  p.evidence.state = "complete";
  p.evidence.source_identity = ContentTransferIdentity(info.identity);
  p.evidence.observed_bytes = info.size;
  p.evidence.observed_sha256 = HexDigest(request.expected_sha256);
  auto result = p.evidence; p.Release(); return result;
}
ContentTransferResult ContentTransfer::Inspect(const ContentTransferRequest& request) {
  auto& p = *impl_;
  if (p.writer) return p.Fail("conflict", false);
  if (!ValidContentTransferRequest(request)) return p.Fail("invalidData");
  if (p.cancelled.load()) return p.Fail("cancelled");
  const auto failure = p.Open(request, false, false);
  if (!failure.empty() && failure != "missing") return p.Fail(failure);
  fs::Error error;
  auto source = p.directory ? p.directory->OpenRead(Child(request), kMaximumBytes, &error) : std::nullopt;
  if (!source) {
    if (failure != "missing" && !Missing(error)) return p.Fail(Failure(error));
    p.evidence.state = "missing"; auto result = p.evidence; p.Release(); return result;
  }
  // A known filename is not ownership. If creation's identity was never
  // durably acknowledged, retain the orphan for explicit manual recovery.
  if (request.source_identity.empty() || request.directory_identity.empty() ||
      request.source_identity != ContentTransferIdentity(source->info().identity) ||
      source->info().size > request.expected_size) return p.Fail("recovery");
  const auto observed = p.Observe(source->handle(), source->info());
  if (!observed.empty()) return p.Fail(observed);
  p.evidence.state = p.evidence.observed_bytes == request.expected_size &&
      p.evidence.observed_sha256 == HexDigest(request.expected_sha256) ? "complete" : "checkpoint";
  auto result = p.evidence; source.reset(); p.Release(); return result;
}
ContentTransferResult ContentTransfer::Discard(const ContentTransferRequest& request) {
  auto& p = *impl_;
  if (p.writer) return p.Fail("conflict", false);
  const bool uncreated = request.source_identity.empty();
  if (!ValidContentTransferRequest(request) || request.root_identity.empty() ||
      (uncreated ? (request.observed_bytes != 0 || !request.observed_sha256.empty()) :
        request.directory_identity.empty())) return p.Fail("invalidData");
  if (p.cancelled.load()) return p.Fail("cancelled");
  const auto failure = p.Open(request, false, true);
  if (failure == "missing") {
    p.evidence.state = "discarded";
    p.evidence.directory_identity = request.directory_identity;
    p.evidence.source_identity = request.source_identity;
    p.evidence.observed_bytes = request.observed_bytes;
    p.evidence.observed_sha256 = request.observed_sha256;
    auto result = p.evidence; p.Release(); return result;
  }
  if (!failure.empty()) return p.Fail(failure);
  if (request.directory_identity.empty()) return p.Fail("recovery");
  fs::Error error;
  auto source = p.directory->OpenForRemoval(Child(request), kMaximumBytes, &error);
  if (!source) {
    if (!Missing(error)) return p.Fail(Failure(error));
    // Reconcile a durable exact removal intent after the file is already absent.
    // These fields identify the removed plan, not a fresh present-file reading.
    p.evidence.state = "discarded";
    p.evidence.source_identity = request.source_identity;
    p.evidence.observed_bytes = request.observed_bytes;
    p.evidence.observed_sha256 = request.observed_sha256;
    auto result = p.evidence; p.Release(); return result;
  }
  // A persisted creation identity can reconcile actual absence before its first
  // hash checkpoint. It never authorizes deleting a present, unhashed file.
  if (uncreated || request.observed_sha256.empty()) return p.Fail("recovery");
  if (request.source_identity != ContentTransferIdentity(source->info().identity) ||
      request.observed_bytes != source->info().size) return p.Fail("recovery");
  const auto observed = p.Observe(source->handle(), source->info());
  if (!observed.empty()) return p.Fail(observed);
  if (!p.ExactObservation(request)) return p.Fail("recovery");
  if (p.cancelled.load()) return p.Fail("cancelled");
  if (!source->Remove(&error)) return p.Fail(Failure(error));
  p.evidence.state = "discarded";
  auto result = p.evidence; source.reset(); p.Release(); return result;
}
}  // namespace ryhze
