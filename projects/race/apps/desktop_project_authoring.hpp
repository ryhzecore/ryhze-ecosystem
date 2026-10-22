#pragma once
#include "desktop_project_browser.hpp"
#include <functional>
#include <optional>
#include <thread>

namespace race::desktop {
enum class ProjectAuthoringKind : std::uint8_t {Create,Add};
struct ProjectAuthoringRequest {
  ProjectAuthoringKind kind{ProjectAuthoringKind::Create};
  // Retained capacities are bounded too: 4096/root and 96/canonical leaf.
  std::string recipe_root,source_root,product_root,checkpoint_root;
  std::string new_index_leaf,source_leaf;
  assets::AssetId asset{};
  std::uint64_t epoch{};
  // Explicit recipe interpretation. Create accepts only the default profile.
  mesh_pipeline::Profile profile{mesh_pipeline::Profile::Geometry};
  // An explicit surface profile selects texture/material authoring. It requires
  // the default mesh profile; texture is an optional complete cooked product key.
  std::optional<surface_recipe::Profile> surface_profile;
  std::optional<assets::surface::Reference> texture;
};
enum class ProjectAuthoringPhase : std::uint8_t {
  Idle,Revalidating,ReadingSource,Publishing,ReadingBack,Complete
};
struct ProjectAuthoringProgress {
  ProjectAuthoringPhase phase{ProjectAuthoringPhase::Idle};
  bool cancellation_requested{};
};
struct ProjectAuthoringCompletion {
  std::uint64_t request{},epoch{};
  ProjectAuthoringKind kind{ProjectAuthoringKind::Create};
  assets::AssetId asset{};
  ProjectAuthoringRequest names;
  std::string recipe_leaf;
  assets::Digest source{};
  std::size_t source_bytes{},entries{},retained_bytes{};
  Result<void> result;
  bool changed{};
  double admission_ms{},revalidate_ms{},source_ms{},publish_ms{},readback_ms{},publication_ms{};
};
// One joined worker. Creates a binary .raceproject or adds one explicit recipe without
// importing, cooking or modifying Scene. Caller serializes other file tasks and explicitly
// cancels browser selection ABA. Scene selection and inspector input are not
// borrowed or changed. Add requires the displayed recipe root; Create
// may start with no view. The original visible pointer and epoch gate completion.
// Destination index must be absent at preflight, even if byte-identical. A race
// after absence may converge on identical immutable bytes; different bytes fail.
// Digest-named recipes may already exist only when their exact bytes match.
// Complete index/recipe readback precedes the single no-throw visible-view swap.
class ProjectAuthoringTask {
public:
  using RootFactory=ProjectBrowserTask::RootFactory;
private:
  struct Pending;
  const std::thread::id owner_{std::this_thread::get_id()};
  RootFactory roots_;
  std::uint64_t sequence_{};
  std::unique_ptr<Pending> pending_;
  std::thread worker_;
public:
  explicit ProjectAuthoringTask(RootFactory={});
  ~ProjectAuthoringTask();
  ProjectAuthoringTask(const ProjectAuthoringTask&)=delete;
  ProjectAuthoringTask& operator=(const ProjectAuthoringTask&)=delete;
  bool busy() const noexcept{return worker_.joinable();} // Owner observation.
  Result<std::uint64_t> start(std::shared_ptr<const ProjectBrowserView> prior,ProjectAuthoringRequest);
  Result<void> cancel();
  Result<ProjectAuthoringProgress> progress() const noexcept;
  Result<std::optional<ProjectAuthoringCompletion>> poll(std::uint64_t epoch,
      std::shared_ptr<const ProjectBrowserView>& visible);
};
} // namespace race::desktop
