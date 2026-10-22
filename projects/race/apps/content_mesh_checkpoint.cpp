#include "content_mesh_checkpoint.hpp"
#include "mesh_checkpoint_internal.hpp"

namespace race::mesh_checkpoint {
namespace {
detail::ProjectPinView project_view(const content_project::Snapshot& project) noexcept {
  return {project.revision(),&project,[](const void* owner,assets::AssetId id) noexcept -> Result<detail::RecipePin> {
    auto entry=static_cast<const content_project::Snapshot*>(owner)->find(id);if(!entry)return entry.error();
    const auto* recipe=std::get_if<mesh_recipe::Snapshot>(&entry.value()->recipe);
    if(entry.value()->reference.kind!=content_project::Kind::Mesh||!recipe)
      return fail(ErrorCode::InvalidDependency,"Checkpoint asset is not a mesh recipe");
    return detail::RecipePin{recipe->revision,recipe->definition.profile};
  }};
}
}
Result<Published> capture(const content_project::Snapshot& project,assets::mesh::Lease lease,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints,const assets::mesh::Limits& limits){
  return detail::Core::capture(project_view(project),std::move(lease),products,checkpoints,limits);
}
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,const content_project::Snapshot& project,
    platform::RootedFiles& products,const assets::mesh::Limits& limits){
  return detail::Core::load(checkpoints,leaf,project_view(project),products,limits);
}
} // namespace race::mesh_checkpoint
