#include "steam_inventory.h"
#include "secure_files.h"
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <map>
#include <set>
#include <utility>

namespace ryhze {
namespace {
struct Node {
  bool object = false;
  std::string value;
  std::map<std::string, Node> children;
};
// Deliberately bounded quoted KeyValues subset. Unknown syntax fails closed.
class Parser {
 public:
  explicit Parser(const std::string& input) : input_(input) {}
  bool Parse(Node* root) { root->object = true; return Object(root, 0, false); }
 private:
  const std::string& input_;
  size_t position_ = 0;
  size_t nodes_ = 0;
  void Space() {
    while (position_ < input_.size()) {
      const char c = input_[position_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') { ++position_; continue; }
      if (c == '/' && position_ + 1 < input_.size() && input_[position_ + 1] == '/') {
        while (position_ < input_.size() && input_[position_] != '\n') ++position_;
        continue;
      }
      break;
    }
  }
  bool Text(std::string* out) {
    Space();
    if (position_ >= input_.size() || input_[position_++] != '"') return false;
    while (position_ < input_.size()) {
      unsigned char c = static_cast<unsigned char>(input_[position_++]);
      if (c == '"') return true;
      if (c == '\\') {
        if (position_ >= input_.size()) return false;
        c = static_cast<unsigned char>(input_[position_++]);
        if (c != '\\' && c != '"') return false;
      }
      if (c < 32 || c == 127 || out->size() >= 8192) return false;
      out->push_back(static_cast<char>(c));
    }
    return false;
  }
  bool Object(Node* out, size_t depth, bool nested) {
    if (depth > 8) return false;
    for (;;) {
      Space();
      if (position_ == input_.size()) return !nested;
      if (input_[position_] == '}') { ++position_; return nested; }
      std::string key;
      if (++nodes_ > 20000 || !Text(&key) || key.empty()) return false;
      Space();
      Node child;
      if (position_ < input_.size() && input_[position_] == '{') {
        ++position_; child.object = true;
        if (!Object(&child, depth + 1, true)) return false;
      } else if (!Text(&child.value)) return false;
      if (!out->children.emplace(key, std::move(child)).second) return false;
    }
  }
};
std::optional<std::wstring> Wide(const std::string& input) {
  if (input.empty() || input.size() > 1024 * 1024) return std::nullopt;
  const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
      static_cast<int>(input.size()), nullptr, 0);
  if (!size) return std::nullopt;
  std::wstring value(static_cast<size_t>(size), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(),
      static_cast<int>(input.size()), value.data(), size) != size) return std::nullopt;
  return value;
}
std::string Utf8(const std::wstring& input) {
  const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
      static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
  if (!size) return {};
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
      static_cast<int>(input.size()), result.data(), size, nullptr, nullptr);
  return result;
}
bool Number(const std::string& value, bool zero = false) {
  if (value.empty() || value.size() > 10 || (value.size() > 1 && value[0] == '0')) return false;
  uint64_t result = 0;
  for (const char c : value) {
    if (c < '0' || c > '9') return false;
    result = result * 10 + static_cast<unsigned>(c - '0');
  }
  return result <= UINT32_MAX && (zero || result != 0);
}
const Node* Child(const Node& parent, const char* key, bool object) {
  const auto found = parent.children.find(key);
  return found != parent.children.end() && found->second.object == object ? &found->second : nullptr;
}
std::optional<std::wstring> RegistryRoot() {
  auto read = [](const wchar_t* name) -> std::optional<std::wstring> {
    wchar_t value[32768]{};
    DWORD bytes = static_cast<DWORD>(sizeof(value));
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", name,
        RRF_RT_REG_SZ | RRF_NOEXPAND, nullptr, value, &bytes) != ERROR_SUCCESS ||
        bytes < sizeof(wchar_t) || bytes > sizeof(value) || value[bytes / sizeof(wchar_t) - 1] != 0) return std::nullopt;
    std::wstring result(value);
    if ((result.size() + 1) * sizeof(wchar_t) != bytes) return std::nullopt;
    std::replace(result.begin(), result.end(), L'/', L'\\');
    return result;
  };
  auto root = read(L"SteamPath");
  auto executable = read(L"SteamExe");
  if (!root || !executable || root->empty()) return std::nullopt;
  while (root->size() > 3 && root->back() == L'\\') root->pop_back();
  if (_wcsicmp(executable->c_str(), (*root + L"\\steam.exe").c_str()) != 0) return std::nullopt;
  return root;
}
bool Read(const secure_files::Root& root, const std::wstring& name,
          size_t limit, size_t* budget, Node* out) {
  if (*budget == 0) return false;
  secure_files::Error error;
  secure_files::FileInfo info;
  std::vector<uint8_t> bytes;
  // Root checks the pinned file size before ReadFile. Apply the remaining scan
  // budget there, rather than reading and rejecting every oversized tail file.
  if (!root.Read(name, std::min(limit, *budget), &bytes, &info, &error)) return false;
  *budget -= bytes.size();
  const std::string input(bytes.begin(), bytes.end());
  return Wide(input).has_value() && Parser(input).Parse(out);
}
bool Cancelled(const std::atomic_bool* flag) { return flag && flag->load(); }
}  // namespace

SteamInventoryResult InspectSteamInstallations(
    const std::optional<std::wstring>& fixture_root, const std::atomic_bool* cancelled) {
  SteamInventoryResult result;
  if (Cancelled(cancelled)) return result;
  const auto location = fixture_root ? fixture_root : RegistryRoot();
  if (!location) return result;
  secure_files::Error error;
  auto root = secure_files::Root::Open(*location, false, &error);
  if (!root) return result;
  size_t budget = 8 * 1024 * 1024;
  Node document;
  if (!Read(*root, L"steamapps\\libraryfolders.vdf", 1024 * 1024, &budget, &document) ||
      document.children.size() != 1) return result;
  const auto* libraries = Child(document, "libraryfolders", true);
  if (!libraries || libraries->children.size() > 32) return result;
  result.quality = SteamInventoryQuality::complete;
  std::set<std::string> ids;
  std::set<std::wstring> roots;
  size_t attempts = 0;
  for (const auto& pair : libraries->children) {
    if (Cancelled(cancelled)) return {};
    const auto* path = Child(pair.second, "path", false);
    const auto* apps = Child(pair.second, "apps", true);
    if (!Number(pair.first, true) || !pair.second.object || !path || !apps) {
      result.quality = SteamInventoryQuality::partial; continue;
    }
    const auto wide = Wide(path->value);
    if (!wide || wide->size() > 32767) { result.quality = SteamInventoryQuality::partial; continue; }
    auto key = *wide;
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    if (!roots.insert(key).second) { result.quality = SteamInventoryQuality::partial; continue; }
    auto library = secure_files::Root::Open(*wide, false, &error);
    if (!library) { result.quality = SteamInventoryQuality::partial; continue; }
    for (const auto& app : apps->children) {
      if (Cancelled(cancelled)) return {};
      if (++attempts > 1000 || budget == 0) { result.quality = SteamInventoryQuality::partial; return result; }
      if (!Number(app.first) || app.second.object) {
        result.quality = SteamInventoryQuality::partial; continue;
      }
      if (!ids.insert(app.first).second) {
        result.values.erase(std::remove_if(result.values.begin(), result.values.end(),
            [&](const SteamInstallationObservation& item) { return item.app_id == app.first; }), result.values.end());
        result.quality = SteamInventoryQuality::partial; continue;
      }
      Node manifest;
      const auto app_id = Wide(app.first);
      if (!Read(*library, L"steamapps\\appmanifest_" + *app_id + L".acf", 64 * 1024,
                &budget, &manifest) || manifest.children.size() != 1) {
        result.quality = SteamInventoryQuality::partial; continue;
      }
      const auto* state = Child(manifest, "AppState", true);
      if (!state) { result.quality = SteamInventoryQuality::partial; continue; }
      const auto* id = Child(*state, "appid", false);
      const auto* name = Child(*state, "name", false);
      const auto* directory = Child(*state, "installdir", false);
      const auto* flags = Child(*state, "StateFlags", false);
      if (!id || id->value != app.first || !name || name->value.empty() || name->value.size() > 512 ||
          !directory || directory->value.empty() || directory->value.size() > 255 ||
          directory->value.find_first_of("/\\:") != std::string::npos ||
          !flags || !Number(flags->value, true)) {
        result.quality = SteamInventoryQuality::partial; continue;
      }
      const auto relative = Wide("steamapps\\common\\" + directory->value);
      secure_files::FileInfo info;
      if (!relative || !library->Inspect(*relative, &info, &error) || !info.is_directory) {
        result.quality = SteamInventoryQuality::partial; continue;
      }
      result.values.push_back({app.first, name->value, Utf8(*wide), Utf8(*relative), flags->value});
    }
  }
  return result;
}
}  // namespace ryhze
