#include "update_stager.h"
#include "secure_files.h"

#include <windows.h>
#include <bcrypt.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <softpub.h>

#include <array>
#include <utility>

namespace ryhze {
namespace {
namespace fs = secure_files;
std::wstring Name(const UpdateDigest& hash) {
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring name;
  for (uint8_t byte : hash) { name += hex[byte >> 4]; name += hex[byte & 15]; }
  return name + L".stage";
}
struct Algorithm {
  BCRYPT_ALG_HANDLE handle = nullptr;
  ~Algorithm() { if (handle) BCryptCloseAlgorithmProvider(handle, 0); }
};
struct Hash {
  BCRYPT_HASH_HANDLE handle = nullptr;
  ~Hash() { if (handle) BCryptDestroyHash(handle); }
};
bool DigestBytes(const std::vector<uint8_t>& bytes, UpdateDigest* digest) {
  Algorithm algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  return BCryptHash(algorithm.handle, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), digest->data(), static_cast<ULONG>(digest->size())) >= 0;
}
bool DigestFile(HANDLE file, uint64_t expected_size, UpdateDigest* digest) {
  LARGE_INTEGER origin{};
  if (!SetFilePointerEx(file, origin, nullptr, FILE_BEGIN)) return false;
  Algorithm algorithm;
  if (BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
  Hash hash;
  if (BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0) return false;
  std::array<uint8_t, 65536> buffer{};
  uint64_t total = 0;
  for (;;) {
    DWORD read = 0;
    if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) return false;
    if (!read) break;
    total += read;
    if (total > expected_size || BCryptHashData(hash.handle, buffer.data(), read, 0) < 0) return false;
  }
  if (total != expected_size || BCryptFinishHash(hash.handle, digest->data(),
      static_cast<ULONG>(digest->size()), 0) < 0) return false;
  return SetFilePointerEx(file, origin, nullptr, FILE_BEGIN) != 0;
}
class TrustState {
 public:
  TrustState(HANDLE handle, const std::wstring& path) {
    file_.cbStruct = sizeof(file_);
    file_.pcwszFilePath = path.c_str();
    file_.hFile = handle;
    data_.cbStruct = sizeof(data_);
    data_.dwUIChoice = WTD_UI_NONE;
    data_.dwUnionChoice = WTD_CHOICE_FILE;
    data_.pFile = &file_;
    data_.dwStateAction = WTD_STATEACTION_VERIFY;
    // Revocation is explicitly required for the chain excluding its root. All
    // URL retrieval is cache-only; unavailable revocation evidence fails closed.
    data_.fdwRevocationChecks = WTD_REVOKE_NONE;
    data_.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT |
                        WTD_CACHE_ONLY_URL_RETRIEVAL | WTD_DISABLE_MD2_MD4;
  }
  ~TrustState() {
    data_.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(nullptr, &action_, &data_);
  }
  bool Verify(const UpdateDigest& expected_publisher) {
    // WinVerifyTrust returns LONG, not HRESULT: only exact zero means trusted.
    if (WinVerifyTrust(nullptr, &action_, &data_) != ERROR_SUCCESS) return false;
    auto* provider = WTHelperProvDataFromStateData(data_.hWVTStateData);
    if (!provider) return false;
    auto* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
    if (!signer || signer->dwError != ERROR_SUCCESS || !signer->csCertChain ||
        !signer->pasCertChain || !signer->pasCertChain[0].pCert) return false;
    UpdateDigest publisher{};
    DWORD size = static_cast<DWORD>(publisher.size());
    if (!CertGetCertificateContextProperty(signer->pasCertChain[0].pCert,
        CERT_SHA256_HASH_PROP_ID, publisher.data(), &size) || size != publisher.size()) return false;
    return publisher == expected_publisher;
  }
 private:
  GUID action_ = WINTRUST_ACTION_GENERIC_VERIFY_V2;
  WINTRUST_FILE_INFO file_{};
  WINTRUST_DATA data_{};
};
UpdateStageResult FileFailure(const fs::Error& error) {
  switch (error.code) {
    case fs::ErrorCode::kBusy:
    case fs::ErrorCode::kAlreadyExists: return {UpdateStageState::conflict, 0};
    case fs::ErrorCode::kInvalidPath: return {UpdateStageState::invalid, 0};
    case fs::ErrorCode::kReparsePoint:
    case fs::ErrorCode::kHardLink:
    case fs::ErrorCode::kIdentityMismatch:
    case fs::ErrorCode::kOutsideRoot:
    case fs::ErrorCode::kTooLarge: return {UpdateStageState::recovery, 0};
    default: return {UpdateStageState::unavailable, 0};
  }
}
UpdateStageResult Verify(fs::Root* root, const std::wstring& absolute_root,
                         const UpdateDigest& expected, uint64_t expected_size,
                         const UpdateDigest& publisher) {
  fs::Error error;
  const auto name = Name(expected);
  auto file = root->OpenRead(name, UpdateStager::kMaximumBytes, &error);
  if (!file) return FileFailure(error);
  if (file->info().size != expected_size) return {UpdateStageState::recovery, file->info().size};
  UpdateDigest actual{};
  if (!DigestFile(file->handle(), expected_size, &actual) || actual != expected)
    return {UpdateStageState::recovery, file->info().size};
  const std::wstring path = absolute_root + L"\\" + name;
  TrustState trust(file->handle(), path);
  if (!trust.Verify(publisher)) return {UpdateStageState::untrusted, file->info().size};
  return {UpdateStageState::staged, file->info().size};
}
}  // namespace
UpdateStager::UpdateStager(std::wstring private_root,
                           std::optional<UpdateDigest> publisher_der_sha256)
    : private_root_(std::move(private_root)),
      publisher_der_sha256_(publisher_der_sha256) {}

UpdateStageResult UpdateStager::Stage(const std::vector<uint8_t>& bytes,
                                      const UpdateDigest& expected_sha256) {
  if (!publisher_der_sha256_) return {UpdateStageState::unavailable, 0};
  if (bytes.empty() || bytes.size() > kMaximumBytes) return {UpdateStageState::invalid, 0};
  UpdateDigest actual{};
  if (!DigestBytes(bytes, &actual)) return {UpdateStageState::unavailable, 0};
  if (actual != expected_sha256) return {UpdateStageState::invalid, 0};
  fs::Error error;
  auto root = fs::Root::Open(private_root_, true, &error);
  if (!root) return FileFailure(error);
  auto lease = root->Lock(L"stage.lease", &error);
  if (!lease) return FileFailure(error);
  const auto name = Name(expected_sha256);
  fs::FileInfo existing;
  if (!root->Inspect(name, &existing, &error)) {
    if (error.code != fs::ErrorCode::kNotFound) return FileFailure(error);
    if (!root->CreateExclusive(name, bytes, &existing, &error)) {
      // A newly created partial file is intentionally preserved. Never retry
      // by truncating it or deleting a path that might now identify other data.
      if (error.code == fs::ErrorCode::kIo) return {UpdateStageState::recovery, 0};
      return FileFailure(error);
    }
  }
  return Verify(&*root, private_root_, expected_sha256, bytes.size(), *publisher_der_sha256_);
}
UpdateStageResult UpdateStager::Inspect(const UpdateDigest& expected_sha256,
                                        uint64_t expected_size) {
  if (!publisher_der_sha256_) return {UpdateStageState::unavailable, 0};
  if (!expected_size || expected_size > kMaximumBytes) return {UpdateStageState::invalid, 0};
  fs::Error error;
  auto root = fs::Root::Open(private_root_, false, &error);
  if (!root) return FileFailure(error);
  auto lease = root->Lock(L"stage.lease", &error);
  if (!lease) return FileFailure(error);
  return Verify(&*root, private_root_, expected_sha256, expected_size, *publisher_der_sha256_);
}
}  // namespace ryhze
