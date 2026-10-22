#include "host_notifications.h"
#include "host_activation.h"
#include "secure_files.h"

#include <bcrypt.h>
#include <propkey.h>
#include <propvarutil.h>
#include <roapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <winrt/Windows.Data.Xml.Dom.h>
#include <winrt/Windows.UI.Notifications.h>
#include <flutter/standard_method_codec.h>
#include <algorithm>
#include <array>
#include <optional>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
using Microsoft::WRL::ComPtr;
constexpr wchar_t kAumid[] = L"Ryhze.Entertainment";
constexpr wchar_t kGroup[] = L"activity";
constexpr wchar_t kProductKey[] = L"Software\\Ryhze\\Entertainment";
constexpr wchar_t kProtocolKey[] = L"Software\\Classes\\ryhze";
constexpr wchar_t kStub[] = L"{dc65f38b-6b5d-49e9-ba7c-7d6ce82f2d7d}";

std::optional<std::wstring> Registry(const wchar_t* key, const wchar_t* name) {
  std::array<wchar_t, 32768> buffer{};
  DWORD size = static_cast<DWORD>(buffer.size() * sizeof(wchar_t));
  if (RegGetValueW(HKEY_CURRENT_USER, key, name,
      RRF_RT_REG_SZ | RRF_SUBKEY_WOW6432KEY, nullptr, buffer.data(), &size) != ERROR_SUCCESS ||
      size < sizeof(wchar_t) || size > buffer.size() * sizeof(wchar_t)) return std::nullopt;
  const size_t count = size / sizeof(wchar_t);
  if (size % sizeof(wchar_t) || buffer[count - 1] != L'\0' ||
      wcslen(buffer.data()) != count - 1) return std::nullopt;
  return std::wstring(buffer.data());
}
bool SamePath(const std::wstring& first, const std::wstring& second) {
  return CompareStringOrdinal(first.c_str(), -1, second.c_str(), -1, TRUE) == CSTR_EQUAL;
}
bool Guid(const std::wstring& text) {
  GUID value{};
  return text.size() == 38 && text.front() == L'{' && text.back() == L'}' &&
      SUCCEEDED(CLSIDFromString(text.c_str(), &value)) && value != GUID_NULL;
}
bool Property(IPropertyStore* store, REFPROPERTYKEY key, const wchar_t* expected) {
  PROPVARIANT value;
  PropVariantInit(&value);
  const bool match = SUCCEEDED(store->GetValue(key, &value)) &&
      value.vt == VT_LPWSTR && value.pwszVal && wcscmp(value.pwszVal, expected) == 0;
  PropVariantClear(&value);
  return match;
}

// Identity is rechecked for every operation. This is an installation/association
// gate, not a security boundary against arbitrary code running as the same user.
bool InstalledIdentity() {
  const auto root_path = Registry(kProductKey, L"InstallDir");
  const auto owner = Registry(kProductKey, L"RyhzeOwnerId");
  if (!root_path || !owner || !Guid(*owner)) return false;
  std::array<wchar_t, 32768> module{};
  const DWORD count = GetModuleFileNameW(nullptr, module.data(), static_cast<DWORD>(module.size()));
  if (!count || count >= module.size()) return false;
  const auto executable = *root_path + L"\\app\\ryhze.exe";
  if (!SamePath(module.data(), executable)) return false;
  const auto protocol_owner = Registry(kProtocolKey, L"RyhzeOwnerId");
  const auto protocol_root = Registry(kProtocolKey, L"RyhzeInstallRoot");
  const auto protocol_flag = Registry(kProtocolKey, L"URL Protocol");
  const auto command = Registry(L"Software\\Classes\\ryhze\\shell\\open\\command", nullptr);
  if (!protocol_owner || *protocol_owner != *owner || !protocol_root ||
      !SamePath(*protocol_root, *root_path) || !protocol_flag || !protocol_flag->empty() ||
      !command || *command != L"\"" + executable + L"\" \"%1\"") return false;

  ryhze::secure_files::Error error;
  auto root = ryhze::secure_files::Root::Open(*root_path, false, &error);
  if (!root) return false;
  auto image = root->OpenRead(L"app\\ryhze.exe", 1024ull * 1024 * 1024, &error);
  auto marker = root->OpenRead(L".ryhze-install.ini", 16384, &error);
  if (!image || !marker) return false;
  const auto marker_path = *root_path + L"\\.ryhze-install.ini";
  auto ini = [&marker_path](const wchar_t* field) {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetPrivateProfileStringW(L"Install", field, L"", buffer.data(),
        static_cast<DWORD>(buffer.size()), marker_path.c_str());
    return length < buffer.size() - 1 ? std::wstring(buffer.data()) : std::wstring();
  };
  if (!SamePath(ini(L"Root"), *root_path) || ini(L"Product") != L"RyhzeEntertainment" ||
      ini(L"OwnerId") != *owner) return false;

  PWSTR programs = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, KF_FLAG_DEFAULT, nullptr, &programs))) return false;
  const std::wstring programs_path(programs);
  CoTaskMemFree(programs);
  auto menu = ryhze::secure_files::Root::Open(programs_path, false, &error);
  if (!menu) return false;
  auto shortcut = menu->OpenRead(L"Ryhze Entertainment.lnk", 1024 * 1024, &error);
  if (!shortcut) return false;
  ComPtr<IShellLinkW> link;
  ComPtr<IPersistFile> file;
  ComPtr<IPropertyStore> properties;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) ||
      FAILED(link.As(&file)) || FAILED(link.As(&properties)) ||
      FAILED(file->Load((programs_path + L"\\Ryhze Entertainment.lnk").c_str(), STGM_READ))) return false;
  std::array<wchar_t, 32768> target{}, args{}, description{};
  if (FAILED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)) ||
      !SamePath(target.data(), executable) ||
      FAILED(link->GetArguments(args.data(), static_cast<int>(args.size()))) || args[0] ||
      FAILED(link->GetDescription(description.data(), static_cast<int>(description.size()))) ||
      std::wstring(description.data()) != L"RyhzeEntertainment|" + *owner ||
      !Property(properties.Get(), PKEY_AppUserModel_ID, kAumid)) return false;
  GUID expected{};
  if (FAILED(CLSIDFromString(kStub, &expected))) return false;
  PROPVARIANT clsid;
  PropVariantInit(&clsid);
  const bool matches = SUCCEEDED(properties->GetValue(PKEY_AppUserModel_ToastActivatorCLSID, &clsid)) &&
      clsid.vt == VT_CLSID && clsid.puuid && *clsid.puuid == expected;
  PropVariantClear(&clsid);
  return matches;
}

class WindowsNotifications final : public NotificationBackend {
 public:
  WindowsNotifications() : initialized_(SUCCEEDED(RoInitialize(RO_INIT_SINGLETHREADED))) {}
  ~WindowsNotifications() override { if (initialized_) RoUninitialize(); }
  bool Available() override {
    if (!initialized_ || !InstalledIdentity()) return false;
    try {
      return winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(kAumid)
          .Setting() == winrt::Windows::UI::Notifications::NotificationSetting::Enabled;
    } catch (...) { return false; }
  }
  bool Submit(const std::wstring& tag, const std::wstring& xml) override {
    if (!Available()) return false;
    try {
      winrt::Windows::Data::Xml::Dom::XmlDocument document;
      document.LoadXml(xml);
      winrt::Windows::UI::Notifications::ToastNotification toast(document);
      toast.Tag(tag);
      toast.Group(kGroup);
      winrt::Windows::UI::Notifications::ToastNotificationManager::CreateToastNotifier(kAumid).Show(toast);
      return true;
    } catch (...) { return false; }
  }
  bool Clear() override {
    if (!initialized_ || !InstalledIdentity()) return false;
    try {
      winrt::Windows::UI::Notifications::ToastNotificationManager::History().RemoveGroup(kGroup, kAumid);
      return true;
    } catch (...) { return false; }
  }
 private:
  bool initialized_;
};

const char* Summary(const std::string& kind) {
  if (kind == "completed") return "Operation completed.";
  if (kind == "handoff") return "Provider handoff accepted. Running state is not verified.";
  if (kind == "recovery") return "An interrupted operation needs recovery.";
  if (kind == "failed") return "An operation failed. Review its current status.";
  if (kind == "blocked") return "An operation is unavailable on this device.";
  return nullptr;
}
std::wstring Escape(const std::string& text) {
  // Validated routes and closed summaries are ASCII. Escape XML attributes/text.
  std::wstring output;
  for (const unsigned char value : text) {
    switch (value) {
      case '&': output += L"&amp;"; break;
      case '<': output += L"&lt;"; break;
      case '>': output += L"&gt;"; break;
      case '"': output += L"&quot;"; break;
      case '\'': output += L"&apos;"; break;
      default: output += static_cast<wchar_t>(value); break;
    }
  }
  return output;
}
const EncodableValue* Get(const EncodableMap& map, const char* key) {
  const auto it = map.find(EncodableValue(key));
  return it == map.end() ? nullptr : &it->second;
}
const std::string* String(const EncodableMap& map, const char* key) {
  const auto* value = Get(map, key);
  return value ? std::get_if<std::string>(value) : nullptr;
}
}  // namespace

bool HostNotifications::Validate(const NotificationRequest& request) {
  return !request.id.empty() && request.id.size() <= 8192 &&
      std::none_of(request.id.begin(), request.id.end(), [](unsigned char c) { return c < 32 || c == 127; }) &&
      MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, request.id.data(),
          static_cast<int>(request.id.size()), nullptr, 0) > 0 && Summary(request.kind) &&
      HostActivation::CanonicalizeUri(request.uri).has_value();
}
std::wstring HostNotifications::ToastXml(const NotificationRequest& request) {
  if (!Validate(request)) return {};
  const auto uri = HostActivation::CanonicalizeUri(request.uri);
  return L"<toast activationType=\"protocol\" launch=\"" + Escape(*uri) +
      L"\"><visual><binding template=\"ToastGeneric\"><text>Ryhze</text><text>" +
      Escape(request.private_preview ? "Ryhze activity is available." : Summary(request.kind)) +
      L"</text></binding></visual><audio silent=\"true\"/></toast>";
}
std::wstring HostNotifications::Tag(const std::string& id) {
  if (id.empty() || id.size() > 8192) return {};
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  std::array<unsigned char, 32> digest{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
  const NTSTATUS status = BCryptHash(algorithm, nullptr, 0,
      reinterpret_cast<PUCHAR>(const_cast<char*>(id.data())), static_cast<ULONG>(id.size()),
      digest.data(), static_cast<ULONG>(digest.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (status < 0) return {};
  // 96 bits encoded as 16 URL-safe ASCII characters, including old Windows tag limits.
  constexpr wchar_t alphabet[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  std::wstring tag;
  for (size_t i = 0; i < 12; i += 3) {
    const unsigned value = (digest[i] << 16) | (digest[i + 1] << 8) | digest[i + 2];
    tag += alphabet[(value >> 18) & 63]; tag += alphabet[(value >> 12) & 63];
    tag += alphabet[(value >> 6) & 63]; tag += alphabet[value & 63];
  }
  return tag;
}

HostNotifications::HostNotifications(std::unique_ptr<NotificationBackend> backend,
                                   std::function<ULONGLONG()> clock)
    : backend_(backend ? std::move(backend) : std::make_unique<WindowsNotifications>()),
      clock_(std::move(clock)) {}
HostNotifications::~HostNotifications() { Detach(); }
void HostNotifications::Attach(flutter::BinaryMessenger* messenger) {
  Detach();
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.notifications.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    const auto* args = call.arguments();
    if (call.method_name() == "probe" || call.method_name() == "clear") {
      if (args && !args->IsNull()) { result->Error("invalidData"); return; }
      if (call.method_name() == "probe") {
        result->Success(EncodableValue(EncodableMap{
          {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("notifications"), EncodableValue(backend_->Available())}}));
      } else {
        const bool cleared = backend_->Clear();
        recent_.clear(); burst_count_ = 0;
        result->Success(EncodableValue(EncodableMap{
          {EncodableValue("state"), EncodableValue(cleared ? "cleared" : "unavailable")}}));
      }
      return;
    }
    if (call.method_name() != "show") { result->NotImplemented(); return; }
    const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
    const auto* id = map ? String(*map, "id") : nullptr;
    const auto* kind = map ? String(*map, "kind") : nullptr;
    const auto* uri = map ? String(*map, "uri") : nullptr;
    const auto* privacy = map ? Get(*map, "private") : nullptr;
    const auto* private_preview = privacy ? std::get_if<bool>(privacy) : nullptr;
    if (!map || map->size() != 4 || !id || !kind || !uri || !private_preview ||
        !Validate({*id, *kind, *uri, *private_preview})) { result->Error("invalidData"); return; }
    const auto tag = Tag(*id);
    const ULONGLONG now = clock_();
    for (auto it = recent_.begin(); it != recent_.end();) {
      if (now - it->second >= 30000) it = recent_.erase(it); else ++it;
    }
    if (!burst_count_ || now - burst_start_ >= 60000) { burst_start_ = now; burst_count_ = 0; }
    const char* state = "unavailable";
    if (!tag.empty() && backend_->Available()) {
      if (recent_.find(tag) != recent_.end() || burst_count_ >= 5) state = "coalesced";
      else if (backend_->Submit(tag, ToastXml({*id, *kind, *uri, *private_preview}))) {
        recent_[tag] = now; ++burst_count_; state = "submitted";
      }
    }
    result->Success(EncodableValue(EncodableMap{{EncodableValue("state"), EncodableValue(state)}}));
  });
}
void HostNotifications::Detach() {
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset();
  // Toasts intentionally survive normal exit so protocol activation can cold-start.
}
