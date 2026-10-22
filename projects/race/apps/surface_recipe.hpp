#pragma once
#include "surface_pipeline.hpp"

namespace race::surface_recipe {
inline constexpr std::size_t maximum_bytes=4096;
using Profile=surface_pipeline::Profile;
struct Definition {
  assets::AssetId id;
  std::string source;
  Profile profile{Profile::PpmSrgb};
  std::optional<assets::surface::Reference> texture;
  friend bool operator==(const Definition&,const Definition&)=default;
};
struct Snapshot {
  Definition definition;
  assets::Digest revision;
  std::vector<std::byte> bytes; // Exact raw admitted recipe, independently owned.
};
struct Built {
  std::vector<std::byte> imported,cooked;
};
// Schema 1: fourteen required flat fields; fixed compiled profile tuple, strict
// unsigned JSON integers and unescaped printable ASCII. No roots or executable
// authority. Texture pins are complete cooked keys, never recipe/latest aliases.
Result<std::string> encode(const Definition&);
Result<Snapshot> decode(std::span<const std::byte>);
Result<Snapshot> load(platform::RootedFiles&,std::string_view recipe_leaf);
// Revalidates the owned raw snapshot and explicit source/profile/dependency
// meaning. Produces both owned validated products without publishing any file.
// Material cooking requires its exact cooked texture in dependencies when bound.
Result<Built> build(const Snapshot&,std::span<const std::byte> source,
                    platform::RootedFiles* dependencies=nullptr);
} // namespace race::surface_recipe
