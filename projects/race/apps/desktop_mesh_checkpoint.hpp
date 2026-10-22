#pragma once
#include "desktop_meshes.hpp"
#include <functional>
#include <optional>
#include <string>
#include <thread>

namespace race::desktop {
struct MeshCheckpointRequest {
  // Retained capacities are limited too: 4096/root and 96/canonical leaf.
  std::string recipe_root, project_leaf, checkpoint_root, checkpoint_leaf;
  // Optional browser pin of Snapshot::revision(): the exact raw index bytes,
  // not its canonical encoding. Legacy callers remain explicitly unpinned.
  std::optional<assets::Digest> expected_project_revision{};
};
struct MeshCheckpointCompletion {
  std::uint64_t request{};
  PersistentId selected;
  MeshReference previous, replacement;
  Result<void> result;
  bool changed{};
  double snapshot_ms{}, load_ms{}, prepare_ms{}, commit_ms{};
  std::size_t product_bytes{};
};
// One owner, one joined worker and one completion. Callers must gate admission
// against Scene persistence and qualify the selected root as loose content.
// Explicit cancel() is required for input/selection ABA, preview admission and
// history clearing (which does not advance the Document generation).
class MeshCheckpointTask {
public:
  using RootFactory=std::function<Result<std::shared_ptr<platform::RootedFiles>>(std::string_view)>;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  std::uint64_t sequence_{};
  bool cancelled_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit MeshCheckpointTask(RootFactory={});
  ~MeshCheckpointTask();
  MeshCheckpointTask(const MeshCheckpointTask&)=delete;
  MeshCheckpointTask& operator=(const MeshCheckpointTask&)=delete;
  bool busy() const noexcept { return worker_.joinable(); } // Owner observation.
  Result<std::uint64_t> start(const Document&,std::shared_ptr<const MeshSet>,
                            PersistentId selected,MeshCheckpointRequest);
  // Sticky publication cancellation; physical preparation remains retained.
  Result<void> cancel();
  Result<std::optional<MeshCheckpointCompletion>> poll(Document&,
      std::shared_ptr<const MeshSet>& current,PersistentId selection);
};
} // namespace race::desktop
