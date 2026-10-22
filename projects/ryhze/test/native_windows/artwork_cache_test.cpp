#include "artwork_cache.h"
#include "artwork_test_fixture.h"
#include <algorithm>
#include <iostream>

namespace {
using namespace artwork_test;
using namespace ryhze;
void RoundTrip() {
  Fixture fixture;
  ArtworkCache cache(fixture.parent);
  ArtworkError error{};
  ArtworkHealth health;
  Check(cache.Ready(&error) && cache.Health(&health, &error) && health.entries == 0 && health.bytes == 0, "ready empty cache");
  std::optional<ArtworkRecord> record;
  Check(cache.Get(Key(1), &record, &error) && !record, "missing artwork is cache miss");
  Check(!cache.Put("../escape", {1}, 1, &error) && error == ArtworkError::invalid, "path never accepted as key");
  Check(!cache.Put(std::string(64, 'A'), {1}, 1, &error), "uppercase keys refused");
  Check(!cache.Put(Key(1), {}, 1, &error), "empty artwork refused");
  Check(!cache.Put(Key(1), {1}, -1, &error), "negative timestamp refused");
  Check(!cache.Put(Key(1), {1}, ArtworkCache::kMaximumTimestamp + 1, &error), "timestamp bounded");
  Check(!cache.Put(Key(1), std::vector<uint8_t>(ArtworkCache::kMaximumEntry + 1), 1, &error), "entry bytes bounded");
  Check(cache.Put(Key(1), {1,2,3}, 123456, &error), "cache write");
  Check(cache.Get(Key(1), &record, &error) && record && record->bytes == std::vector<uint8_t>({1,2,3}) && record->stored_at_ms == 123456, "exact record read");
  Check(cache.Health(&health, &error) && health.entries == 1 && health.bytes == 123, "health includes encoded header");
  Check(cache.Put(Key(1), {4,5}, 222, &error), "validated replacement");
  ArtworkCache reopened(fixture.parent);
  Check(reopened.Get(Key(1), &record, &error) && record && record->bytes == std::vector<uint8_t>({4,5}) && record->stored_at_ms == 222, "restart reopens exact persisted bytes");
  secure_files::Error native;
  auto root = secure_files::Root::Open(fixture.parent, false, &native);
  auto lock = root->Lock(L"artwork.lease", &native);
  Check(lock.has_value(), "fixture process-wide cache lease");
  Check(!cache.Put(Key(2), {1}, 1, &error) && error == ArtworkError::conflict, "another owner cannot mutate locked cache");
  lock.reset();
  Check(!cache.Put(Key(2), {1}, 1, &error, [] { return true; }) && error == ArtworkError::cancelled, "pre-cancel has no effects");
  Check(cache.Get(Key(2), &record, &error) && !record, "cancelled write remains absent");
}
void Preserve() {
  Fixture fixture;
  ArtworkCache cache(fixture.parent);
  ArtworkError error{};
  Check(cache.Put(Key(1), {1,2}, 1, &error) && cache.Put(Key(2), {3,4}, 2, &error), "preservation initial writes");
  auto corrupt = Read(fixture.File(2)); corrupt.back() ^= 1;
  Write(fixture.File(2), corrupt, true);
  Write(fixture.parent + L"\\artwork\\owner-note.txt", {9,8,7});
  std::optional<ArtworkRecord> record;
  Check(!cache.Get(Key(2), &record, &error) && error == ArtworkError::integrity && !record, "payload checksum mismatch refused");
  Check(!cache.Put(Key(2), {5}, 3, &error) && error == ArtworkError::integrity && Read(fixture.File(2)) == corrupt, "damaged same key preserved");
  ArtworkHealth health;
  Check(cache.Health(&health, &error) && health.entries == 1 && health.preserved == 2, "unknown and corrupt are separately counted");
  ArtworkClear clear;
  Check(cache.Clear(&clear, &error) && clear.removed == 1 && clear.preserved == 2, "clear deletes validated entries only");
  Check(Read(fixture.File(2)) == corrupt && Read(fixture.parent + L"\\artwork\\owner-note.txt") == std::vector<uint8_t>({9,8,7}), "clear preserves exact unknown bytes");
  Check(cache.Put(Key(3), {5,6}, 3, &error), "header corruption fixture");
  auto header = Read(fixture.File(3)); header[40] ^= 1;
  Write(fixture.File(3), header, true);
  Check(!cache.Get(Key(3), &record, &error) && error == ArtworkError::integrity, "timestamp header checksum verified");
  Check(cache.Put(Key(4), {1}, 4, &error), "key binding fixture");
  Check(MoveFileW(fixture.File(4).c_str(), fixture.File(5).c_str()) != FALSE, "fixture renamed cache record");
  Check(!cache.Get(Key(5), &record, &error) && error == ArtworkError::integrity, "record cannot be relabelled to another key");
}
void UnsafeObjects() {
  Fixture fixture;
  ArtworkError error{};
  {
    ArtworkCache prepare(fixture.parent);
    Check(prepare.Put(Key(1), {1,2}, 1, &error), "hardlink original");
  }
  const auto outside = fixture.parent + L"\\outside-copy";
  Check(CreateHardLinkW(outside.c_str(), fixture.File(1).c_str(), nullptr) != FALSE, "fixture hardlink");
  ArtworkCache cache(fixture.parent);
  std::optional<ArtworkRecord> record;
  Check(!cache.Get(Key(1), &record, &error), "hardlinked artwork refused");
  Check(cache.Put(Key(2), {3,4}, 2, &error), "alternate stream original");
  Write(fixture.File(2) + L":owner-data", {8,9});
  Check(!cache.Get(Key(2), &record, &error), "alternate stream artwork refused");
  Check(CreateDirectoryW(fixture.File(3).c_str(), nullptr) != FALSE, "directory masquerade fixture");
  Check(!cache.Put(Key(3), {1}, 1, &error), "directory name never overwritten");
  int reparse_count = 0;
  if (CreateSymbolicLinkW(fixture.File(4).c_str(), outside.c_str(), 0x2)) {
    ++reparse_count;
    Check(!cache.Get(Key(4), &record, &error), "reparse file refused");
  } else {
    Check(GetLastError() == ERROR_PRIVILEGE_NOT_HELD || GetLastError() == ERROR_INVALID_PARAMETER, "known symlink environment limitation only");
    std::cout << "SKIP file reparse fixture requires permitted symbolic links\n";
  }
  ArtworkClear clear;
  Check(cache.Clear(&clear, &error) && clear.removed == 0 && clear.preserved == static_cast<size_t>(3 + reparse_count), "unsafe objects preserved by clear");
  Check(Read(outside).size() == 122 && Read(fixture.File(2) + L":owner-data") == std::vector<uint8_t>({8,9}), "outside hardlink and ADS unchanged");
  Check(!MoveFileW((fixture.parent + L"\\artwork").c_str(), (fixture.parent + L"\\old-artwork").c_str()) &&
      GetLastError() == ERROR_SHARING_VIOLATION, "retained ancestor pins prevent Win32 directory replacement");
}
void Bounds() {
  Fixture fixture;
  ArtworkCache cache(fixture.parent);
  ArtworkError error{};
  for (unsigned i = 1; i <= 64; ++i) Check(cache.Put(Key(i), {1}, 100, &error), "bounded entry insert");
  auto unknown_case = fixture.File(65);
  unknown_case.replace(unknown_case.size() - 5, 5, L".RART");
  Write(unknown_case, {8});
  Check(!cache.Put(Key(65), {1}, 100, &error) && error == ArtworkError::integrity,
      "case-insensitive unknown same name prevents eviction before creation");
  std::optional<ArtworkRecord> record;
  Check(cache.Get(Key(1), &record, &error) && record, "blocked same-name write does not evict unrelated entry");
  Check(cache.Put(Key(66), {1}, 100, &error), "bounded overflow insert");
  ArtworkHealth health;
  Check(cache.Health(&health, &error) && health.entries == 64 && health.bytes == 64 * 121, "entry limit includes new entry");
  Check(cache.Get(Key(1), &record, &error) && !record, "equal age eviction breaks ties by key");
  Check(cache.Get(Key(66), &record, &error) && record, "new entry retained");
  Fixture bytes_fixture;
  ArtworkCache bytes_cache(bytes_fixture.parent);
  const std::vector<uint8_t> payload(ArtworkCache::kMaximumEntry, 0x3a);
  for (unsigned i = 1; i <= 8; ++i) Check(bytes_cache.Put(Key(i), payload, i, &error), "byte quota insert");
  Check(bytes_cache.Health(&health, &error) && health.entries == 7 && health.bytes == 7 * (ArtworkCache::kMaximumEntry + 120), "encoded header participates in 64 MiB quota");
  Check(bytes_cache.Get(Key(1), &record, &error) && !record, "oldest bytes evicted");
  Fixture scan_fixture;
  ArtworkCache scan_cache(scan_fixture.parent);
  Check(scan_cache.Ready(&error), "scan fixture ready");
  for (unsigned i = 0; i < 129; ++i) Write(scan_fixture.parent + L"\\artwork\\unknown-" + std::to_wstring(i), {1});
  Check(!scan_cache.Health(&health, &error) && error == ArtworkError::quota, "enumeration bound refuses oversized directory");
  ArtworkClear clear;
  Check(!scan_cache.Clear(&clear, &error) && clear.removed == 0, "oversized scan produces no deletion");
}
void DirectoryCreation() {
  Fixture fixture;
  secure_files::Error error;
  auto root = secure_files::Root::Open(fixture.parent, false, &error);
  Check(root && root->EnsureDirectory(L"held", &error), "held creation fixture");
  auto held = root->OpenDirectory(L"held", true, &error);
  secure_files::FileInfo info;
  Check(held && held->CreateExclusive(L"entry", {1,2,3}, &info, &error) && info.size == 3, "exclusive creation through held removal directory");
  Check(!held->CreateExclusive(L"entry", {9}, &info, &error) && error.code == secure_files::ErrorCode::kAlreadyExists, "held creation never truncates");
  Check(!held->CreateExclusive(L"..\\escape", {9}, &info, &error), "held creation accepts one component only");
  Check(Read(fixture.parent + L"\\held\\entry") == std::vector<uint8_t>({1,2,3}), "rejected overwrite retained bytes");
}
}  // namespace
int main() {
  try {
    RoundTrip(); Preserve(); UnsafeObjects(); Bounds(); DirectoryCreation();
    std::cout << "PASS bounded artwork cache persistence, integrity, preservation, identity, locks and eviction\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << "\n"; return 1; }
}
