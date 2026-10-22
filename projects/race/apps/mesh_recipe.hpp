#pragma once
#include "mesh_pipeline.hpp"

namespace race::mesh_recipe {
inline constexpr std::size_t maximum_bytes=4096;
struct Definition {assets::AssetId id;std::string source;mesh_pipeline::Profile profile{mesh_pipeline::Profile::Geometry};};
struct Snapshot {
  Definition definition;
  assets::Digest revision;
  // Exact admitted input, independently owned. Canonical reserialization may
  // differ in whitespace/order; neither recipe digest changes mesh provenance.
  std::vector<std::byte> bytes;
};
// Schemas 1/2: one flat object, unescaped printable ASCII strings, strict unsigned
// JSON integers, ten required fields, no unknown/duplicate fields. Profile is
// schema 1 Geometry -> native mesh schema 1, schema 2 GeometryUv -> schema 2;
// both use their exact compiled hashes and portable CPU target 1.
// Roots, source contents and executable authority are not recipe fields.
Result<std::string> encode(const Definition&);
Result<Snapshot> decode(std::span<const std::byte>);
Result<Snapshot> load(platform::RootedFiles&,std::string_view recipe_leaf);
} // namespace race::mesh_recipe
