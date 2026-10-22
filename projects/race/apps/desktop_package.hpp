#pragma once
#include "desktop_meshes.hpp"
#include "race/assets/package.hpp"

namespace race::desktop {
struct PreparedPackage {
  scene::Document scene;
  std::shared_ptr<const MeshSet> meshes;
  assets::pak::Key scene_key;
};
// This composition admits exactly one cooked desktop Scene and its complete
// typed Mesh/Material/Texture closure. Missing entries never fall back to loose files.
Result<PreparedPackage> prepare_package(std::span<const std::byte>,const scene::Schemas&);
Result<PreparedPackage> read_package(platform::Files&,std::string_view,const scene::Schemas&);
Result<std::vector<std::byte>> pack_scene(platform::RootedFiles&,std::string_view,
                                       PersistentId,const scene::Schemas&);
Result<std::string> publish_package(platform::RootedFiles&,std::span<const std::byte>,const scene::Schemas&);
} // namespace race::desktop
