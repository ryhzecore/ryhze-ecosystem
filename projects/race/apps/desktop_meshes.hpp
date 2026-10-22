#pragma once
#include "desktop_scene.hpp"
#include "mesh_render.hpp"
#include "race/render/viewport.hpp"
#include "race/assets/surface.hpp"

namespace race::desktop {
struct ResolvedMesh {
  MeshReference reference;
  assets::mesh::Bounds bounds;
  std::shared_ptr<const render::ViewportGeometry> geometry;
};
struct ResolvedTexture {
  MeshReference reference;
  std::shared_ptr<const render::ViewportTexture> texture;
};
struct ResolvedMaterial {
  MaterialReference reference;
  std::optional<MeshReference> texture_reference;
  std::shared_ptr<const render::ViewportMaterial> material;
};
// Immutable prepared dependencies. Preparation may run on a worker; publication
// is an application-owner pointer swap after the matching Scene change commits.
class MeshSet {
  std::shared_ptr<platform::RootedFiles> files_;
  std::vector<ResolvedMesh> entries_;
  std::vector<ResolvedTexture> textures_;
  std::vector<ResolvedMaterial> materials_;
  Result<void> load_materials(std::span<const MaterialReference>,
      const std::shared_ptr<const MeshSet>&,std::span<const std::byte> exact_product={});
  MeshSet(std::shared_ptr<platform::RootedFiles>,std::vector<ResolvedMesh>);
public:
  static Result<std::shared_ptr<const MeshSet>> prepare(
      std::shared_ptr<platform::RootedFiles>,std::span<const MeshReference>,
      const std::shared_ptr<const MeshSet>& reuse={},std::span<const MaterialReference> materials={});
  // Add one already-owned cooked revision of an asset retained by prior. No file
  // read occurs; existing packets and the root capability remain shared. Even a
  // duplicate validates the complete supplied product before returning prior.
  static Result<std::shared_ptr<const MeshSet>> extend(
      std::shared_ptr<const MeshSet> prior,std::span<const std::byte> exact_product);
  // Adds or updates a material, retaining previous revisions for Undo. The exact
  // material bytes are admitted before reading any missing texture dependency.
  static Result<std::shared_ptr<const MeshSet>> extend_material(
      std::shared_ptr<const MeshSet> prior,std::span<const std::byte> exact_product);
  const auto& files() const noexcept{return files_;}
  std::span<const ResolvedMesh> entries() const noexcept{return entries_;}
  std::span<const ResolvedMaterial> materials() const noexcept{return materials_;}
  std::span<const ResolvedTexture> textures() const noexcept{return textures_;}
  Result<const ResolvedMaterial*> find_material(const MaterialReference&) const;
  Result<void> validate_scene(const scene::Document&) const;
  Result<const ResolvedMesh*> find(const MeshReference&) const;
  // Count every Scene instance, including hidden objects, before publication.
  Result<void> validate_instances(std::span<const MeshReference>) const;
};
Result<MaterialReference> material_file_reference(platform::RootedFiles&,std::string_view leaf);
Result<MeshReference> mesh_file_reference(platform::RootedFiles&,std::string_view leaf);
Result<assets::mesh::Bounds> local_bounds(const ObjectRow&,const MeshSet*);
Result<render::DrawPrimitive> mesh_draw(const ObjectRow&,const MeshSet*,bool selected=false);
// Applies the Scene's RenderOutput/DirectionalLight settings to one frame: missing or legacy
// settings leave LegacyViewportV1 with inactive physical fields; a physical profile resolves
// its photometric values here so an unsupported request (HDR presentation) rejects before any
// GPU work instead of being downgraded.
Result<void> apply_lighting(render::RenderFrame&,const LightingSettings&);
Result<render::RenderFrame> scene_frame(const Document&,const MeshSet*,
    const render::ViewportCamera&,bool play=false,std::uint64_t tick=0);
} // namespace race::desktop
