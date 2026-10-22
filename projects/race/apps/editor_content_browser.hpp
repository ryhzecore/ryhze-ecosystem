#pragma once
#include "desktop_scene.hpp"
#include "mesh_checkpoint.hpp"
#include "surface_pipeline.hpp"
#include "race/assets/assets.hpp"
#include <QWidget>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace race::content_project { class Snapshot; }
namespace race::desktop { struct ResolvedMesh; struct ResolvedMaterial; struct ResolvedTexture; }
namespace race::desktop::ui {
QString surface_profile_label(surface_pipeline::Profile);
struct ContentCapture {
  assets::AssetId id;
  assets::Digest index_revision,recipe_revision,source,cooked_revision,revision;
};
// GUI-thread view only. Owns the displayed immutable project snapshot and copies
// mesh references for presentation; it never retains geometry or starts work.
class ContentBrowser final : public QWidget {
  struct State;
  std::unique_ptr<State> state_;
  void refresh_details();
public:
  explicit ContentBrowser(QWidget* parent=nullptr);
  ~ContentBrowser() override;
  std::function<void()> on_new_project,on_open,on_add_recipe,on_refresh,on_cancel,on_reimport,on_update,on_selection;
  // Call only after a complete successful browser publication. A null snapshot
  // clears the view. Preserves the selected stable asset ID when still present.
  void set_snapshot(std::shared_ptr<const content_project::Snapshot>,QString full_project_path);
  // Scene selection and resident revisions are independent of the browser row.
  // The supplied span is consumed synchronously and is never retained.
  void set_scene(std::optional<MeshReference>,std::span<const ResolvedMesh>,
      std::optional<MaterialReference> = {},std::span<const ResolvedMaterial> = {},std::span<const ResolvedTexture> = {});
  // Displays one explicit completed capture, only for its matching browser row
  // and raw project/recipe pins. Passing null clears it. No Scene adoption occurs.
  void set_checkpoint(std::optional<mesh_checkpoint::Published>,QString full_checkpoint_path={});
  void set_capture(std::optional<ContentCapture>,QString full_checkpoint_path={});
  // scene_busy includes import/adoption/persistence work. Root owns cancellation
  // routing; operation_cancellable exposes Cancel without conflating adoption.
  void set_busy(bool browsing,bool scene_busy,bool can_update,
                bool can_reimport=false,bool operation_cancellable=false,
                bool can_create=false,bool can_add=false);
  std::optional<assets::AssetId> selected_asset() const;
  // Selects and reveals a present project row, emitting the normal selection
  // callback only if the row changes. Missing IDs leave the selection intact.
  bool select_asset(assets::AssetId);
};
} // namespace race::desktop::ui
