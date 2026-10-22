#include "host_security.h"
#include <windows.h>
#include <shlobj.h>
#include <flutter/standard_method_codec.h>
#include "secure_files.h"

namespace {
using flutter::EncodableValue;
using flutter::EncodableMap;
using Result = flutter::MethodResult<EncodableValue>;
const EncodableValue* Get(const EncodableMap& map, const char* name) {
  const auto it = map.find(EncodableValue(name));
  return it == map.end() ? nullptr : &it->second;
}
const std::string* String(const EncodableMap& map, const char* name) {
  const auto* value = Get(map, name);
  return value ? std::get_if<std::string>(value) : nullptr;
}
std::optional<std::wstring> Wide(const std::string& value) {
  if (value.empty() || value.size() > 32767) return std::nullopt;
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      value.data(), static_cast<int>(value.size()), nullptr, 0);
  if (!count) return std::nullopt;
  std::wstring result(count, L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), result.data(), count) != count) return std::nullopt;
  return result;
}
std::optional<std::wstring> PrivateRoot() {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                nullptr, &value))) return std::nullopt;
  const std::wstring base(value);
  CoTaskMemFree(value);
  ryhze::secure_files::Error error;
  auto root = ryhze::secure_files::Root::Open(base, false, &error);
  if (!root || !root->EnsureDirectory(L"Ryhze-Entertainment", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private\\credentials", &error)) {
    return std::nullopt;
  }
  return base + L"\\Ryhze-Entertainment\\native-private\\credentials";
}
void Reply(ryhze::CredentialStatus status, Result* result) {
  switch (status) {
    case ryhze::CredentialStatus::ok: result->Success(); return;
    case ryhze::CredentialStatus::missing: result->Success(); return;
    case ryhze::CredentialStatus::invalid_argument: result->Error("invalidData"); return;
    case ryhze::CredentialStatus::denied: result->Error("denied"); return;
    case ryhze::CredentialStatus::corrupt: result->Error("recovery"); return;
    case ryhze::CredentialStatus::conflict: result->Error("conflict"); return;
    case ryhze::CredentialStatus::unavailable: result->Error("unsupported"); return;
  }
}
}  // namespace

HostSecurity::HostSecurity(std::optional<std::wstring> private_root)
    : private_root_(std::move(private_root)) {}
HostSecurity::~HostSecurity() { Detach(); }
void HostSecurity::Attach(flutter::BinaryMessenger* messenger) {
  Detach();
  if (!private_root_) private_root_ = PrivateRoot();
  ryhze::secure_files::Error error;
  if (private_root_ && ryhze::secure_files::Root::Open(*private_root_, false, &error)) {
    vault_ = std::make_unique<ryhze::CredentialVault>(*private_root_);
  }
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      messenger, "com.ryhze.ryhze/windows.security.v1",
      &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    const auto* args = call.arguments();
    if (call.method_name() == "probe") {
      if (args && !args->IsNull()) { result->Error("invalidData"); return; }
      result->Success(EncodableValue(EncodableMap{
        {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
        {EncodableValue("secureCredentials"), EncodableValue(vault_ != nullptr)},
        {EncodableValue("fileIdentity"), EncodableValue(true)}}));
      return;
    }
    if (call.method_name() != "inspectFile" && call.method_name() != "readCredential" &&
        call.method_name() != "writeCredential" && call.method_name() != "deleteCredential") {
      result->NotImplemented(); return;
    }
    const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
    if (!map) { result->Error("invalidData"); return; }
    if (call.method_name() == "inspectFile") {
      const auto* root_text = String(*map, "root");
      const auto* relative_text = String(*map, "relative");
      if (map->size() != 2 || !root_text || !relative_text) {
        result->Error("invalidData"); return;
      }
      auto root_path = Wide(*root_text), relative = Wide(*relative_text);
      if (!root_path || !relative) { result->Error("invalidData"); return; }
      ryhze::secure_files::Error failure;
      auto root = ryhze::secure_files::Root::Open(*root_path, false, &failure);
      ryhze::secure_files::FileInfo info;
      if (!root || !root->Inspect(*relative, &info, &failure)) {
        result->Error("denied"); return;
      }
      // This is an observation, never a reusable launch/delete authority.
      result->Success(EncodableValue(EncodableMap{
        {EncodableValue("identity"), EncodableValue(std::vector<uint8_t>(info.identity.id.begin(), info.identity.id.end()))},
        {EncodableValue("volume"), EncodableValue(std::to_string(info.identity.volume))},
        {EncodableValue("directory"), EncodableValue(info.is_directory)},
        {EncodableValue("links"), EncodableValue(static_cast<int64_t>(info.link_count))}}));
      return;
    }
    const bool write = call.method_name() == "writeCredential";
    const bool read = call.method_name() == "readCredential";
    const bool remove = call.method_name() == "deleteCredential";
    if (!write && !read && !remove) { result->NotImplemented(); return; }
    const auto* provider = String(*map, "provider");
    const auto* account = String(*map, "account");
    if (map->size() != (write ? 3u : 2u) || !provider || !account) {
      result->Error("invalidData"); return;
    }
    const auto* data = Get(*map, "value");
    const auto* bytes = data ? std::get_if<std::vector<uint8_t>>(data) : nullptr;
    if (write && (!bytes || bytes->empty() || bytes->size() > ryhze::CredentialVault::kMaxSecretBytes)) {
      result->Error("invalidData"); return;
    }
    if (!vault_) { result->Error("unsupported"); return; }
    if (read) {
      auto value = vault_->Read(*provider, *account);
      if (value.status != ryhze::CredentialStatus::ok) {
        Reply(value.status, result.get()); return;
      }
      // StandardMethodCodec necessarily copies into its transport buffer. The
      // vault's plaintext buffer is scrubbed; Dart/engine memory is not secure RAM.
      EncodableValue payload(std::vector<uint8_t>(value.secret.data(),
          value.secret.data() + value.secret.size()));
      result->Success(payload);
      auto& plaintext = std::get<std::vector<uint8_t>>(payload);
      SecureZeroMemory(plaintext.data(), plaintext.size());
      return;
    }
    if (remove) { Reply(vault_->Delete(*provider, *account), result.get()); return; }
    Reply(vault_->Write(*provider, *account, bytes->data(), bytes->size()), result.get());
  });
}
void HostSecurity::Detach() {
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset();
  vault_.reset();
}
