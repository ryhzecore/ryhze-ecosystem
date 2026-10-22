#include "content_stager.h"
#include "secure_files.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <locale>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using ryhze::ContentDigest;
using ryhze::ContentStageRequest;
using ryhze::ContentDiscardState;
using ryhze::ContentStageState;
using ryhze::ContentStager;
namespace sf = ryhze::secure_files;
int skipped = 0;
void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(std::string(message) + " Win32=" + std::to_string(GetLastError()));
}
std::wstring Join(const std::wstring& a, const std::wstring& b) { return a + L"\\" + b; }
bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }
void Directory(const std::wstring& path) { Check(CreateDirectoryW(path.c_str(), nullptr) != FALSE, "fixture directory"); }
void Put(const std::wstring& path, const std::vector<uint8_t>& bytes) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "new fixture file");
  DWORD count = 0;
  const bool okay = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) &&
      count == bytes.size() && FlushFileBuffers(file);
  CloseHandle(file);
  Check(okay, "fixture write");
}
void Corrupt(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "synthetic corruption handle");
  const uint8_t byte = 0xee;
  DWORD count = 0;
  const bool okay = WriteFile(file, &byte, 1, &count, nullptr) && count == 1 && FlushFileBuffers(file);
  CloseHandle(file);
  Check(okay, "synthetic corruption write");
}
std::vector<uint8_t> Read(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "fixture read handle");
  LARGE_INTEGER length{};
  Check(GetFileSizeEx(file, &length) && length.QuadPart >= 0 && length.QuadPart <= 32 * 1024 * 1024,
        "fixture read bound");
  std::vector<uint8_t> bytes(static_cast<size_t>(length.QuadPart));
  DWORD count = 0;
  const bool okay = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) && count == bytes.size();
  CloseHandle(file);
  Check(okay, "fixture read");
  return bytes;
}
ContentDigest Digest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "fixture SHA256");
  ContentDigest digest{};
  const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), digest.data(), static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  Check(status >= 0, "fixture digest");
  return digest;
}
std::vector<uint8_t> Payload(size_t size) {
  std::vector<uint8_t> result(size);
  for (size_t i = 0; i < size; ++i) result[i] = static_cast<uint8_t>((i * 31 + i / 257) % 251);
  return result;
}
struct Fixture {
  std::wstring base;
  std::wstring root;
  explicit Fixture(bool announce = true) {
    wchar_t temp[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    Check(length > 0 && length < std::size(temp), "OS temporary path");
    static std::atomic<unsigned> sequence{0};
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
      base = std::wstring(temp) + L"ryhze content " + std::to_wstring(GetCurrentProcessId()) + L"-" +
          std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(++sequence);
      if (CreateDirectoryW(base.c_str(), nullptr)) break;
      Check(GetLastError() == ERROR_ALREADY_EXISTS && attempt < 15, "exclusive fixture");
    }
    HANDLE directory = CreateFileW(base.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(directory != INVALID_HANDLE_VALUE, "fixture canonical handle");
    wchar_t canonical[32768]{};
    const DWORD count = GetFinalPathNameByHandleW(directory, canonical,
        static_cast<DWORD>(std::size(canonical)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(directory);
    Check(count > 4 && count < std::size(canonical), "fixture canonical path");
    base.assign(canonical, count);
    Check(base.rfind(L"\\\\?\\", 0) == 0, "fixture local namespace");
    base.erase(0, 4);
    root = Join(base, L"native-private");
    Directory(root);
    Directory(Join(root, L"incoming"));
    Directory(Join(root, L"content-staging"));
    if (announce) std::wcout << L"FIXTURE retained: " << base << L"\n";
  }
  std::wstring Source() const { return Join(root, L"incoming\\payload.bin"); }
  std::wstring Slot(uint32_t slot = 0) const { return Join(root, L"content-staging\\slot-" + std::to_wstring(slot)); }
  std::wstring Chunk(uint32_t index, uint32_t slot = 0) const {
    return Join(Slot(slot), L"chunk-" + std::to_wstring(index) + L".bin");
  }
  // Bounded successful and failed synthetic fixtures are preserved for review.
};
ContentStageRequest Request(const std::vector<uint8_t>& bytes) {
  ContentStageRequest request;
  request.operation_id = Digest({1, 2, 3});
  request.manifest_sha256 = Digest({4, 5, 6});
  request.source_relative = L"payload.bin";
  request.expected_size = bytes.size();
  request.expected_sha256 = Digest(bytes);
  return request;
}
ContentStageRequest Prepare(ContentStager* stager, ContentStageRequest request) {
  const auto result = stager->Inspect(request);
  Check(result.state == ContentStageState::checkpoint && result.verified_bytes == 0 && result.verified_chunks == 0,
        "initial read-only observation");
  Check(result.root_identity.size() == 49 && result.source_identity.size() == 49, "pinned identity grammar");
  request.expected_root_identity = result.root_identity;
  request.expected_source_identity = result.source_identity;
  return request;
}
void LargeRoundTripAndQuota() {
  Fixture fixture;
  const auto bytes = Payload(17 * 1024 * 1024 + 17);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(!Exists(fixture.Slot()) && !Exists(Join(fixture.root, L"content-staging\\stage.lease")),
        "preflight has no staging writes");
  auto unbound = request;
  unbound.expected_source_identity.clear();
  Check(stager.Stage(unbound).state == ContentStageState::invalid && !Exists(fixture.Slot()),
        "preflight identities mandatory before effects");
  auto oversized = request;
  oversized.expected_size = ContentStager::kMaximumBytes + 1;
  Check(stager.Stage(oversized).state == ContentStageState::quota && !Exists(fixture.Slot()), "one GiB bounded before effect");
  const auto complete = stager.Stage(request);
  Check(complete.state == ContentStageState::staged && complete.verified_bytes == bytes.size() &&
        complete.verified_chunks == 69, "payload larger than16MiB staged in bounded chunks");
  std::vector<uint8_t> assembled;
  for (uint32_t i = 0; i < complete.verified_chunks; ++i) {
    const auto chunk = Read(fixture.Chunk(i));
    assembled.insert(assembled.end(), chunk.begin(), chunk.end());
  }
  Check(assembled == bytes && Read(fixture.Source()) == bytes, "real copied bytes and unchanged source");
  Check(stager.Inspect(request).state == ContentStageState::staged, "restart inspection revalidates");
  Check(stager.Stage(request).verified_bytes == bytes.size(), "idempotent complete request");
  auto other = request;
  other.operation_id[0] ^= 1;
  Check(stager.Stage(other).state == ContentStageState::conflict, "occupied slot refuses foreign operation");
  other = request;
  other.manifest_sha256[0] ^= 1;
  Check(stager.Stage(other).state == ContentStageState::conflict, "manifest identity exact");
  other = request;
  other.slot = ContentStager::kSlotCount;
  Check(stager.Stage(other).state == ContentStageState::invalid, "finite eight-slot namespace");
}
void CancelResumeAndPartialRecovery() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 19);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(stager.Stage(request, [] { return true; }).state == ContentStageState::cancelled &&
        !Exists(fixture.Slot()), "early cancellation has no effects");
  auto cancelled = stager.Stage(request, [&] { return Exists(fixture.Chunk(0)); });
  Check(cancelled.state == ContentStageState::cancelled && cancelled.verified_chunks == 1 &&
        cancelled.verified_bytes == ContentStager::kChunkBytes, "cancellation retains flushed verified prefix");
  const auto prefix = Read(fixture.Chunk(0));
  ContentStager restarted(fixture.root);
  auto inspected = restarted.Inspect(request);
  Check(inspected.state == ContentStageState::checkpoint && inspected.verified_bytes == prefix.size(),
        "restart reconciles prefix, no automatic copy");
  Check(!Exists(fixture.Chunk(1)), "inspection never resumes");
  Check(restarted.Stage(request).state == ContentStageState::staged && Read(fixture.Chunk(0)) == prefix,
        "explicit resume preserves previous durable bytes");

  request.slot = 1;
  request.operation_id[1] ^= 1;
  cancelled = restarted.Stage(request, [&] { return Exists(fixture.Chunk(0, 1)); });
  Check(cancelled.state == ContentStageState::cancelled, "partial fixture prefix");
  Put(fixture.Chunk(1, 1), {9, 8, 7});  // Real interrupted-write shape, not a mocked filesystem.
  const auto partial = restarted.Stage(request);
  Check(partial.state == ContentStageState::recovery && partial.verified_chunks == 1 &&
        Read(fixture.Chunk(1, 1)) == std::vector<uint8_t>({9, 8, 7}) && !Exists(fixture.Chunk(2, 1)),
        "partial file preserved; no overwrite or progress fabrication");
}
void CorruptionForeignAndHoles() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 7);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Directory(fixture.Slot());
  Put(Join(fixture.Slot(), L"foreign.bin"), {88});
  Check(stager.Stage(request).state == ContentStageState::recovery &&
        !Exists(Join(fixture.Slot(), L"checkpoint.bin")) &&
        Read(Join(fixture.Slot(), L"foreign.bin")) == std::vector<uint8_t>({88}), "unclaimed directory preserved");
  request.slot = 1;
  Check(stager.Stage(request).state == ContentStageState::staged, "corruption fixture copied");
  Corrupt(fixture.Chunk(0, 1));
  const auto bad = stager.Inspect(request);
  Check(bad.state == ContentStageState::recovery && bad.verified_bytes == 0, "corrupted old progress not trusted");
  Check(stager.Stage(request).state == ContentStageState::recovery && Read(fixture.Chunk(0, 1))[0] == 0xee,
        "corrupt data not silently repaired");
  request.slot = 2;
  Check(stager.Stage(request, [&] { return Exists(fixture.Chunk(0, 2)); }).state == ContentStageState::cancelled,
        "hole fixture prefix");
  Put(fixture.Chunk(2, 2), std::vector<uint8_t>(bytes.end() - 7, bytes.end()));
  Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(fixture.Chunk(1, 2)),
        "later file after gap refuses fill");
  request.slot = 3;
  Check(stager.Stage(request).state == ContentStageState::staged, "metadata fixture complete");
  Corrupt(Join(fixture.Slot(3), L"checkpoint.bin"));
  Check(stager.Inspect(request).state == ContentStageState::recovery, "checkpoint checksum and schema validated");
}
void ResumePrefixFloor() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 3 + 19);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  for (uint64_t floor : {uint64_t{1}, uint64_t{ContentStager::kChunkBytes + 1},
                         request.expected_size + 1}) {
    request.minimum_verified_bytes = floor;
    Check(stager.Stage(request).state == ContentStageState::invalid &&
          stager.Inspect(request).state == ContentStageState::invalid &&
          !Exists(fixture.Slot()) && !Exists(Join(fixture.root, L"content-staging\\stage.lease")),
          "invalid prefix floor refused before filesystem effects");
  }
  request.minimum_verified_bytes = ContentStager::kChunkBytes;
  Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(fixture.Slot()),
        "nonzero floor cannot create an absent slot or checkpoint");
  request.minimum_verified_bytes = 0;
  const auto interrupted = stager.Stage(request, [&] { return Exists(fixture.Chunk(1)); });
  Check(interrupted.state == ContentStageState::cancelled && interrupted.verified_chunks == 2,
        "two retained chunks before resume inspection");
  const auto inspected = stager.Inspect(request);
  Check(inspected.state == ContentStageState::checkpoint && inspected.verified_chunks == 2,
        "explicit inspection admits two-chunk prefix");
  request.minimum_verified_bytes = inspected.verified_bytes;
  const auto checkpoint_path = Join(fixture.Slot(), L"checkpoint.bin");
  const auto checkpoint = Read(checkpoint_path);
  const auto first = Read(fixture.Chunk(0));
  const auto removed = Read(fixture.Chunk(1));
  const auto removed_path = Join(fixture.base, L"retained-removed-suffix.bin");
  Check(MoveFileW(fixture.Chunk(1).c_str(), removed_path.c_str()) != FALSE,
        "simulate suffix disappearing after inspection while preserving fixture bytes");
  const auto refused = stager.Stage(request);
  Check(refused.state == ContentStageState::recovery && refused.verified_chunks == 1 &&
        refused.verified_bytes == ContentStager::kChunkBytes &&
        !Exists(fixture.Chunk(1)) && !Exists(fixture.Chunk(2)) &&
        Read(fixture.Chunk(0)) == first && Read(removed_path) == removed &&
        Read(checkpoint_path) == checkpoint && Read(fixture.Source()) == bytes,
        "shorter prefix cannot silently recopy and every remaining byte is preserved");
  const auto shorter = stager.Inspect(request);
  Check(shorter.state == ContentStageState::checkpoint && shorter.verified_chunks == 1,
        "inspection truthfully reports regression without granting copy permission");
  Check(MoveFileW(removed_path.c_str(), fixture.Chunk(1).c_str()) != FALSE,
        "restore exact synthetic suffix for valid resume");
  Check(stager.Stage(request).state == ContentStageState::staged && Read(checkpoint_path) == checkpoint,
        "unchanged prefix resumes without altering immutable checkpoint identity");
  request.minimum_verified_bytes = request.expected_size;
  Check(stager.Stage(request).state == ContentStageState::staged,
        "exact final short chunk is a valid full-size floor");
  const auto tail = Read(fixture.Chunk(3));
  const auto tail_path = Join(fixture.base, L"retained-final-tail.bin");
  Check(MoveFileW(fixture.Chunk(3).c_str(), tail_path.c_str()) != FALSE,
        "retain removed final tail fixture");
  const auto lost_tail = stager.Stage(request);
  Check(lost_tail.state == ContentStageState::recovery && lost_tail.verified_chunks == 3 &&
        !Exists(fixture.Chunk(3)) && Read(tail_path) == tail && Read(checkpoint_path) == checkpoint,
        "complete floor refuses missing final tail instead of repairing it");
  const auto retained_slot = Join(fixture.base, L"retained-missing-slot");
  Check(MoveFileW(fixture.Slot().c_str(), retained_slot.c_str()) != FALSE,
        "retain vanished slot fixture");
  Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(fixture.Slot()) &&
        Read(Join(retained_slot, L"checkpoint.bin")) == checkpoint,
        "vanished admitted slot is not recreated");
}
void ExclusiveSlotRace() {
  Fixture fixture;
  const auto bytes = Payload(37);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  const auto request = Prepare(&stager, Request(bytes));
  bool inserted = false;
  const auto result = stager.Stage(request, [&] {
    // Stage has acquired its lease and observed the slot absent before this
    // cancellation boundary. Insert a real foreign directory in that gap.
    if (!inserted && Exists(Join(fixture.root, L"content-staging\\stage.lease")) && !Exists(fixture.Slot())) {
      Directory(fixture.Slot());
      Put(Join(fixture.Slot(), L"foreign-sentinel.bin"), {71, 72, 73});
      inserted = true;
    }
    return false;
  });
  Check(inserted && result.state == ContentStageState::conflict && result.verified_bytes == 0,
        "exclusive creation refuses foreign slot inserted after absence observation");
  Check(Read(Join(fixture.Slot(), L"foreign-sentinel.bin")) == std::vector<uint8_t>({71, 72, 73}) &&
        !Exists(Join(fixture.Slot(), L"checkpoint.bin")) && !Exists(fixture.Chunk(0)),
        "foreign race directory preserved without ownership adoption");

  sf::Error error;
  auto root = sf::Root::Open(Join(fixture.root, L"content-staging"), false, &error);
  Check(root.has_value(), "exclusive directory fixture root");
  auto pin = root->CreateDirectoryExclusive(L"pinned", &error);
  Check(pin.has_value(), "exclusive directory creation returns pin");
  Check(!root->CreateDirectoryExclusive(L"pinned", &error) && error.code == sf::ErrorCode::kAlreadyExists,
        "exclusive directory never reopens existing directory");
  const auto pinned = Join(fixture.root, L"content-staging\\pinned");
  root.reset();
  Check(!MoveFileW(pinned.c_str(), Join(fixture.root, L"content-staging\\moved").c_str()),
        "returned directory lease keeps ancestors and child pinned after Root destruction");
}
void IdentityReplacementAndPinning() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes + 1);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(MoveFileW(fixture.Source().c_str(), Join(fixture.root, L"incoming\\old.bin").c_str()) != FALSE, "replace fixture source identity");
  Put(fixture.Source(), bytes);
  Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(fixture.Slot()),
        "equal bytes cannot override preflight file identity");
  request = Prepare(&stager, Request(bytes));
  const auto staging = Join(fixture.root, L"content-staging");
  Check(MoveFileW(staging.c_str(), Join(fixture.root, L"old-staging").c_str()) != FALSE, "replace fixture root identity");
  Directory(staging);
  Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(fixture.Slot()),
        "preflight root replacement refused before effects");
  request = Prepare(&stager, Request(bytes));
  bool tried = false;
  auto result = stager.Stage(request, [&] {
    if (!tried && Exists(fixture.Chunk(0))) {
      tried = true;
      HANDLE writer = CreateFileW(fixture.Source().c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      const DWORD status = GetLastError();
      if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
      Check(writer == INVALID_HANDLE_VALUE && status == ERROR_SHARING_VIOLATION, "source pinned against write");
      Check(!MoveFileW(Join(fixture.root, L"incoming").c_str(), Join(fixture.root, L"moved-incoming").c_str()),
            "source parent pinned against rename");
    }
    return false;
  });
  Check(tried && result.state == ContentStageState::staged, "actual Windows pins held throughout copy");
  sf::Error error;
  auto root = sf::Root::Open(staging, false, &error);
  Check(root.has_value(), "lease fixture root");
  auto lease = root->Lock(L"stage.lease", &error);
  Check(lease.has_value() && stager.Stage(request).state == ContentStageState::conflict, "cooperating writers serialized");
}
bool Junction(const std::wstring& link, const std::wstring& target) {
  Directory(link);
  HANDLE file = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "junction fixture handle");
  struct Data {
    DWORD tag; WORD length; WORD reserved; WORD substitute_offset; WORD substitute_length;
    WORD print_offset; WORD print_length; wchar_t names[2048];
  } data{};
  const auto substitute = L"\\??\\" + target;
  Check(substitute.size() + target.size() + 2 < std::size(data.names), "fixture junction length");
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.substitute_length = static_cast<WORD>(substitute.size() * sizeof(wchar_t));
  data.print_offset = static_cast<WORD>((substitute.size() + 1) * sizeof(wchar_t));
  data.print_length = static_cast<WORD>(target.size() * sizeof(wchar_t));
  std::copy(substitute.begin(), substitute.end(), data.names);
  std::copy(target.begin(), target.end(), data.names + substitute.size() + 1);
  data.length = static_cast<WORD>(8 + data.print_offset + data.print_length + sizeof(wchar_t));
  DWORD returned = 0;
  const bool okay = DeviceIoControl(file, FSCTL_SET_REPARSE_POINT, &data,
      static_cast<DWORD>(8 + data.length), nullptr, 0, &returned, nullptr) != FALSE;
  const DWORD error = GetLastError();
  CloseHandle(file);
  if (!okay && (error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_NOT_SUPPORTED || error == ERROR_ACCESS_DENIED)) {
    ++skipped; std::cout << "SKIP junction creation Win32=" << error << '\n'; return false;
  }
  Check(okay, "junction fixture creation");
  return true;
}
void NamespaceAndLinks() {
  Fixture fixture;
  const auto bytes = Payload(37);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  for (const auto& name : {L"..\\payload.bin", L"C:\\payload.bin", L"payload.bin:stream", L"NUL", L"a/../b", L"a\\..\\b"}) {
    auto bad = request; bad.source_relative = name;
    Check(stager.Stage(bad).state == ContentStageState::invalid, "untrusted source grammar rejected");
  }
  const auto outside = Join(fixture.base, L"outside");
  Directory(outside);
  Put(Join(outside, L"sentinel.bin"), bytes);
  Check(CreateHardLinkW(Join(fixture.root, L"incoming\\hard.bin").c_str(), Join(outside, L"sentinel.bin").c_str(), nullptr) != FALSE,
        "source hardlink fixture");
  auto bad = request; bad.source_relative = L"hard.bin";
  Check(stager.Stage(bad).state == ContentStageState::recovery, "hardlinked source refused");
  if (Junction(Join(fixture.root, L"incoming\\linked"), outside)) {
    bad = request; bad.source_relative = L"linked\\sentinel.bin";
    Check(stager.Stage(bad).state == ContentStageState::recovery, "source junction refused");
  }
  if (Junction(fixture.Slot(), outside))
    Check(stager.Stage(request).state == ContentStageState::recovery && !Exists(Join(outside, L"checkpoint.bin")),
          "destination junction refuses escape");
  Check(Read(Join(outside, L"sentinel.bin")) == bytes, "outside synthetic sentinel unchanged");
}
int CrashChild(const std::wstring& root) {
  ContentStager stager(root);
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 3);
  auto request = Prepare(&stager, Request(bytes));
  stager.Stage(request, [&] {
    if (Exists(Join(root, L"content-staging\\slot-0\\chunk-0.bin"))) ExitProcess(77);
    return false;
  });
  return 4;
}
void RealProcessInterruption() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 3);
  Put(fixture.Source(), bytes);
  wchar_t executable[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  Check(length > 0 && length < std::size(executable), "test executable path");
  std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --crash-child \"" + fixture.root + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                       nullptr, fixture.base.c_str(), &startup, &process) != FALSE, "synthetic child starts");
  CloseHandle(process.hThread);
  const DWORD wait = WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = 0;
  const bool observed = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  CloseHandle(process.hProcess);
  Check(wait == WAIT_OBJECT_0 && observed && exit_code == 77, "actual child exit after durable chunk");
  ContentStager restarted(fixture.root);
  auto request = Request(bytes);
  const auto observation = restarted.Inspect(request);
  Check(observation.state == ContentStageState::checkpoint && observation.verified_chunks == 1,
        "actual process interruption leaves verifiable prefix");
  request.expected_root_identity = observation.root_identity;
  request.expected_source_identity = observation.source_identity;
  Check(restarted.Stage(request).state == ContentStageState::staged, "explicit retry after process interruption");
}

void PlannedDiscardAndResume() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 17);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(stager.PlanDiscard(request).plan.empty() && !Exists(fixture.Slot()), "absent slot has no discard plan");
  Check(stager.Stage(request, [&] { return Exists(fixture.Chunk(1)); }).state == ContentStageState::cancelled,
        "retained prefix discard fixture");
  request.minimum_verified_bytes = ContentStager::kChunkBytes * 2;
  const auto checkpoint = Read(Join(fixture.Slot(), L"checkpoint.bin"));
  const auto planned = stager.PlanDiscard(request);
  Check(planned.state == ContentStageState::checkpoint && planned.verified_chunks == 2 &&
        planned.verified_bytes == ContentStager::kChunkBytes * 2 && !planned.plan.empty() &&
        planned.plan.size() <= ContentStager::kMaximumDiscardPlanBytes, "bounded exact retained-prefix plan");
  Check(stager.PlanDiscard(request).plan == planned.plan &&
        Read(Join(fixture.Slot(), L"checkpoint.bin")) == checkpoint, "planning deterministic and read-only");
  Put(Join(fixture.base, L"persisted-discard.plan"), planned.plan);
  Check(stager.InspectDiscard(planned.plan).state == ContentDiscardState::ready && Exists(fixture.Chunk(0)),
        "read-only discard reconciliation never removes content");
  Check(stager.Discard(planned.plan, [] { return true; }).state == ContentDiscardState::cancelled &&
        Exists(fixture.Chunk(0)), "early discard cancellation has no removal");
  const auto partial = stager.Discard(planned.plan, [&] { return !Exists(fixture.Chunk(0)); });
  Check(partial.state == ContentDiscardState::cancelled && partial.removed_files == 1 &&
        partial.removed_bytes == ContentStager::kChunkBytes && Exists(fixture.Chunk(1)) &&
        Read(Join(fixture.Slot(), L"checkpoint.bin")) == checkpoint, "partial removal retains checkpoint and truthful counts");
  const auto inspected = stager.InspectDiscard(planned.plan);
  Check(inspected.state == ContentDiscardState::ready && inspected.removed_files == 1 &&
        inspected.removed_bytes == ContentStager::kChunkBytes, "restart recognizes missing planned file only");
  Check(MoveFileW(fixture.Source().c_str(), Join(fixture.base, L"retained-source.bin").c_str()) != FALSE,
        "discard no longer depends on incoming source availability");
  const auto complete = stager.Discard(Read(Join(fixture.base, L"persisted-discard.plan")));
  Check(complete.state == ContentDiscardState::completed && complete.removed_files == 3 &&
        complete.removed_bytes == planned.verified_bytes && !Exists(fixture.Slot()) &&
        Read(Join(fixture.base, L"retained-source.bin")) == bytes, "only planned prefix and empty slot removed");
  Check(stager.InspectDiscard(planned.plan).state == ContentDiscardState::completed &&
        stager.Discard(planned.plan).removed_files == 3, "durable discard reconciliation is idempotent");
  Directory(fixture.Slot());
  Put(Join(fixture.Slot(), L"unknown.bin"), {8});
  Check(stager.Discard(planned.plan).state == ContentDiscardState::recovery &&
        Read(Join(fixture.Slot(), L"unknown.bin")) == std::vector<uint8_t>({8}), "old plan never adopts a reused slot");
}

void DiscardRefusalsAndPins() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes + 7);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(stager.Stage(request).state == ContentStageState::staged, "discard refusal fixture");
  auto unbound = request; unbound.expected_source_identity.clear();
  Check(stager.PlanDiscard(unbound).state == ContentStageState::invalid, "discard planning requires prior physical binding");
  const auto planned = stager.PlanDiscard(request);
  Check(planned.state == ContentStageState::staged && !planned.plan.empty(), "complete discard plan");
  for (uint64_t floor : {uint64_t{1}, request.expected_size + 1}) {
    auto invalid_floor = request; invalid_floor.minimum_verified_bytes = floor;
    Check(stager.PlanDiscard(invalid_floor).state == ContentStageState::invalid,
          "discard planning rejects unaligned and excessive floors");
  }
  for (auto corrupted : {std::vector<uint8_t>{}, std::vector<uint8_t>(ContentStager::kMaximumDiscardPlanBytes + 1),
                         planned.plan}) {
    if (corrupted.size() == planned.plan.size()) corrupted.back() ^= 1;
    Check(stager.Discard(corrupted).state == ContentDiscardState::invalid && Exists(fixture.Chunk(0)),
          "malformed or damaged plans cannot remove anything");
  }
  auto invalid_slot = planned.plan;
  invalid_slot[8] = ContentStager::kSlotCount;
  const auto invalid_checksum = Digest(std::vector<uint8_t>(invalid_slot.begin(), invalid_slot.end() - 32));
  std::copy(invalid_checksum.begin(), invalid_checksum.end(), invalid_slot.end() - 32);
  Check(stager.Discard(invalid_slot).state == ContentDiscardState::invalid && Exists(fixture.Chunk(0)),
        "valid checksum does not bypass structural plan bounds");
  Put(Join(fixture.Slot(), L"unknown.bin"), {8, 9});
  Check(stager.PlanDiscard(request).plan.empty() &&
        stager.Discard(planned.plan).state == ContentDiscardState::recovery && Exists(fixture.Chunk(0)),
        "unknown names refuse planning and deletion before effects");
  Check(MoveFileW(Join(fixture.Slot(), L"unknown.bin").c_str(), Join(fixture.base, L"retained-unknown.bin").c_str()),
        "retain unknown fixture before continuing");
  const auto original = Read(fixture.Chunk(0));
  Corrupt(fixture.Chunk(0));
  Check(stager.Discard(planned.plan).state == ContentDiscardState::recovery && Exists(fixture.Chunk(1)) &&
        Read(fixture.Chunk(0))[0] == 0xee, "corrupt owned bytes refused before any deletion");
  Check(MoveFileW(fixture.Chunk(0).c_str(), Join(fixture.base, L"retained-corrupt.bin").c_str()), "retain corrupt fixture");
  Put(fixture.Chunk(0), original);
  Check(stager.Discard(planned.plan).state == ContentDiscardState::recovery && Exists(fixture.Chunk(1)),
        "equal-byte replacement identity is not owned by old plan");
  // Use a different owned slot to exercise simultaneous content and directory pins.
  request.slot = 1;
  Check(stager.Stage(request).state == ContentStageState::staged, "pinned discard fixture");
  const auto pin_plan = stager.PlanDiscard(request);
  bool probed = false;
  const auto pinned = stager.Discard(pin_plan.plan, [&] {
    if (!probed && Exists(fixture.Chunk(0, 1))) {
      HANDLE writer = CreateFileW(fixture.Chunk(0, 1).c_str(), GENERIC_WRITE,
          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
      if (writer != INVALID_HANDLE_VALUE) CloseHandle(writer);
      // Wait until every chunk has its delete/read pin, rather than the earlier
      // source/slot acquisition callbacks. A denied writer proves that boundary.
      if (writer == INVALID_HANDLE_VALUE) {
        probed = true;
        Check(!MoveFileW(fixture.Slot(1).c_str(), Join(fixture.base, L"substituted").c_str()),
              "slot remains pinned during removal verification");
      }
    }
    return false;
  });
  Check(probed && pinned.state == ContentDiscardState::completed && Read(fixture.Source()) == bytes,
        "pinned exact handles remove only destination content");
  request.slot = 2;
  Check(stager.Stage(request, [&] { return Exists(fixture.Chunk(0, 2)); }).state == ContentStageState::cancelled,
        "lost-prefix planning fixture");
  request.minimum_verified_bytes = ContentStager::kChunkBytes;
  Check(MoveFileW(fixture.Chunk(0, 2).c_str(), Join(fixture.base, L"retained-lost-prefix.bin").c_str()), "retain lost prefix");
  Check(stager.PlanDiscard(request).plan.empty() && Exists(Join(fixture.Slot(2), L"checkpoint.bin")),
        "discard planning cannot silently forget admitted prefix");
}

void DiscardPendingHandlesAndReplacement() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes + 3);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(stager.Stage(request).state == ContentStageState::staged, "pending deletion fixture");
  const auto planned = stager.PlanDiscard(request);
  HANDLE reader = CreateFileW(fixture.Chunk(0).c_str(), GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
  Check(reader != INVALID_HANDLE_VALUE, "delete-sharing external reader fixture");
  const auto pending = stager.Discard(planned.plan);
  CloseHandle(reader);
  Check(pending.state == ContentDiscardState::recovery && pending.removed_files == 0 &&
        pending.removed_bytes == 0 && Exists(fixture.Chunk(1)),
        "delete-pending name is not reported as confirmed removed");
  const auto reconciled = stager.InspectDiscard(planned.plan);
  Check(reconciled.state == ContentDiscardState::ready && reconciled.removed_files == 1 &&
        stager.Discard(planned.plan).state == ContentDiscardState::completed,
        "closed foreign handle permits fresh reconciliation and explicit completion");

  request.slot = 1;
  Check(stager.Stage(request).state == ContentStageState::staged, "pending directory fixture");
  const auto directory_plan = stager.PlanDiscard(request);
  HANDLE directory = CreateFileW(fixture.Slot(1).c_str(), FILE_LIST_DIRECTORY,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  Check(directory != INVALID_HANDLE_VALUE, "delete-sharing external directory fixture");
  const auto pending_directory = stager.Discard(directory_plan.plan);
  CloseHandle(directory);
  Check(pending_directory.state == ContentDiscardState::recovery && pending_directory.removed_files == 3 &&
        pending_directory.removed_bytes == bytes.size() &&
        stager.InspectDiscard(directory_plan.plan).state == ContentDiscardState::completed,
        "pending directory delays completion while confirmed file counts remain truthful");

  request.slot = 2;
  Check(stager.Stage(request).state == ContentStageState::staged, "hardlink discard fixture");
  const auto linked_plan = stager.PlanDiscard(request);
  const auto linked_copy = Join(fixture.base, L"retained-hardlink.bin");
  Check(CreateHardLinkW(linked_copy.c_str(), fixture.Chunk(0, 2).c_str(), nullptr), "late hardlink fixture");
  Check(stager.Discard(linked_plan.plan).state == ContentDiscardState::recovery &&
        Exists(fixture.Chunk(1, 2)) && Read(linked_copy) == Read(fixture.Chunk(0, 2)),
        "new hardlink refuses every removal and preserves both names");

  request.slot = 3;
  Check(stager.Stage(request).state == ContentStageState::staged, "slot junction replacement fixture");
  const auto replaced_plan = stager.PlanDiscard(request);
  const auto original_slot = Join(fixture.base, L"retained-original-slot");
  Check(MoveFileW(fixture.Slot(3).c_str(), original_slot.c_str()), "retain original slot fixture");
  const auto outside = Join(fixture.base, L"outside-discard");
  Directory(outside); Put(Join(outside, L"sentinel.bin"), {71});
  if (Junction(fixture.Slot(3), outside)) {
    Check(stager.Discard(replaced_plan.plan).state == ContentDiscardState::recovery &&
          Read(Join(outside, L"sentinel.bin")) == std::vector<uint8_t>({71}) &&
          Exists(Join(original_slot, L"chunk-0.bin")), "replaced junction slot never followed or removed");
  }

  request.slot = 4;
  Check(stager.Stage(request).state == ContentStageState::staged, "staging root replacement fixture");
  const auto root_plan = stager.PlanDiscard(request);
  const auto original_root = Join(fixture.base, L"retained-staging-root");
  Check(MoveFileW(Join(fixture.root, L"content-staging").c_str(), original_root.c_str()),
        "retain original staging namespace fixture");
  Directory(Join(fixture.root, L"content-staging"));
  Check(stager.Discard(root_plan.plan).state == ContentDiscardState::recovery &&
        Exists(Join(original_root, L"slot-4\\chunk-0.bin")) && Read(fixture.Source()) == bytes,
        "changed root is not mistaken for a completed discard");
}

void DiscardLateUnknownAndCrash() {
  Fixture fixture;
  const auto bytes = Payload(ContentStager::kChunkBytes * 2 + 3);
  Put(fixture.Source(), bytes);
  ContentStager stager(fixture.root);
  auto request = Prepare(&stager, Request(bytes));
  Check(stager.Stage(request).state == ContentStageState::staged, "late unknown fixture staged");
  const auto plan = stager.PlanDiscard(request);
  bool inserted = false;
  const auto interrupted = stager.Discard(plan.plan, [&] {
    if (!inserted && !Exists(fixture.Chunk(0))) {
      Put(Join(fixture.Slot(), L"late-unknown.bin"), {11, 12});
      inserted = true;
    }
    return false;
  });
  Check(inserted && interrupted.state == ContentDiscardState::recovery &&
        Read(Join(fixture.Slot(), L"late-unknown.bin")) == std::vector<uint8_t>({11, 12}) &&
        stager.InspectDiscard(plan.plan).state == ContentDiscardState::recovery,
        "late unknown file survives and exact empty slot removal refuses");
  request.slot = 1;
  Check(stager.Stage(request).state == ContentStageState::staged, "crash discard fixture staged");
  const auto crash_plan = stager.PlanDiscard(request);
  Put(Join(fixture.root, L"discard.plan"), crash_plan.plan);
  wchar_t executable[32768]{};
  const DWORD length = GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
  Check(length > 0 && length < std::size(executable), "discard test executable");
  std::wstring command = L"\"" + std::wstring(executable, length) + L"\" --discard-crash-child \"" + fixture.root + L"\"";
  STARTUPINFOW startup{}; startup.cb = sizeof(startup);
  PROCESS_INFORMATION process{};
  Check(CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
      nullptr, fixture.base.c_str(), &startup, &process), "synthetic discard child starts");
  CloseHandle(process.hThread);
  const DWORD waited = WaitForSingleObject(process.hProcess, 30000);
  DWORD exit_code = 0;
  const bool exited = GetExitCodeProcess(process.hProcess, &exit_code) != FALSE;
  CloseHandle(process.hProcess);
  Check(waited == WAIT_OBJECT_0 && exited && exit_code == 78, "actual process exit after first removal");
  const auto resumed = stager.InspectDiscard(crash_plan.plan);
  Check(resumed.state == ContentDiscardState::ready && resumed.removed_files == 1 &&
        resumed.removed_bytes == ContentStager::kChunkBytes, "durable plan reconciles real interrupted discard");
  Check(stager.Discard(crash_plan.plan).state == ContentDiscardState::completed &&
        Read(fixture.Source()) == bytes, "explicit discard resumes after process death without source deletion");
}
int DiscardCrashChild(const std::wstring& root) {
  ContentStager stager(root);
  stager.Discard(Read(Join(root, L"discard.plan")), [&] {
    if (!Exists(Join(root, L"content-staging\\slot-1\\chunk-0.bin"))) ExitProcess(78);
    return false;
  });
  return 5;
}

// Opt-in baseline only: each repetition uses a fresh private synthetic fixture.
// Setup and preflight intentionally warm the source; no cold-cache or GPU claim.
constexpr uint64_t kBenchmarkBytes = 32ull * 1024 * 1024;
ContentDigest BenchmarkSource(const std::wstring& path) {
  struct Resources {
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    ~Resources() {
      if (hash) BCryptDestroyHash(hash);
      if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
      if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    }
  } resources;
  resources.file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(resources.file != INVALID_HANDLE_VALUE, "benchmark exclusive source");
  Check(BCryptOpenAlgorithmProvider(&resources.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(resources.algorithm, &resources.hash, nullptr, 0, nullptr, 0, 0) >= 0,
        "benchmark streaming source digest");
  const auto buffer = Payload(ContentStager::kChunkBytes);
  for (uint64_t total = 0; total < kBenchmarkBytes; total += buffer.size()) {
    DWORD written = 0;
    Check(WriteFile(resources.file, buffer.data(), static_cast<DWORD>(buffer.size()), &written, nullptr) &&
          written == buffer.size(), "benchmark bounded source write");
    Check(BCryptHashData(resources.hash, const_cast<PUCHAR>(buffer.data()),
                        static_cast<ULONG>(buffer.size()), 0) >= 0, "benchmark source digest update");
  }
  ContentDigest digest{};
  Check(BCryptFinishHash(resources.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0 &&
        FlushFileBuffers(resources.file), "benchmark source digest and flush");
  return digest;
}
uint64_t Ticks(const FILETIME& value) {
  return (uint64_t{value.dwHighDateTime} << 32) | value.dwLowDateTime;
}
struct ProcessSnapshot {
  uint64_t kernel_100ns = 0;
  uint64_t user_100ns = 0;
  uint64_t working_set = 0;
  uint64_t lifetime_peak_working_set = 0;
  uint64_t private_bytes = 0;
  DWORD handles = 0;
  IO_COUNTERS io{};
};
ProcessSnapshot SnapshotProcess() {
  ProcessSnapshot result;
  FILETIME creation{}, exit{}, kernel{}, user{};
  const HANDLE process = GetCurrentProcess();
  PROCESS_MEMORY_COUNTERS_EX memory{};
  memory.cb = sizeof(memory);
  Check(GetProcessTimes(process, &creation, &exit, &kernel, &user) &&
        GetProcessIoCounters(process, &result.io) && GetProcessHandleCount(process, &result.handles) &&
        GetProcessMemoryInfo(process, reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory)),
        "benchmark process counters");
  result.kernel_100ns = Ticks(kernel);
  result.user_100ns = Ticks(user);
  result.working_set = memory.WorkingSetSize;
  result.lifetime_peak_working_set = memory.PeakWorkingSetSize;
  result.private_bytes = memory.PrivateUsage;
  return result;
}
struct Measurement {
  ProcessSnapshot before;
  ProcessSnapshot after;
  double elapsed_ms = 0;
  ryhze::ContentStageResult result;
};
Measurement Measure(ContentStager* stager, const ContentStageRequest& request, bool stage) {
  LARGE_INTEGER frequency{}, start{}, finish{};
  Check(QueryPerformanceFrequency(&frequency) && frequency.QuadPart > 0, "benchmark counter frequency");
  Measurement measurement;
  measurement.before = SnapshotProcess();
  Check(QueryPerformanceCounter(&start) != FALSE, "benchmark start counter");
  measurement.result = stage ? stager->Stage(request) : stager->Inspect(request);
  Check(QueryPerformanceCounter(&finish) != FALSE, "benchmark finish counter");
  measurement.after = SnapshotProcess();
  measurement.elapsed_ms = static_cast<double>(finish.QuadPart - start.QuadPart) * 1000.0 /
      static_cast<double>(frequency.QuadPart);
  Check(measurement.result.state == ContentStageState::staged &&
        measurement.result.verified_bytes == kBenchmarkBytes && measurement.result.verified_chunks == 128,
        "benchmark operation must verify complete payload");
  return measurement;
}
void JsonString(const std::wstring& value) {
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  Check(count > 0, "benchmark fixture UTF8 length");
  std::string utf8(static_cast<size_t>(count), '\0');
  Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
      utf8.data(), count, nullptr, nullptr) == count, "benchmark fixture UTF8");
  constexpr char hex[] = "0123456789abcdef";
  std::cout << '"';
  for (unsigned char ch : utf8) {
    if (ch == '\\' || ch == '"') std::cout << '\\' << static_cast<char>(ch);
    else if (ch < 32) std::cout << "\\u00" << hex[ch >> 4] << hex[ch & 15];
    else std::cout << static_cast<char>(ch);
  }
  std::cout << '"';
}
void PrintIo(const IO_COUNTERS& value) {
  std::cout << "{\"readOperations\":" << value.ReadOperationCount
      << ",\"writeOperations\":" << value.WriteOperationCount
      << ",\"otherOperations\":" << value.OtherOperationCount
      << ",\"readBytes\":" << value.ReadTransferCount
      << ",\"writeBytes\":" << value.WriteTransferCount
      << ",\"otherBytes\":" << value.OtherTransferCount << '}';
}
void PrintSnapshot(const ProcessSnapshot& value) {
  std::cout << "{\"kernel100ns\":" << value.kernel_100ns << ",\"user100ns\":" << value.user_100ns
      << ",\"workingSetBytes\":" << value.working_set
      << ",\"processLifetimePeakWorkingSetBytes\":" << value.lifetime_peak_working_set
      << ",\"privateBytes\":" << value.private_bytes << ",\"handleCount\":" << value.handles << ",\"io\":";
  PrintIo(value.io);
  std::cout << '}';
}
void PrintMeasurement(const Measurement& value) {
  const double kernel_ms = static_cast<double>(value.after.kernel_100ns - value.before.kernel_100ns) / 10000.0;
  const double user_ms = static_cast<double>(value.after.user_100ns - value.before.user_100ns) / 10000.0;
  IO_COUNTERS delta{};
  delta.ReadOperationCount = value.after.io.ReadOperationCount - value.before.io.ReadOperationCount;
  delta.WriteOperationCount = value.after.io.WriteOperationCount - value.before.io.WriteOperationCount;
  delta.OtherOperationCount = value.after.io.OtherOperationCount - value.before.io.OtherOperationCount;
  delta.ReadTransferCount = value.after.io.ReadTransferCount - value.before.io.ReadTransferCount;
  delta.WriteTransferCount = value.after.io.WriteTransferCount - value.before.io.WriteTransferCount;
  delta.OtherTransferCount = value.after.io.OtherTransferCount - value.before.io.OtherTransferCount;
  std::cout << "{\"elapsedMs\":" << value.elapsed_ms << ",\"kernelCpuMs\":" << kernel_ms
      << ",\"userCpuMs\":" << user_ms << ",\"totalCpuMs\":" << kernel_ms + user_ms
      << ",\"handleDelta\":" << static_cast<int64_t>(value.after.handles) - static_cast<int64_t>(value.before.handles)
      << ",\"ioDelta\":";
  PrintIo(delta);
  std::cout << ",\"before\":"; PrintSnapshot(value.before);
  std::cout << ",\"after\":"; PrintSnapshot(value.after);
  std::cout << ",\"result\":{\"state\":\"staged\",\"verifiedBytes\":" << value.result.verified_bytes
      << ",\"verifiedChunks\":" << value.result.verified_chunks << "}}";
}
int Benchmark() {
  struct Run { std::wstring fixture; Measurement stage; Measurement inspect; };
  std::vector<Run> runs;
  for (unsigned repetition = 0; repetition < 3; ++repetition) {
    Fixture fixture(false);
    const auto digest = BenchmarkSource(fixture.Source());
    ContentStageRequest request;
    request.operation_id = Digest({1, 2, 3});
    request.manifest_sha256 = Digest({4, 5, 6});
    request.source_relative = L"payload.bin";
    request.expected_size = kBenchmarkBytes;
    request.expected_sha256 = digest;
    ContentStager stager(fixture.root);
    request = Prepare(&stager, request);  // Source/manifest setup and hashing excluded.
    Run run;
    run.fixture = fixture.base;
    run.stage = Measure(&stager, request, true);
    run.inspect = Measure(&stager, request, false);
    runs.push_back(std::move(run));
  }
  std::cout.imbue(std::locale::classic());
  std::cout << std::fixed << std::setprecision(3)
      << "{\"schemaVersion\":1,\"kind\":\"native-content-staging-baseline\",\"payloadBytes\":" << kBenchmarkBytes
      << ",\"chunkBytes\":" << ContentStager::kChunkBytes
      << ",\"repetitions\":3,\"fixtureRetention\":true,\"setupExcluded\":true,"
      << "\"conditions\":\"Same process; fresh synthetic slot per run; preflight warms source; inspect follows stage; no cache flush\","
      << "\"measurementLimits\":\"Process counters; lifetime working-set peak; I/O counters do not establish physical disk traffic; no GPU work or improvement comparison\",\"runs\":[";
  for (size_t i = 0; i < runs.size(); ++i) {
    if (i) std::cout << ',';
    std::cout << "{\"repetition\":" << i + 1 << ",\"fixture\":"; JsonString(runs[i].fixture);
    std::cout << ",\"stage\":"; PrintMeasurement(runs[i].stage);
    std::cout << ",\"inspect\":"; PrintMeasurement(runs[i].inspect);
    std::cout << '}';
  }
  std::cout << "]}\n";
  return 0;
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc == 3 && std::wstring(argv[1]) == L"--crash-child") return CrashChild(argv[2]);
    if (argc == 3 && std::wstring(argv[1]) == L"--discard-crash-child") return DiscardCrashChild(argv[2]);
    if (argc == 2 && std::wstring(argv[1]) == L"--benchmark") return Benchmark();
    Check(argc == 1, "only benchmark or explicit synthetic child mode supported");
    LargeRoundTripAndQuota();
    CancelResumeAndPartialRecovery();
    CorruptionForeignAndHoles();
    ResumePrefixFloor();
    ExclusiveSlotRace();
    IdentityReplacementAndPinning();
    NamespaceAndLinks();
    RealProcessInterruption();
    PlannedDiscardAndResume();
    DiscardRefusalsAndPins();
    DiscardPendingHandlesAndReplacement();
    DiscardLateUnknownAndCrash();
    std::cout << "Content staging: twelve native suites passed; skipped=" << skipped
              << "; inert synthetic fixtures retained; no installation/execution of content.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
  }
}
