#pragma once
#include "race/assets/mesh.hpp"

// Trusted, statically linked authoring code. Runtime Assets never parses OBJ or
// launches importers. This is a deliberately small OBJ profile, not a plugin ABI.
namespace race::mesh_pipeline {
enum class Profile : std::uint8_t {Geometry=1,GeometryUv=2};
struct Limits {
  std::size_t source_bytes{8 * 1024 * 1024}, line_bytes{1024}, token_bytes{64};
  std::size_t source_vertices{65536}, triangles{21845};
  assets::mesh::Limits mesh{};
};
Result<void> validate_limits(const Limits &) noexcept;
// Exact first-party import provenance, shared by importer and parent admission.
assets::mesh::Stamp import_stamp(assets::Digest source) noexcept;
Result<assets::mesh::Stamp> profile_import_stamp(Profile,assets::Digest source) noexcept;
assets::mesh::Stamp cook_stamp(assets::mesh::Stamp imported) noexcept;
// Auto-detecting direct-tool profile: complete UV mappings select schema 2.
// ASCII v x y z, optional vt u v and triangles; predeclared one-based indices.
// Metres, right-handed Y-up, counterclockwise faces, generated flat normals.
Result<std::vector<std::byte>> import_obj(assets::AssetId, std::span<const std::byte>,
                                        const Limits & = {});
// Recipe/worker admission requires this explicit profile before allocation.
// Geometry rejects UV mappings; GeometryUv requires complete UV mappings.
Result<std::vector<std::byte>> import_obj(assets::AssetId,std::span<const std::byte>,
                                        Profile,const Limits & = {});
Result<std::vector<std::byte>> cook(std::span<const std::byte>,
                                  const assets::mesh::Limits & = {});
// Content-addressed immutable leaf: <full SHA-256>.rasset. Never replaces a
// previous revision. Readback failure may leave a complete published product.
Result<std::string> publish(platform::RootedFiles &, std::span<const std::byte>,
                            const assets::mesh::Limits & = {});
} // namespace race::mesh_pipeline
