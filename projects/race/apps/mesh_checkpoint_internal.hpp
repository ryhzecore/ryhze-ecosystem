#pragma once
#include "mesh_checkpoint.hpp"

namespace race::mesh_checkpoint::detail {
// Private synchronous bridge: each adapter supplies an already admitted owner.
// Never synthesize an index or expose a selected subset as project authority.
struct RecipePin {assets::Digest revision;mesh_pipeline::Profile profile;};
struct ProjectPinView {
  assets::Digest revision;
  const void* owner;
  Result<RecipePin> (*lookup)(const void*,assets::AssetId) noexcept;
  Result<RecipePin> find(assets::AssetId id) const noexcept{return lookup(owner,id);}
};
struct Core {
  static Result<Published> capture(ProjectPinView,assets::mesh::Lease,
      platform::RootedFiles&,platform::RootedFiles&,const assets::mesh::Limits&);
  static Result<Loaded> load(platform::RootedFiles&,std::string_view,ProjectPinView,
      platform::RootedFiles&,const assets::mesh::Limits&);
};
} // namespace race::mesh_checkpoint::detail
