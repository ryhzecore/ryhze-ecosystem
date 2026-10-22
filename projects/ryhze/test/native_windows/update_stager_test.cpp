#include "update_stager.h"
#include "secure_files.h"

#include <windows.h>
#include <bcrypt.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using ryhze::UpdateDigest;
using ryhze::UpdateStager;
using ryhze::UpdateStageState;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  fs::path directory;
  Fixture() {
    wchar_t temp[MAX_PATH + 1]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "temporary fixture location");
    std::array<uint8_t, 16> bytes{};
    Check(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "fixture identity");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"ryhze-update-test-";
    for (uint8_t byte : bytes) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    directory = fs::path(temp) / name;
    Check(CreateDirectoryW(directory.c_str(), nullptr) != 0, "exclusive fixture directory");
  }
  ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
  fs::path Root(const wchar_t* name) const { return directory / name; }
};
UpdateDigest Digest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "fixture hash provider");
  UpdateDigest result{};
  const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), result.data(), static_cast<ULONG>(result.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  Check(status >= 0, "fixture digest");
  return result;
}
std::wstring Name(const UpdateDigest& digest) {
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring name;
  for (uint8_t byte : digest) { name += hex[byte >> 4]; name += hex[byte & 15]; }
  return name + L".stage";
}
std::vector<uint8_t> Read(const fs::path& path) {
  Check(fs::file_size(path) <= UpdateStager::kMaximumBytes, "bounded explicit fixture");
  std::ifstream stream(path, std::ios::binary);
  Check(stream.good(), "explicit fixture readable");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}
void Put(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(stream.good(), "synthetic fixture write");
}
std::wstring Environment(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size || size > 32768) return {};
  std::wstring value(size, L'\0');
  const DWORD read = GetEnvironmentVariableW(name, value.data(), size);
  if (!read || read >= size) return {};
  value.resize(read);
  return value;
}
UpdateDigest Pin(const std::wstring& text) {
  Check(text.size() == 64, "independently supplied SHA256 publisher pin required");
  auto nibble = [](wchar_t c) -> int {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
  };
  UpdateDigest pin{};
  for (size_t i = 0; i < pin.size(); ++i) {
    const int high = nibble(text[i * 2]), low = nibble(text[i * 2 + 1]);
    Check(high >= 0 && low >= 0, "publisher pin hexadecimal");
    pin[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return pin;
}
void SyntheticRejections() {
  Fixture fixture;
  const std::vector<uint8_t> unsigned_bytes = {'s','y','n','t','h','e','t','i','c'};
  const auto digest = Digest(unsigned_bytes);
  const UpdateDigest test_pin{};
  UpdateStager absent(fixture.Root(L"no-pin").wstring());
  Check(absent.Stage(unsigned_bytes, digest).state == UpdateStageState::unavailable, "missing publisher unavailable");
  Check(absent.Inspect(digest, unsigned_bytes.size()).state == UpdateStageState::unavailable, "missing publisher inspect unavailable");
  Check(!fs::exists(fixture.Root(L"no-pin")), "missing pin causes no writes");
  UpdateStager stager(fixture.Root(L"stages").wstring(), test_pin);
  auto wrong_hash = digest;
  wrong_hash[0] ^= 1;
  Check(stager.Stage(unsigned_bytes, wrong_hash).state == UpdateStageState::invalid, "wrong input hash");
  Check(!fs::exists(fixture.Root(L"stages")), "hash checked before writes");
  Check(stager.Stage({}, digest).state == UpdateStageState::invalid, "empty rejected");
  Check(stager.Stage(std::vector<uint8_t>(UpdateStager::kMaximumBytes + 1), digest).state == UpdateStageState::invalid, "oversize rejected");
  Check(stager.Stage(unsigned_bytes, digest).state == UpdateStageState::untrusted, "unsigned file not trusted");
  const auto staged_path = fixture.Root(L"stages") / Name(digest);
  Check(Read(staged_path) == unsigned_bytes, "unsigned evidence preserved");
  Check(stager.Inspect(digest, unsigned_bytes.size()).state == UpdateStageState::untrusted, "unsigned reverified");
  Check(stager.Inspect(digest, unsigned_bytes.size() + 1).state == UpdateStageState::recovery, "exact size checked");
  Put(staged_path, {1, 2});
  Check(stager.Stage(unsigned_bytes, digest).state == UpdateStageState::recovery, "partial cannot be overwritten");
  Check(Read(staged_path) == std::vector<uint8_t>({1, 2}), "partial bytes preserved");
  Check(stager.Inspect(digest, unsigned_bytes.size()).state == UpdateStageState::recovery, "partial inspect recovery");
  Check(stager.Inspect(digest, 0).state == UpdateStageState::invalid, "invalid inspect bound");
  Check(stager.Inspect(wrong_hash, unsigned_bytes.size()).state == UpdateStageState::unavailable, "missing stage unavailable");
  UpdateStager traversal((fixture.directory / L".." / L"escaped").wstring(), test_pin);
  Check(traversal.Stage(unsigned_bytes, digest).state == UpdateStageState::invalid, "root traversal rejected");
  ryhze::secure_files::Error error;
  auto root = ryhze::secure_files::Root::Open(fixture.Root(L"stages").wstring(), false, &error);
  Check(root.has_value(), "fixture root");
  auto lease = root->Lock(L"stage.lease", &error);
  Check(lease.has_value(), "exclusive stage lease");
  Check(stager.Stage(unsigned_bytes, digest).state == UpdateStageState::conflict, "stage serialized");
  Check(stager.Inspect(digest, unsigned_bytes.size()).state == UpdateStageState::conflict, "inspect serialized");
}
void SignedFixture(const fs::path& external, const UpdateDigest& trusted_pin) {
  // The external fixture is read only. Its publisher is trusted by this test
  // instance only; no app defaults or certificate-store entries are changed.
  const auto original = Read(external);
  Check(original.size() > 4096, "signed PE fixture bounds");
  const auto digest = Digest(original);
  Fixture fixture;
  UpdateStager stager(fixture.Root(L"stages").wstring(), trusted_pin);
  const auto first = stager.Stage(original, digest);
  Check(first.state == UpdateStageState::staged && first.bytes == original.size(),
        "signed fixture must pass cache-only chain revocation and independent publisher pin");
  Check(stager.Inspect(digest, original.size()).state == UpdateStageState::staged, "signed restart inspection");
  Check(stager.Stage(original, digest).state == UpdateStageState::staged, "existing stage reverified");
  auto wrong_pin = trusted_pin;
  wrong_pin[0] ^= 1;
  UpdateStager wrong(fixture.Root(L"stages").wstring(), wrong_pin);
  Check(wrong.Inspect(digest, original.size()).state == UpdateStageState::untrusted, "trusted chain insufficient without expected publisher");
  auto damaged = original;
  damaged[4096] ^= 0x80;
  const auto damaged_digest = Digest(damaged);
  Check(stager.Stage(damaged, damaged_digest).state == UpdateStageState::untrusted, "matching hash cannot legitimize tampered signature");
  const auto staged_path = fixture.Root(L"stages") / Name(digest);
  Put(staged_path, damaged);
  Check(stager.Inspect(digest, original.size()).state == UpdateStageState::recovery, "stored digest revalidated");
  Check(stager.Stage(original, digest).state == UpdateStageState::recovery, "unknown changed stage preserved");
  Check(Read(staged_path) == damaged, "no overwrite after tampering");
  Check(Read(external) == original, "external fixture unchanged");
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    SyntheticRejections();
    const auto path = argc == 3 ? std::wstring(argv[1]) : Environment(L"RYHZE_UPDATE_TEST_SIGNED_FILE");
    const auto pin = argc == 3 ? std::wstring(argv[2]) : Environment(L"RYHZE_UPDATE_TEST_PUBLISHER_SHA256");
    if (path.empty() || pin.empty()) {
      std::cerr << "Synthetic rejection checks passed; signed acceptance NOT RUN: supply explicit fixture and independent SHA256 publisher pin.\n";
      return 2;
    }
    SignedFixture(path, Pin(pin));
    std::cout << "Update stager: synthetic rejection and real signed-fixture suites passed; no execution performed.\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
