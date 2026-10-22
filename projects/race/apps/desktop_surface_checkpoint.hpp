#pragma once
#include "desktop_project_browser.hpp"
#include "desktop_meshes.hpp"
#include "surface_checkpoint.hpp"
#include <functional>
#include <optional>
#include <thread>

namespace race::desktop {
struct SurfaceCheckpointRequest {
  std::string checkpoint_root,checkpoint_leaf; // capacity <=4096/root, <=96/leaf.
  assets::AssetId asset{}; // Selected Content asset, distinct from Scene selection.
  std::uint64_t epoch{};
};
struct SurfaceCheckpointCompletion {
  std::uint64_t request{},epoch{};
  PersistentId selected{};
  assets::AssetId asset{};
  std::optional<MaterialReference> previous;
  MaterialReference replacement;
  Result<void> result;
  bool changed{};
  std::size_t product_bytes{},texture_bytes{};
  double snapshot_ms{},load_ms{},prepare_ms{},commit_ms{};
};
// Explicit material checkpoint adoption only. Texture checkpoints supply exact
// recipe keys for material authoring, never a direct Scene assignment. All IO
// and preparation are on the joined worker; owner commits Scene then cache.
class SurfaceCheckpointTask {
public:
  using RootFactory=ProjectBrowserTask::RootFactory;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  std::uint64_t sequence_{};
  bool cancelled_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit SurfaceCheckpointTask(RootFactory={});
  ~SurfaceCheckpointTask();
  SurfaceCheckpointTask(const SurfaceCheckpointTask&)=delete;
  SurfaceCheckpointTask& operator=(const SurfaceCheckpointTask&)=delete;
  bool busy() const noexcept{return worker_.joinable();}
  Result<std::uint64_t> start(const Document&,std::shared_ptr<const MeshSet>,PersistentId selected,
      std::shared_ptr<const ProjectBrowserView>,SurfaceCheckpointRequest);
  Result<void> cancel(); // Explicitly signal selection/input/history ABA.
  Result<std::optional<SurfaceCheckpointCompletion>> poll(Document&,std::shared_ptr<const MeshSet>& current,
      PersistentId selection,std::shared_ptr<const ProjectBrowserView> current_project,
      assets::AssetId content_selection,std::uint64_t epoch,bool allow_publication=true);
};
} // namespace race::desktop
