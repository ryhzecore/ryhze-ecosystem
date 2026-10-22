#include "steam_inventory.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
namespace fs = std::filesystem;
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string Escape(const std::string& value) {
  std::string out;
  for (char c : value) { if (c == '\\' || c == '"') out += '\\'; out += c; }
  return out;
}
void Put(const fs::path& path, const std::string& value) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out << value; Check(out.good(), "synthetic fixture write");
}
std::string Manifest(const char* id = "123", const char* folder = "Synthetic Game") {
  return std::string("\"AppState\" { \"appid\" \"") + id +
      "\" \"name\" \"Synthetic Game\" \"installdir\" \"" + folder + "\" \"StateFlags\" \"4\" }";
}
void BudgetBoundary(const fs::path& root) {
  constexpr size_t scan_budget = 8 * 1024 * 1024;
  constexpr size_t manifest_size = 64 * 1024;
  std::string document = "\"libraryfolders\" { \"0\" { \"path\" \"" +
      Escape(root.u8string()) + "\" \"apps\" { ";
  // Fixed-width IDs retain numeric ordering in the native KeyValues map. The
  // first half consumes almost the entire budget; the rest must not be read.
  for (unsigned i = 1000; i < 1256; ++i) {
    const auto id = std::to_string(i);
    document += "\"" + id + "\" \"10\" ";
    auto manifest = Manifest(id.c_str());
    manifest.resize(manifest_size, ' ');
    Put(root / L"steamapps" / (L"appmanifest_" + std::to_wstring(i) + L".acf"), manifest);
  }
  document += "\"9999\" \"10\" } } }";
  Put(root / L"steamapps" / L"libraryfolders.vdf", document);
  const auto tail = Manifest("9999");
  Put(root / L"steamapps" / L"appmanifest_9999.acf", tail);
  const size_t full_count = (scan_budget - document.size()) / manifest_size;
  Check((scan_budget - document.size()) % manifest_size > tail.size(),
        "fixture leaves room for a smaller later record");
  IO_COUNTERS before{}, after{};
  Check(GetProcessIoCounters(GetCurrentProcess(), &before) != 0, "read byte counter before scan");
  const auto value = ryhze::InspectSteamInstallations(root.wstring());
  Check(GetProcessIoCounters(GetCurrentProcess(), &after) != 0, "read byte counter after scan");
  const ULONGLONG transferred = after.ReadTransferCount - before.ReadTransferCount;
  // The standalone harness has no background readers. Windows counts transferred
  // bytes even for cached file reads; checking outcomes alone misses over-reading.
  Check(transferred >= full_count * manifest_size && transferred <= scan_budget,
        "aggregate actual file reads stay within eight MiB");
  Check(value.quality == ryhze::SteamInventoryQuality::partial &&
      value.values.size() == full_count + 1 &&
      std::any_of(value.values.begin(), value.values.end(), [](const auto& item) { return item.app_id == "9999"; }),
      "oversized tail records preserve partial results and later in-budget record");
}
}
int main() {
  fs::path root;
  try {
    wchar_t base[32768]{};
    Check(GetTempPathW(32768, base) > 0, "temporary fixture base");
    root = fs::path(base) / (L"ryhze-steam-inventory-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    Check(CreateDirectoryW(root.c_str(), nullptr) != 0, "exclusive synthetic fixture");
    fs::create_directories(root / L"steamapps" / L"common" / L"Synthetic Game");
    const auto manifest = root / L"steamapps" / L"appmanifest_123.acf";
    Put(root / L"steamapps" / L"libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + Escape(root.u8string()) + "\" \"apps\" { \"123\" \"10\" } } }");
    Put(manifest, Manifest());
    auto value = ryhze::InspectSteamInstallations(root.wstring());
    Check(value.quality == ryhze::SteamInventoryQuality::complete && value.values.size() == 1 &&
        value.values[0].app_id == "123" && value.values[0].state_flags == "4", "exact observation without ownership");
    // Unlisted files are not enumerated, let alone read.
    Put(root / L"steamapps" / L"appmanifest_999.acf", "malformed unlisted file");
    Check(ryhze::InspectSteamInstallations(root.wstring()).values.size() == 1, "declared IDs only");
    Put(manifest, Manifest("124"));
    value = ryhze::InspectSteamInstallations(root.wstring());
    Check(value.quality == ryhze::SteamInventoryQuality::partial && value.values.empty(), "manifest ID mismatch fails closed");
    Put(manifest, Manifest("123", ".."));
    Check(ryhze::InspectSteamInstallations(root.wstring()).values.empty(), "directory traversal rejected");
    Put(manifest, "\"AppState\" { \"appid\" \"123\" \"appid\" \"123\" }");
    Check(ryhze::InspectSteamInstallations(root.wstring()).values.empty(), "duplicate keys rejected");
    Put(manifest, std::string(65537, 'x'));
    Check(ryhze::InspectSteamInstallations(root.wstring()).values.empty(), "manifest byte bound");
    Put(manifest, Manifest());
    const auto alias = root / L"alias.acf";
    Check(CreateHardLinkW(alias.c_str(), manifest.c_str(), nullptr) != 0, "synthetic hardlink");
    Check(ryhze::InspectSteamInstallations(root.wstring()).values.empty(), "multiply linked manifest rejected");
    Check(DeleteFileW(alias.c_str()) != 0, "remove owned alias");
    std::atomic_bool cancelled{true};
    Check(ryhze::InspectSteamInstallations(root.wstring(), &cancelled).quality ==
        ryhze::SteamInventoryQuality::unavailable, "cancelled scan has no observations");
    Check(ryhze::InspectSteamInstallations(root.wstring() + L"\\missing").quality ==
        ryhze::SteamInventoryQuality::unavailable, "missing root unavailable");
    BudgetBoundary(root);
    // Restore the small manifest list before the independent reparse fixture.
    Put(root / L"steamapps" / L"libraryfolders.vdf", "\"libraryfolders\" { \"0\" { \"path\" \"" + Escape(root.u8string()) + "\" \"apps\" { \"123\" \"10\" } } }");
    const auto target = root / L"steamapps" / L"common" / L"Synthetic Game";
    fs::remove(target);
    if (CreateSymbolicLinkW(target.c_str(), root.c_str(), SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2)) {
      Check(ryhze::InspectSteamInstallations(root.wstring()).values.empty(), "reparse install directory rejected");
      fs::remove(target);
    } else { std::cout << "SKIP directory symlink fixture unavailable\n"; }
    fs::remove_all(root);
    std::cout << "PASS bounded read-only Steam inventory synthetic fixtures\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL " << error.what() << "\n";
    return 1;
  }
}
