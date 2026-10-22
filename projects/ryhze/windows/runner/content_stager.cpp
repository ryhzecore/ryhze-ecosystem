#include "content_stager.h"
#include "secure_files.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <iterator>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace ryhze {
namespace {
namespace fs = secure_files;
constexpr size_t kMaximumCheckpoint = 2048;
constexpr size_t kMaximumSourceName = 512;
constexpr uint8_t kMagic[] = {'R', 'Y', 'H', 'Z', 'C', 'S', '0', '1'};

bool Cancelled(const std::function<bool()>& cancelled) {
  try { return cancelled && cancelled(); }
  catch (...) { return true; }
}
bool Nonzero(const ContentDigest& value) {
  return std::any_of(value.begin(), value.end(), [](uint8_t byte) { return byte != 0; });
}
bool IdentityText(const std::string& value) {
  if (value.size() != 49 || value[16] != ':') return false;
  for (size_t i = 0; i < value.size(); ++i) {
    if (i == 16) continue;
    if (!((value[i] >= '0' && value[i] <= '9') ||
          (value[i] >= 'a' && value[i] <= 'f'))) return false;
  }
  return true;
}
std::string Identity(const fs::FileIdentity& identity) {
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  for (int shift = 60; shift >= 0; shift -= 4)
    result += hex[(identity.volume >> shift) & 15];
  result += ':';
  for (uint8_t byte : identity.id) { result += hex[byte >> 4]; result += hex[byte & 15]; }
  return result;
}
class Hash {
 public:
  Hash() {
    if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0)
      BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0);
  }
  ~Hash() {
    if (hash_) BCryptDestroyHash(hash_);
    if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
  }
  bool Add(const uint8_t* bytes, size_t size) {
    return hash_ && size <= ContentStager::kChunkBytes &&
        BCryptHashData(hash_, const_cast<PUCHAR>(bytes), static_cast<ULONG>(size), 0) >= 0;
  }
  bool Finish(ContentDigest* digest) {
    return hash_ && BCryptFinishHash(hash_, digest->data(),
                                    static_cast<ULONG>(digest->size()), 0) >= 0;
  }
 private:
  BCRYPT_ALG_HANDLE algorithm_ = nullptr;
  BCRYPT_HASH_HANDLE hash_ = nullptr;
};
ContentStageState NativeFailure(DWORD native) {
  if (native == ERROR_OPERATION_ABORTED) return ContentStageState::cancelled;
  if (native == ERROR_DISK_FULL || native == ERROR_HANDLE_DISK_FULL ||
      native == ERROR_DISK_QUOTA_EXCEEDED) return ContentStageState::quota;
  if (native == ERROR_SHARING_VIOLATION || native == ERROR_LOCK_VIOLATION)
    return ContentStageState::conflict;
  if (native == ERROR_FILE_EXISTS || native == ERROR_ALREADY_EXISTS)
    return ContentStageState::conflict;
  return ContentStageState::recovery;
}
ContentStageState FileFailure(const fs::Error& error) {
  if (error.win32 != ERROR_SUCCESS) return NativeFailure(error.win32);
  switch (error.code) {
    case fs::ErrorCode::kInvalidPath: return ContentStageState::invalid;
    case fs::ErrorCode::kTooLarge: return ContentStageState::quota;
    case fs::ErrorCode::kAlreadyExists:
    case fs::ErrorCode::kBusy: return ContentStageState::conflict;
    case fs::ErrorCode::kUnsupported: return ContentStageState::unavailable;
    default: return ContentStageState::recovery;
  }
}
bool ReadAt(HANDLE file, uint64_t offset, size_t size,
            std::vector<uint8_t>* bytes, DWORD* failure) {
  bytes->assign(size, 0);
  LARGE_INTEGER position{};
  position.QuadPart = static_cast<LONGLONG>(offset);
  if (!SetFilePointerEx(file, position, nullptr, FILE_BEGIN)) {
    *failure = GetLastError(); return false;
  }
  size_t total = 0;
  while (total < size) {
    DWORD read = 0;
    if (!ReadFile(file, bytes->data() + total, static_cast<DWORD>(size - total), &read, nullptr)) {
      *failure = GetLastError(); return false;
    }
    if (!read) { *failure = ERROR_HANDLE_EOF; return false; }
    total += read;
  }
  return true;
}
ContentStageState VerifySource(fs::FileLease* source, const ContentStageRequest& request,
                               const std::function<bool()>& cancelled) {
  if (source->info().size != request.expected_size) return ContentStageState::recovery;
  Hash hash;
  std::vector<uint8_t> buffer;
  for (uint64_t offset = 0; offset < request.expected_size;) {
    if (Cancelled(cancelled)) return ContentStageState::cancelled;
    const auto size = static_cast<size_t>((std::min)(
        uint64_t{ContentStager::kChunkBytes}, request.expected_size - offset));
    DWORD failure = 0;
    if (!ReadAt(source->handle(), offset, size, &buffer, &failure)) return NativeFailure(failure);
    if (!hash.Add(buffer.data(), buffer.size())) return ContentStageState::unavailable;
    offset += size;
  }
  ContentDigest digest{};
  if (!hash.Finish(&digest)) return ContentStageState::unavailable;
  return digest == request.expected_sha256 ? ContentStageState::staged : ContentStageState::recovery;
}
void Append64(std::vector<uint8_t>* bytes, uint64_t value) {
  for (unsigned shift = 0; shift < 64; shift += 8)
    bytes->push_back(static_cast<uint8_t>(value >> shift));
}
void AppendIdentity(std::vector<uint8_t>* bytes, const fs::FileIdentity& identity) {
  Append64(bytes, identity.volume);
  bytes->insert(bytes->end(), identity.id.begin(), identity.id.end());
}
std::vector<uint8_t> Checkpoint(const ContentStageRequest& request,
                                const fs::FileIdentity& source,
                                const fs::FileIdentity& root) {
  std::vector<uint8_t> bytes(std::begin(kMagic), std::end(kMagic));
  bytes.insert(bytes.end(), request.operation_id.begin(), request.operation_id.end());
  bytes.insert(bytes.end(), request.manifest_sha256.begin(), request.manifest_sha256.end());
  Append64(&bytes, request.slot);
  Append64(&bytes, request.expected_size);
  bytes.insert(bytes.end(), request.expected_sha256.begin(), request.expected_sha256.end());
  Append64(&bytes, request.source_relative.size());
  for (wchar_t ch : request.source_relative) {
    bytes.push_back(static_cast<uint8_t>(ch));
    bytes.push_back(static_cast<uint8_t>(ch >> 8));
  }
  AppendIdentity(&bytes, source);
  AppendIdentity(&bytes, root);
  Hash hash;
  ContentDigest digest{};
  if (!hash.Add(bytes.data(), bytes.size()) || !hash.Finish(&digest)) return {};
  bytes.insert(bytes.end(), digest.begin(), digest.end());
  return bytes;
}
bool ValidCheckpoint(const std::vector<uint8_t>& bytes) {
  if (bytes.size() < 208 || bytes.size() > kMaximumCheckpoint ||
      !std::equal(std::begin(kMagic), std::end(kMagic), bytes.begin())) return false;
  Hash hash;
  ContentDigest digest{};
  if (!hash.Add(bytes.data(), bytes.size() - digest.size()) || !hash.Finish(&digest)) return false;
  return std::equal(digest.begin(), digest.end(), bytes.end() - digest.size());
}
std::wstring ChunkName(const std::wstring& slot, uint32_t chunk) {
  return slot + L"\\chunk-" + std::to_wstring(chunk) + L".bin";
}
bool SpaceAvailable(const std::wstring& root, uint64_t required,
                    ContentStageState* failure) {
  ULARGE_INTEGER available{};
  // This absolute path is already covered by Root's pinned no-reparse chain.
  // The caller-available value honors Windows disk quotas; it is not a lease.
  if (!GetDiskFreeSpaceExW(root.c_str(), &available, nullptr, nullptr)) {
    *failure = NativeFailure(GetLastError()); return false;
  }
  if (available.QuadPart < required) { *failure = ContentStageState::quota; return false; }
  return true;
}

constexpr uint8_t kDiscardMagic[] = {'R', 'Y', 'H', 'Z', 'D', 'P', '0', '1'};
struct DiscardEntry {
  fs::FileIdentity identity;
  uint64_t size = 0;
  ContentDigest hash{};
};
struct DiscardPlan {
  ContentStageRequest request;
  fs::FileIdentity root, source, slot;
  uint64_t verified_bytes = 0;
  uint32_t chunks = 0;
  std::vector<DiscardEntry> entries;  // Checkpoint first, followed by ordered chunks.
};
bool HashBytes(const std::vector<uint8_t>& bytes, size_t count, ContentDigest* digest) {
  if (count > bytes.size()) return false;
  Hash hash;
  for (size_t offset = 0; offset < count;) {
    const auto size = (std::min)(size_t{ContentStager::kChunkBytes}, count - offset);
    if (!hash.Add(bytes.data() + offset, size)) return false;
    offset += size;
  }
  return hash.Finish(digest);
}
void AppendDigest(std::vector<uint8_t>* bytes, const ContentDigest& hash) {
  bytes->insert(bytes->end(), hash.begin(), hash.end());
}
std::vector<uint8_t> EncodeDiscard(const DiscardPlan& plan) {
  std::vector<uint8_t> bytes(std::begin(kDiscardMagic), std::end(kDiscardMagic));
  const auto& request = plan.request;
  Append64(&bytes, request.slot);
  Append64(&bytes, request.expected_size);
  Append64(&bytes, request.minimum_verified_bytes);
  AppendDigest(&bytes, request.operation_id);
  AppendDigest(&bytes, request.manifest_sha256);
  AppendDigest(&bytes, request.expected_sha256);
  Append64(&bytes, request.source_relative.size());
  for (wchar_t ch : request.source_relative) {
    bytes.push_back(static_cast<uint8_t>(ch));
    bytes.push_back(static_cast<uint8_t>(ch >> 8));
  }
  AppendIdentity(&bytes, plan.root);
  AppendIdentity(&bytes, plan.source);
  AppendIdentity(&bytes, plan.slot);
  Append64(&bytes, plan.verified_bytes);
  Append64(&bytes, plan.chunks);
  for (const auto& entry : plan.entries) {
    AppendIdentity(&bytes, entry.identity);
    Append64(&bytes, entry.size);
    AppendDigest(&bytes, entry.hash);
  }
  ContentDigest hash{};
  if (bytes.size() + hash.size() > ContentStager::kMaximumDiscardPlanBytes ||
      !HashBytes(bytes, bytes.size(), &hash)) return {};
  AppendDigest(&bytes, hash);
  return bytes;
}
class PlanReader {
 public:
  explicit PlanReader(const std::vector<uint8_t>& bytes) : bytes_(bytes), end_(bytes.size() - 32) {}
  bool Number(uint64_t* value) {
    if (position_ > end_ || end_ - position_ < 8) return false;
    *value = 0;
    for (unsigned shift = 0; shift < 64; shift += 8) *value |= uint64_t{bytes_[position_++]} << shift;
    return true;
  }
  bool Bytes(uint8_t* out, size_t count) {
    if (position_ > end_ || count > end_ - position_) return false;
    std::copy_n(bytes_.data() + position_, count, out); position_ += count; return true;
  }
  bool IdentityValue(fs::FileIdentity* value) {
    return Number(&value->volume) && Bytes(value->id.data(), value->id.size());
  }
  bool End() const { return position_ == end_; }
 private:
  const std::vector<uint8_t>& bytes_;
  size_t position_ = sizeof(kDiscardMagic), end_;
};
bool DecodeDiscard(const std::vector<uint8_t>& bytes, DiscardPlan* plan) {
  if (bytes.size() < 32 + sizeof(kDiscardMagic) || bytes.size() > ContentStager::kMaximumDiscardPlanBytes ||
      !std::equal(std::begin(kDiscardMagic), std::end(kDiscardMagic), bytes.begin())) return false;
  ContentDigest checksum{};
  if (!HashBytes(bytes, bytes.size() - checksum.size(), &checksum) ||
      !std::equal(checksum.begin(), checksum.end(), bytes.end() - checksum.size())) return false;
  PlanReader reader(bytes);
  auto& request = plan->request;
  uint64_t slot = 0, source_length = 0, chunks = 0;
  if (!reader.Number(&slot) || slot >= ContentStager::kSlotCount ||
      !reader.Number(&request.expected_size) || !request.expected_size ||
      request.expected_size > ContentStager::kMaximumBytes ||
      !reader.Number(&request.minimum_verified_bytes) ||
      !reader.Bytes(request.operation_id.data(), 32) || !Nonzero(request.operation_id) ||
      !reader.Bytes(request.manifest_sha256.data(), 32) || !Nonzero(request.manifest_sha256) ||
      !reader.Bytes(request.expected_sha256.data(), 32) ||
      !reader.Number(&source_length) || !source_length || source_length > kMaximumSourceName) return false;
  request.slot = static_cast<uint32_t>(slot);
  request.source_relative.clear();
  for (uint64_t i = 0; i < source_length; ++i) {
    uint8_t pair[2]{};
    if (!reader.Bytes(pair, 2)) return false;
    request.source_relative.push_back(static_cast<wchar_t>(pair[0] | (uint16_t{pair[1]} << 8)));
  }
  if (!reader.IdentityValue(&plan->root) || !reader.IdentityValue(&plan->source) ||
      !reader.IdentityValue(&plan->slot) || plan->root.volume != plan->source.volume ||
      plan->root.volume != plan->slot.volume || !reader.Number(&plan->verified_bytes) ||
      !reader.Number(&chunks)) return false;
  const auto total_chunks = (request.expected_size + ContentStager::kChunkBytes - 1) / ContentStager::kChunkBytes;
  if (chunks > total_chunks || plan->verified_bytes != (chunks == total_chunks
      ? request.expected_size : chunks * ContentStager::kChunkBytes) ||
      request.minimum_verified_bytes > plan->verified_bytes ||
      (request.minimum_verified_bytes != request.expected_size &&
       request.minimum_verified_bytes % ContentStager::kChunkBytes != 0)) return false;
  plan->chunks = static_cast<uint32_t>(chunks);
  request.expected_root_identity = Identity(plan->root);
  request.expected_source_identity = Identity(plan->source);
  const auto checkpoint = Checkpoint(request, plan->source, plan->root);
  ContentDigest checkpoint_hash{};
  if (checkpoint.empty() || !HashBytes(checkpoint, checkpoint.size(), &checkpoint_hash)) return false;
  std::set<std::string> identities{Identity(plan->root), Identity(plan->source), Identity(plan->slot)};
  if (identities.size() != 3) return false;
  for (uint32_t i = 0; i <= plan->chunks; ++i) {
    DiscardEntry entry;
    if (!reader.IdentityValue(&entry.identity) || !reader.Number(&entry.size) ||
        !reader.Bytes(entry.hash.data(), 32) || entry.identity.volume != plan->root.volume ||
        !identities.insert(Identity(entry.identity)).second) return false;
    const uint64_t expected_size = i == 0 ? checkpoint.size() :
        (std::min)(uint64_t{ContentStager::kChunkBytes},
                   request.expected_size - uint64_t{i - 1} * ContentStager::kChunkBytes);
    if (entry.size != expected_size || (i == 0 && entry.hash != checkpoint_hash)) return false;
    plan->entries.push_back(entry);
  }
  return reader.End();
}
std::wstring DiscardName(size_t entry) {
  return entry == 0 ? L"checkpoint.bin" : L"chunk-" + std::to_wstring(entry - 1) + L".bin";
}
ContentDiscardState DiscardFailure(const fs::Error& error) {
  const auto state = FileFailure(error);
  if (state == ContentStageState::conflict) return ContentDiscardState::conflict;
  if (state == ContentStageState::unavailable) return ContentDiscardState::unavailable;
  if (state == ContentStageState::cancelled) return ContentDiscardState::cancelled;
  return ContentDiscardState::recovery;
}
bool KnownNames(const std::vector<std::wstring>& names, size_t entries) {
  std::set<std::wstring> allowed;
  for (size_t i = 0; i < entries; ++i) allowed.insert(DiscardName(i));
  for (const auto& name : names) if (!allowed.count(name)) return false;
  return true;
}
}  // namespace

ContentStager::ContentStager(std::wstring private_root) : private_root_(std::move(private_root)) {}
ContentStageResult ContentStager::Stage(const ContentStageRequest& request,
                                       const std::function<bool()>& cancelled) {
  return Run(request, true, cancelled);
}
ContentStageResult ContentStager::Inspect(const ContentStageRequest& request,
                                         const std::function<bool()>& cancelled) {
  return Run(request, false, cancelled);
}

ContentDiscardPlanResult ContentStager::PlanDiscard(
    const ContentStageRequest& request, const std::function<bool()>& cancelled) {
  ContentDiscardPlanResult result;
  auto finish = [&result](ContentStageState state) { result.state = state; return result; };
  if (!request.expected_size || request.expected_size > kMaximumBytes || request.slot >= kSlotCount ||
      !Nonzero(request.operation_id) || !Nonzero(request.manifest_sha256) ||
      request.source_relative.empty() || request.source_relative.size() > kMaximumSourceName ||
      !IdentityText(request.expected_root_identity) || !IdentityText(request.expected_source_identity) ||
      request.minimum_verified_bytes > request.expected_size ||
      (request.minimum_verified_bytes != request.expected_size && request.minimum_verified_bytes % kChunkBytes))
    return finish(ContentStageState::invalid);
  if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
  fs::Error error;
  auto root = fs::Root::Open(private_root_, false, &error);
  if (!root) return finish(FileFailure(error));
  auto staging = fs::Root::Open(private_root_ + L"\\content-staging", false, &error);
  if (!staging) return finish(FileFailure(error));
  auto source = root->OpenRead(L"incoming\\" + request.source_relative, kMaximumBytes, &error);
  if (!source) return finish(FileFailure(error));
  fs::FileInfo root_info;
  if (!root->Inspect(L"content-staging", &root_info, &error)) return finish(FileFailure(error));
  if (Identity(root_info.identity) != request.expected_root_identity ||
      Identity(source->info().identity) != request.expected_source_identity)
    return finish(ContentStageState::recovery);
  const auto source_state = VerifySource(&*source, request, cancelled);
  if (source_state != ContentStageState::staged) return finish(source_state);
  auto slot = staging->OpenDirectory(L"slot-" + std::to_wstring(request.slot), false, &error);
  if (!slot) return finish(FileFailure(error));
  std::vector<std::wstring> before;
  if (!slot->ListNames(4097, &before, &error)) return finish(FileFailure(error));
  const auto total_chunks = (request.expected_size + kChunkBytes - 1) / kChunkBytes;
  if (before.empty() || before.size() > total_chunks + 1 || !KnownNames(before, before.size()))
    return finish(ContentStageState::recovery);
  DiscardPlan plan;
  plan.request = request;
  plan.root = root_info.identity;
  plan.source = source->info().identity;
  plan.slot = slot->info().identity;
  plan.chunks = static_cast<uint32_t>(before.size() - 1);
  const auto checkpoint = Checkpoint(request, plan.source, plan.root);
  if (checkpoint.empty()) return finish(ContentStageState::unavailable);
  std::vector<fs::FileLease> pins;
  pins.reserve(before.size());
  std::vector<uint8_t> bytes, expected;
  for (size_t i = 0; i < before.size(); ++i) {
    if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
    const uint64_t size = i == 0 ? checkpoint.size() :
        (std::min)(uint64_t{kChunkBytes}, request.expected_size - uint64_t{i - 1} * kChunkBytes);
    auto file = slot->OpenRead(DiscardName(i), i == 0 ? kMaximumCheckpoint : kChunkBytes, &error);
    if (!file) return finish(FileFailure(error));
    DWORD native = 0;
    if (file->info().size != size) return finish(ContentStageState::recovery);
    if (!ReadAt(file->handle(), 0, static_cast<size_t>(size), &bytes, &native)) return finish(NativeFailure(native));
    if (i == 0) {
      if (bytes != checkpoint) return finish(ContentStageState::recovery);
    } else {
      if (!ReadAt(source->handle(), uint64_t{i - 1} * kChunkBytes, static_cast<size_t>(size), &expected, &native))
        return finish(NativeFailure(native));
      if (bytes != expected) return finish(ContentStageState::recovery);
      result.verified_bytes += size;
      ++result.verified_chunks;
    }
    DiscardEntry entry{file->info().identity, size, {}};
    if (!HashBytes(bytes, bytes.size(), &entry.hash)) return finish(ContentStageState::unavailable);
    plan.entries.push_back(entry);
    pins.push_back(std::move(*file));
  }
  std::vector<std::wstring> after;
  if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
  if (!slot->ListNames(4097, &after, &error)) return finish(FileFailure(error));
  if (after != before || result.verified_bytes < request.minimum_verified_bytes)
    return finish(ContentStageState::recovery);
  plan.verified_bytes = result.verified_bytes;
  result.plan = EncodeDiscard(plan);
  if (result.plan.empty()) return finish(ContentStageState::unavailable);
  return finish(result.verified_bytes == request.expected_size ? ContentStageState::staged : ContentStageState::checkpoint);
}

ContentDiscardResult ContentStager::Discard(const std::vector<uint8_t>& plan,
                                           const std::function<bool()>& cancelled) {
  return RunDiscard(plan, true, cancelled);
}
ContentDiscardResult ContentStager::InspectDiscard(const std::vector<uint8_t>& plan,
                                                  const std::function<bool()>& cancelled) {
  return RunDiscard(plan, false, cancelled);
}
ContentDiscardResult ContentStager::RunDiscard(const std::vector<uint8_t>& bytes, bool remove,
                                              const std::function<bool()>& cancelled) {
  ContentDiscardResult result;
  auto finish = [&result](ContentDiscardState state) { result.state = state; return result; };
  DiscardPlan plan;
  if (!DecodeDiscard(bytes, &plan)) return finish(ContentDiscardState::invalid);
  if (Cancelled(cancelled)) return finish(ContentDiscardState::cancelled);
  fs::Error error;
  auto root = fs::Root::Open(private_root_, false, &error);
  if (!root) return finish(DiscardFailure(error));
  auto staging = fs::Root::Open(private_root_ + L"\\content-staging", false, &error);
  if (!staging) return finish(DiscardFailure(error));
  fs::FileInfo root_info;
  if (!root->Inspect(L"content-staging", &root_info, &error)) return finish(DiscardFailure(error));
  if (root_info.identity != plan.root) return finish(ContentDiscardState::recovery);
  std::optional<fs::Lease> lock;
  if (remove) {
    lock = staging->Lock(L"stage.lease", &error);
    if (!lock) return finish(DiscardFailure(error));
  }
  auto slot = staging->OpenDirectory(L"slot-" + std::to_wstring(plan.request.slot), remove, &error);
  if (!slot) {
    if (error.code != fs::ErrorCode::kNotFound) return finish(DiscardFailure(error));
    result.removed_bytes = plan.verified_bytes;
    result.removed_files = static_cast<uint32_t>(plan.entries.size());
    return finish(ContentDiscardState::completed);
  }
  if (slot->info().identity != plan.slot) return finish(ContentDiscardState::recovery);
  std::vector<std::wstring> names;
  if (!slot->ListNames(4097, &names, &error)) return finish(DiscardFailure(error));
  if (!KnownNames(names, plan.entries.size())) return finish(ContentDiscardState::recovery);
  std::vector<std::optional<fs::FileRemovalLease>> removals(plan.entries.size());
  std::vector<fs::FileLease> reads;
  std::vector<std::wstring> present;
  std::vector<uint8_t> observed;
  for (size_t i = 0; i < plan.entries.size(); ++i) {
    if (Cancelled(cancelled)) return finish(ContentDiscardState::cancelled);
    const auto& expected = plan.entries[i];
    const auto name = DiscardName(i);
    HANDLE file = INVALID_HANDLE_VALUE;
    fs::FileInfo info;
    std::optional<fs::FileLease> read;
    if (remove) {
      removals[i] = slot->OpenForRemoval(name, expected.size, &error);
      if (removals[i]) { file = removals[i]->handle(); info = removals[i]->info(); }
    } else {
      read = slot->OpenRead(name, expected.size, &error);
      if (read) { file = read->handle(); info = read->info(); }
    }
    if (file == INVALID_HANDLE_VALUE) {
      if (error.code != fs::ErrorCode::kNotFound) return finish(DiscardFailure(error));
      ++result.removed_files;
      if (i != 0) result.removed_bytes += expected.size;
      continue;
    }
    if (info.identity != expected.identity || info.size != expected.size)
      return finish(ContentDiscardState::recovery);
    DWORD native = 0;
    ContentDigest hash{};
    if (!ReadAt(file, 0, static_cast<size_t>(expected.size), &observed, &native)) {
      error.win32 = native; return finish(DiscardFailure(error));
    }
    if (!HashBytes(observed, observed.size(), &hash)) return finish(ContentDiscardState::unavailable);
    if (hash != expected.hash) return finish(ContentDiscardState::recovery);
    present.push_back(name);
    if (read) reads.push_back(std::move(*read));
  }
  std::sort(present.begin(), present.end());
  if (Cancelled(cancelled)) return finish(ContentDiscardState::cancelled);
  if (!slot->ListNames(4097, &names, &error)) return finish(DiscardFailure(error));
  if (names != present) return finish(ContentDiscardState::recovery);
  if (!remove) return finish(ContentDiscardState::ready);
  // Every remaining file is verified and held on the same delete-capable handle.
  // Delete payload first and checkpoint last; each close is a resumable boundary.
  for (size_t step = 0; step < plan.entries.size(); ++step) {
    const size_t i = step + 1 < plan.entries.size() ? step + 1 : 0;
    if (Cancelled(cancelled)) return finish(ContentDiscardState::cancelled);
    if (!removals[i]) continue;
    if (!removals[i]->Remove(&error)) return finish(DiscardFailure(error));
    removals[i].reset();
    // A foreign delete-sharing handle may keep the name delete-pending after
    // our disposition handle closes. Count only confirmed namespace absence.
    auto remaining = slot->OpenRead(DiscardName(i), plan.entries[i].size, &error);
    if (remaining || error.code != fs::ErrorCode::kNotFound)
      return finish(ContentDiscardState::recovery);
    ++result.removed_files;
    if (i != 0) result.removed_bytes += plan.entries[i].size;
  }
  if (Cancelled(cancelled)) return finish(ContentDiscardState::cancelled);
  removals.clear();
  if (!slot->RemoveEmpty(&error)) return finish(DiscardFailure(error));
  auto remaining_slot = staging->OpenDirectory(
      L"slot-" + std::to_wstring(plan.request.slot), false, &error);
  if (remaining_slot || error.code != fs::ErrorCode::kNotFound)
    return finish(ContentDiscardState::recovery);
  return finish(ContentDiscardState::completed);
}
ContentStageResult ContentStager::Run(const ContentStageRequest& request, bool copy,
                                     const std::function<bool()>& cancelled) {
  ContentStageResult result;
  auto finish = [&result](ContentStageState state) { result.state = state; return result; };
  if (!request.expected_size || request.slot >= kSlotCount || !Nonzero(request.operation_id) ||
      !Nonzero(request.manifest_sha256) || request.source_relative.empty() ||
      request.source_relative.size() > kMaximumSourceName ||
      ((!request.expected_root_identity.empty() || copy) && !IdentityText(request.expected_root_identity)) ||
      ((!request.expected_source_identity.empty() || copy) && !IdentityText(request.expected_source_identity)))
    return finish(ContentStageState::invalid);
  if (request.expected_size > kMaximumBytes) return finish(ContentStageState::quota);
  if (request.minimum_verified_bytes > request.expected_size ||
      (request.minimum_verified_bytes != request.expected_size &&
       request.minimum_verified_bytes % kChunkBytes != 0))
    return finish(ContentStageState::invalid);
  if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);

  fs::Error error;
  auto root = fs::Root::Open(private_root_, false, &error);
  if (!root) return finish(error.code == fs::ErrorCode::kNotFound
      ? ContentStageState::unavailable : FileFailure(error));
  // Composition, not a request, owns provisioning both namespaces.
  auto staging = fs::Root::Open(private_root_ + L"\\content-staging", false, &error);
  if (!staging) return finish(error.code == fs::ErrorCode::kNotFound
      ? ContentStageState::unavailable : FileFailure(error));
  auto source = root->OpenRead(L"incoming\\" + request.source_relative, kMaximumBytes, &error);
  if (!source) return finish(FileFailure(error));
  fs::FileInfo root_info;
  if (!root->Inspect(L"content-staging", &root_info, &error)) return finish(FileFailure(error));
  result.root_identity = Identity(root_info.identity);
  result.source_identity = Identity(source->info().identity);
  if ((!request.expected_root_identity.empty() && request.expected_root_identity != result.root_identity) ||
      (!request.expected_source_identity.empty() && request.expected_source_identity != result.source_identity))
    return finish(ContentStageState::recovery);
  const auto source_state = VerifySource(&*source, request, cancelled);
  if (source_state != ContentStageState::staged) return finish(source_state);
  if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);

  std::optional<fs::Lease> lease;
  if (copy) {
    if (!SpaceAvailable(private_root_, kDiskReserveBytes + kMaximumCheckpoint, &result.state)) return result;
    lease = staging->Lock(L"stage.lease", &error);
    if (!lease) return finish(FileFailure(error));
  }
  const auto slot = L"slot-" + std::to_wstring(request.slot);
  const auto checkpoint_name = slot + L"\\checkpoint.bin";
  const auto expected = Checkpoint(request, source->info().identity, root_info.identity);
  if (expected.empty()) return finish(ContentStageState::unavailable);
  std::optional<fs::DirectoryLease> created_slot;
  auto checkpoint = staging->OpenRead(checkpoint_name, kMaximumCheckpoint, &error);
  if (!checkpoint) {
    if (error.code != fs::ErrorCode::kNotFound) return finish(FileFailure(error));
    fs::FileInfo observed;
    if (staging->Inspect(slot, &observed, &error) || error.code != fs::ErrorCode::kNotFound)
      return finish(ContentStageState::recovery);  // Unclaimed/partial directory: preserve it.
    if (!copy) return finish(ContentStageState::checkpoint);
    if (request.minimum_verified_bytes != 0) return finish(ContentStageState::recovery);
    if (!SpaceAvailable(private_root_, request.expected_size + kDiskReserveBytes +
                        kMaximumCheckpoint, &result.state)) return result;
    if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
    // Absence is an observation, not ownership. FILE_CREATE must win, and the
    // resulting exact directory remains pinned throughout checkpoint creation.
    created_slot = staging->CreateDirectoryExclusive(slot, &error);
    if (!created_slot || !staging->CreateExclusive(checkpoint_name, expected, &observed, &error))
      return finish(FileFailure(error));
    checkpoint = staging->OpenRead(checkpoint_name, kMaximumCheckpoint, &error);
    if (!checkpoint) return finish(FileFailure(error));
  }
  std::vector<uint8_t> recorded;
  DWORD failure = 0;
  if (!ReadAt(checkpoint->handle(), 0, static_cast<size_t>(checkpoint->info().size), &recorded, &failure))
    return finish(NativeFailure(failure));
  if (!ValidCheckpoint(recorded)) return finish(ContentStageState::recovery);
  if (recorded != expected) {
    // Matching request with changed physical identity needs recovery, not reuse.
    constexpr size_t identity_and_checksum = 48 + 32;
    const bool same_request = recorded.size() == expected.size() &&
        std::equal(recorded.begin(), recorded.end() - identity_and_checksum, expected.begin());
    return finish(same_request ? ContentStageState::recovery : ContentStageState::conflict);
  }

  const auto chunks = static_cast<uint32_t>((request.expected_size + kChunkBytes - 1) / kChunkBytes);
  std::vector<fs::FileLease> retained_pins;
  retained_pins.reserve(chunks);  // At most 4096 handles; no payload-sized allocation.
  std::vector<uint8_t> expected_chunk;
  std::vector<uint8_t> observed_chunk;
  uint32_t next = 0;
  auto verify_chunk = [&](fs::FileLease file, uint32_t index, bool source_loaded) {
    const uint64_t offset = uint64_t{index} * kChunkBytes;
    const auto size = static_cast<size_t>((std::min)(uint64_t{kChunkBytes}, request.expected_size - offset));
    if (file.info().size != size) { result.state = ContentStageState::recovery; return false; }
    // New chunks already have the exact copied bytes in expected_chunk. The
    // source remains pinned without write/delete sharing throughout this call.
    // Reuse that buffer while still rereading every destination after flush.
    if ((!source_loaded && !ReadAt(source->handle(), offset, size, &expected_chunk, &failure)) ||
        !ReadAt(file.handle(), 0, size, &observed_chunk, &failure)) {
      result.state = NativeFailure(failure); return false;
    }
    if (observed_chunk != expected_chunk) { result.state = ContentStageState::recovery; return false; }
    retained_pins.push_back(std::move(file));
    result.verified_bytes += size;
    ++result.verified_chunks;
    return true;
  };
  for (; next < chunks; ++next) {
    if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
    auto file = staging->OpenRead(ChunkName(slot, next), kChunkBytes, &error);
    if (!file) {
      if (error.code == fs::ErrorCode::kNotFound) break;
      return finish(FileFailure(error));
    }
    if (!verify_chunk(std::move(*file), next, false)) return result;
  }
  // A process interruption produces a contiguous durable prefix. A hole with a
  // later file is foreign/inconsistent state, never permission to fill the gap.
  for (uint32_t index = next + 1; index < chunks; ++index) {
    if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
    fs::FileInfo observed;
    if (staging->Inspect(ChunkName(slot, index), &observed, &error) ||
        error.code != fs::ErrorCode::kNotFound) return finish(ContentStageState::recovery);
  }
  // Verified prefix handles remain pinned through the effect. A suffix removed
  // since admission must not silently turn explicit resume into repair.
  if (copy && result.verified_bytes < request.minimum_verified_bytes)
    return finish(ContentStageState::recovery);
  if (next == chunks) return finish(ContentStageState::staged);
  if (!copy) return finish(ContentStageState::checkpoint);
  if (!SpaceAvailable(private_root_, request.expected_size - result.verified_bytes +
                      kDiskReserveBytes, &result.state)) return result;
  for (; next < chunks; ++next) {
    if (Cancelled(cancelled)) return finish(ContentStageState::cancelled);
    const uint64_t offset = uint64_t{next} * kChunkBytes;
    const auto size = static_cast<size_t>((std::min)(uint64_t{kChunkBytes}, request.expected_size - offset));
    if (!SpaceAvailable(private_root_, size + kDiskReserveBytes, &result.state)) return result;
    if (!ReadAt(source->handle(), offset, size, &expected_chunk, &failure))
      return finish(NativeFailure(failure));
    fs::FileInfo created;
    if (!staging->CreateExclusive(ChunkName(slot, next), expected_chunk, &created, &error))
      return finish(FileFailure(error));  // A failed partial chunk is retained.
    auto file = staging->OpenRead(ChunkName(slot, next), kChunkBytes, &error);
    if (!file) return finish(FileFailure(error));
    if (file->info().identity != created.identity) return finish(ContentStageState::recovery);
    if (!verify_chunk(std::move(*file), next, true)) return result;
  }
  return finish(ContentStageState::staged);
}
}  // namespace ryhze
