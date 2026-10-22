#ifndef RUNNER_STEAM_INVENTORY_H_
#define RUNNER_STEAM_INVENTORY_H_
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace ryhze {
// File observations only. No ownership, executable or completed-install claim.
struct SteamInstallationObservation {
  std::string app_id;
  std::string title;
  std::string library_root;
  std::string relative_directory;
  std::string state_flags;
};
enum class SteamInventoryQuality { complete, partial, unavailable };
struct SteamInventoryResult {
  SteamInventoryQuality quality = SteamInventoryQuality::unavailable;
  std::vector<SteamInstallationObservation> values;
};
// Production root comes only from paired current-user Steam registry values.
// Explicit roots are a native composition/test seam, never channel arguments.
SteamInventoryResult InspectSteamInstallations(
    const std::optional<std::wstring>& fixture_root = std::nullopt,
    const std::atomic_bool* cancelled = nullptr);
}  // namespace ryhze
#endif
