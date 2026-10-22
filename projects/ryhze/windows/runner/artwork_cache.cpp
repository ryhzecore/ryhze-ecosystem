#include "artwork_cache.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <utility>

namespace ryhze {
namespace {
namespace sf = secure_files;
constexpr std::array<uint8_t, 8> kMagic{'R','Y','A','R','T','0','0','1'};
bool Fail(ArtworkError* error, ArtworkError value) { if (error) *error = value; return false; }
bool Cancel(const ArtworkCache::Cancelled& cancel, ArtworkError* error) {
  return cancel && cancel() ? Fail(error, ArtworkError::cancelled) : true;
}
bool Native(const sf::Error& source, ArtworkError* error) {
  return Fail(error, source.code == sf::ErrorCode::kBusy ? ArtworkError::conflict :
      source.code == sf::ErrorCode::kTooLarge ? ArtworkError::quota : ArtworkError::io);
}
bool Hash(const uint8_t* data, size_t size, uint8_t* digest) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(data),
      static_cast<ULONG>(size), digest, 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  return status >= 0;
}
void Number(std::vector<uint8_t>* bytes, size_t offset, uint64_t value) {
  for (size_t i = 0; i < 8; ++i) (*bytes)[offset + i] = static_cast<uint8_t>(value >> (8 * i));
}
uint64_t Number(const std::vector<uint8_t>& bytes, size_t offset) {
  uint64_t result = 0;
  for (size_t i = 0; i < 8; ++i) result |= static_cast<uint64_t>(bytes[offset + i]) << (8 * i);
  return result;
}
std::array<uint8_t, 32> Key(const std::string& key) {
  std::array<uint8_t, 32> result{};
  const auto nibble = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  for (size_t i = 0; i < result.size(); ++i)
    result[i] = static_cast<uint8_t>((nibble(key[i * 2]) << 4) | nibble(key[i * 2 + 1]));
  return result;
}
std::wstring Name(const std::string& key) { return std::wstring(key.begin(), key.end()) + L".rart"; }
std::optional<std::string> FileKey(const std::wstring& name) {
  if (name.size() != 69 || name.substr(64) != L".rart") return std::nullopt;
  std::string key;
  for (size_t i = 0; i < 64; ++i) {
    if (name[i] > 127) return std::nullopt;
    key.push_back(static_cast<char>(name[i]));
  }
  return ArtworkCache::ValidKey(key) ? std::optional<std::string>(key) : std::nullopt;
}
bool Read(HANDLE file, uint64_t size, const std::string& key, ArtworkRecord* record,
          ArtworkError* error, const ArtworkCache::Cancelled& cancel, bool retain_payload = true) {
  if (size <= ArtworkCache::kHeaderBytes || size > ArtworkCache::kMaximumEntry + ArtworkCache::kHeaderBytes)
    return Fail(error, ArtworkError::integrity);
  std::vector<uint8_t> bytes(ArtworkCache::kHeaderBytes);
  size_t offset = 0;
  while (offset < bytes.size()) {
    if (!Cancel(cancel, error)) return false;
    DWORD count = 0;
    const auto wanted = static_cast<DWORD>((std::min)(size_t{65536}, bytes.size() - offset));
    if (!ReadFile(file, bytes.data() + offset, wanted, &count, nullptr) || !count)
      return Fail(error, ArtworkError::io);
    offset += count;
  }
  std::array<uint8_t, 32> digest{};
  const auto expected_key = Key(key);
  const auto timestamp = Number(bytes, 40);
  if (!std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) ||
      !std::equal(expected_key.begin(), expected_key.end(), bytes.begin() + 8) ||
      timestamp > static_cast<uint64_t>(ArtworkCache::kMaximumTimestamp) ||
      Number(bytes, 48) != size - ArtworkCache::kHeaderBytes ||
      !Hash(bytes.data(), 88, digest.data()) || !std::equal(digest.begin(), digest.end(), bytes.begin() + 88))
    return Fail(error, ArtworkError::integrity);
  // Scan/eviction verifies payloads with one 64 KiB buffer. Only a requested get
  // retains encoded artwork, avoiding per-entry 8 MiB allocations in health/clear.
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  BCRYPT_HASH_HANDLE hash = nullptr;
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    return Fail(error, ArtworkError::io);
  struct HashOwner {
    BCRYPT_ALG_HANDLE algorithm;
    BCRYPT_HASH_HANDLE* hash;
    ~HashOwner() { if (*hash) BCryptDestroyHash(*hash); BCryptCloseAlgorithmProvider(algorithm, 0); }
  } owner{algorithm, &hash};
  if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) return Fail(error, ArtworkError::io);
  const auto payload_size = static_cast<size_t>(size - ArtworkCache::kHeaderBytes);
  if (retain_payload) record->bytes.resize(payload_size);
  std::array<uint8_t, 65536> buffer{};
  offset = 0;
  while (offset < payload_size) {
    if (!Cancel(cancel, error)) return false;
    const auto wanted = static_cast<DWORD>((std::min)(buffer.size(), payload_size - offset));
    DWORD count = 0;
    if (!ReadFile(file, buffer.data(), wanted, &count, nullptr) || !count) return Fail(error, ArtworkError::io);
    if (BCryptHashData(hash, buffer.data(), count, 0) < 0) return Fail(error, ArtworkError::io);
    if (retain_payload) std::copy_n(buffer.data(), count, record->bytes.data() + offset);
    offset += count;
  }
  if (BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0)
    return Fail(error, ArtworkError::io);
  if (!std::equal(digest.begin(), digest.end(), bytes.begin() + 56)) return Fail(error, ArtworkError::integrity);
  record->stored_at_ms = static_cast<int64_t>(timestamp);
  return Cancel(cancel, error);
}
struct Entry { std::wstring name; std::string key; sf::FileInfo info; int64_t timestamp = 0; };
bool Scan(sf::DirectoryLease& directory, std::vector<Entry>* entries, size_t* preserved,
          ArtworkError* error, const ArtworkCache::Cancelled& cancel) {
  sf::Error native;
  std::vector<std::wstring> names;
  if (!directory.ListNames(ArtworkCache::kMaximumScan, &names, &native)) return Native(native, error);
  uint64_t inspected = 0;
  for (const auto& name : names) {
    if (!Cancel(cancel, error)) return false;
    auto key = FileKey(name);
    if (!key) { ++*preserved; continue; }
    auto file = directory.OpenRead(name, ArtworkCache::kMaximumEntry + ArtworkCache::kHeaderBytes, &native);
    if (!file) { ++*preserved; continue; }
    // A damaged or externally inflated cache is preserved, not scanned without
    // bound. Normal writes stay within 64 MiB; allow one extra entry for recovery.
    inspected += file->info().size;
    if (inspected > ArtworkCache::kMaximumBytes + ArtworkCache::kMaximumEntry + ArtworkCache::kHeaderBytes)
      return Fail(error, ArtworkError::quota);
    ArtworkRecord record;
    ArtworkError read_error = ArtworkError::none;
    if (!Read(file->handle(), file->info().size, *key, &record, &read_error, cancel, false)) {
      if (read_error == ArtworkError::cancelled) return Fail(error, read_error);
      ++*preserved; continue;
    }
    entries->push_back({name, *key, file->info(), record.stored_at_ms});
  }
  return true;
}
bool PinPlan(sf::DirectoryLease& directory, const std::vector<Entry>& entries,
             std::vector<sf::FileRemovalLease>* plan, ArtworkError* error,
             const ArtworkCache::Cancelled& cancel) {
  for (const auto& entry : entries) {
    if (!Cancel(cancel, error)) return false;
    sf::Error native;
    auto file = directory.OpenForRemoval(entry.name, ArtworkCache::kMaximumEntry + ArtworkCache::kHeaderBytes, &native);
    if (!file || file->info().identity != entry.info.identity || file->info().size != entry.info.size)
      return Fail(error, ArtworkError::conflict);
    ArtworkRecord record;
    if (!Read(file->handle(), file->info().size, entry.key, &record, error, cancel, false)) return false;
    plan->push_back(std::move(*file));
  }
  return true;
}
std::optional<std::wstring> PrivateParent() {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) return std::nullopt;
  const std::wstring base(value);
  CoTaskMemFree(value);
  sf::Error error;
  auto root = sf::Root::Open(base, false, &error);
  if (!root || !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private", &error)) return std::nullopt;
  return base + L"\\Ryhze-Entertainment\\native-private";
}
}  // namespace
ArtworkCache::ArtworkCache(std::optional<std::wstring> parent) : parent_(std::move(parent)) {}
bool ArtworkCache::ValidKey(const std::string& key) {
  return key.size() == 64 && std::all_of(key.begin(), key.end(), [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
  });
}
bool ArtworkCache::Ready(ArtworkError* error) {
  if (error) *error = ArtworkError::none;
  if (root_) return Open(false, error).has_value();
  if (!parent_) parent_ = PrivateParent();
  if (!parent_) return Fail(error, ArtworkError::unavailable);
  sf::Error native;
  auto root = sf::Root::Open(*parent_, false, &native);
  if (!root) return Fail(error, ArtworkError::unavailable);
  auto lock = root->Lock(L"artwork.lease", &native);
  if (!lock) return Native(native, error);
  if (!root->EnsureDirectory(L"artwork", &native)) return Native(native, error);
  auto directory = root->OpenDirectory(L"artwork", false, &native);
  if (!directory) return Native(native, error);
  identity_ = directory->info().identity;
  root_ = std::move(root);
  return true;
}
std::optional<sf::DirectoryLease> ArtworkCache::Open(bool removal, ArtworkError* error) {
  sf::Error native;
  auto directory = root_->OpenDirectory(L"artwork", removal, &native);
  if (!directory) { Native(native, error); return std::nullopt; }
  if (!identity_ || directory->info().identity != *identity_) {
    Fail(error, ArtworkError::conflict); return std::nullopt;
  }
  // Validate the directory's stream set as well as its identity. A bounded
  // enumeration also keeps reads/probes from silently accepting an inflated cache.
  std::vector<std::wstring> names;
  if (!directory->ListNames(kMaximumScan, &names, &native)) { Native(native, error); return std::nullopt; }
  return directory;
}
bool ArtworkCache::Get(const std::string& key, std::optional<ArtworkRecord>* record,
                       ArtworkError* error, const Cancelled& cancel) {
  if (record) record->reset();
  if (!record || !ValidKey(key)) return Fail(error, ArtworkError::invalid);
  if (!Cancel(cancel, error) || !Ready(error)) return false;
  sf::Error native;
  auto lock = root_->Lock(L"artwork.lease", &native);
  if (!lock) return Native(native, error);
  auto directory = Open(false, error);
  if (!directory) return false;
  auto file = directory->OpenRead(Name(key), kMaximumEntry + kHeaderBytes, &native);
  if (!file) return native.code == sf::ErrorCode::kNotFound ? true : Native(native, error);
  ArtworkRecord found;
  if (!Read(file->handle(), file->info().size, key, &found, error, cancel)) return false;
  *record = std::move(found);
  return true;
}
bool ArtworkCache::Health(ArtworkHealth* result, ArtworkError* error, const Cancelled& cancel) {
  if (!result) return Fail(error, ArtworkError::invalid);
  *result = {};
  if (!Cancel(cancel, error) || !Ready(error)) return false;
  sf::Error native;
  auto lock = root_->Lock(L"artwork.lease", &native);
  if (!lock) return Native(native, error);
  auto directory = Open(false, error);
  if (!directory) return false;
  std::vector<Entry> entries;
  if (!Scan(*directory, &entries, &result->preserved, error, cancel)) return false;
  result->entries = entries.size();
  for (const auto& entry : entries) result->bytes += entry.info.size;
  return true;
}
bool ArtworkCache::Clear(ArtworkClear* result, ArtworkError* error, const Cancelled& cancel) {
  if (!result) return Fail(error, ArtworkError::invalid);
  *result = {};
  if (!Cancel(cancel, error) || !Ready(error)) return false;
  sf::Error native;
  auto lock = root_->Lock(L"artwork.lease", &native);
  if (!lock) return Native(native, error);
  auto directory = Open(true, error);
  if (!directory) return false;
  std::vector<Entry> entries;
  if (!Scan(*directory, &entries, &result->preserved, error, cancel)) return false;
  std::vector<sf::FileRemovalLease> plan;
  if (!PinPlan(*directory, entries, &plan, error, cancel)) return false;
  for (auto& file : plan) {
    if (!Cancel(cancel, error)) return false;
    if (!file.Remove(&native)) return Native(native, error);
    ++result->removed;
  }
  return true;
}
bool ArtworkCache::Put(const std::string& key, const std::vector<uint8_t>& bytes,
                      int64_t timestamp, ArtworkError* error, const Cancelled& cancel) {
  if (!ValidKey(key) || bytes.empty() || bytes.size() > kMaximumEntry || timestamp < 0 ||
      timestamp > kMaximumTimestamp) return Fail(error, ArtworkError::invalid);
  if (!Cancel(cancel, error) || !Ready(error)) return false;
  std::vector<uint8_t> encoded(kHeaderBytes + bytes.size());
  std::copy(kMagic.begin(), kMagic.end(), encoded.begin());
  const auto binary_key = Key(key);
  std::copy(binary_key.begin(), binary_key.end(), encoded.begin() + 8);
  Number(&encoded, 40, static_cast<uint64_t>(timestamp));
  Number(&encoded, 48, bytes.size());
  if (!Hash(bytes.data(), bytes.size(), encoded.data() + 56) || !Hash(encoded.data(), 88, encoded.data() + 88))
    return Fail(error, ArtworkError::io);
  std::copy(bytes.begin(), bytes.end(), encoded.begin() + kHeaderBytes);
  sf::Error native;
  auto lock = root_->Lock(L"artwork.lease", &native);
  if (!lock) return Native(native, error);
  auto directory = Open(true, error);
  if (!directory) return false;
  std::vector<Entry> entries;
  size_t preserved = 0;
  if (!Scan(*directory, &entries, &preserved, error, cancel)) return false;
  // Unknown/corrupt same-key objects must prevent all eviction, not merely fail
  // exclusive creation after unrelated owned entries have already been removed.
  const auto existing = std::find_if(entries.begin(), entries.end(), [&](const Entry& entry) { return entry.key == key; });
  if (existing == entries.end()) {
    std::vector<std::wstring> names;
    if (!directory->ListNames(kMaximumScan, &names, &native)) return Native(native, error);
    const auto target = Name(key);
    if (std::any_of(names.begin(), names.end(), [&](const std::wstring& name) {
      return CompareStringOrdinal(name.data(), static_cast<int>(name.size()), target.data(),
          static_cast<int>(target.size()), TRUE) == CSTR_EQUAL;
    })) return Fail(error, ArtworkError::integrity);
    if (names.size() >= kMaximumScan) return Fail(error, ArtworkError::quota);
  }
  std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
    return a.timestamp == b.timestamp ? a.key < b.key : a.timestamp < b.timestamp;
  });
  uint64_t total = encoded.size();
  size_t count = 1;
  std::vector<Entry> victims;
  for (const auto& entry : entries) {
    if (entry.key == key) victims.push_back(entry);
    else { total += entry.info.size; ++count; }
  }
  for (const auto& entry : entries) {
    if (total <= kMaximumBytes && count <= kMaximumEntries) break;
    if (entry.key == key) continue;
    victims.push_back(entry); total -= entry.info.size; --count;
  }
  std::vector<sf::FileRemovalLease> plan;
  if (!PinPlan(*directory, victims, &plan, error, cancel)) return false;
  for (auto& file : plan) {
    if (!Cancel(cancel, error)) return false;
    if (!file.Remove(&native)) return Native(native, error);
  }
  if (!Cancel(cancel, error)) return false;
  sf::FileInfo created;
  return directory->CreateExclusive(Name(key), encoded, &created, &native) ? true : Native(native, error);
}
}  // namespace ryhze
