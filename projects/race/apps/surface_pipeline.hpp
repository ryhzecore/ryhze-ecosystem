#pragma once
#include "race/assets/surface.hpp"

// Trusted offline authoring adapters. Runtime consumers use native products only.
namespace race::surface_pipeline {
inline constexpr std::size_t source_bytes = 8 * 1024 * 1024;
inline constexpr std::size_t material_source_bytes = 4096;
// Explicit recipe profiles. Direct import_texture remains signature-dispatched.
enum class Profile : std::uint8_t {
  PpmSrgb=1,PpmLinear,PpmSrgbMips,BmpSrgb,BmpLinear,BmpSrgbMips,Material,MaterialMips,
  PpmScalarMips,BmpScalarMips,MaterialScalarMips
};
struct ProfileInfo {
  std::string_view name,source_extension;
  bool material{},mips{};
  assets::surface::Encoding encoding{assets::surface::Encoding::SrgbColor};
  std::uint32_t imported_schema{1},cooked_schema{1};
  // Compiled tuples with source zero and empty dependency digest. Callers fill
  // the exact source digest and declared optional material dependency digest.
  assets::surface::Stamp imported,cooked;
};
Result<ProfileInfo> profile_info(Profile) noexcept;
Result<Profile> parse_profile(std::string_view) noexcept;
// Source bytes select the profile: BM is bounded BMP24/INFOHEADER, otherwise
// the unchanged P6 parser handles the source. Filenames do not identify formats.
Result<std::vector<std::byte>> import_texture(assets::AssetId, std::span<const std::byte>,
                                            assets::surface::Encoding);
Result<std::vector<std::byte>> import_ppm(assets::AssetId, std::span<const std::byte>,
                                        assets::surface::Encoding);
Result<std::vector<std::byte>> import_material(assets::AssetId, std::span<const std::byte>);
// Textured materials require the exact cooked dependency in the provided root.
Result<std::vector<std::byte>> cook(std::span<const std::byte>, platform::RootedFiles* = nullptr);
// Explicit offline opaque-sRGB complete-chain profile; imported textures only.
Result<std::vector<std::byte>> cook_mips(std::span<const std::byte>);
// Explicit offline opaque independent scalar RGB chain; imported LinearData only.
Result<std::vector<std::byte>> cook_scalar_mips(std::span<const std::byte>);
Result<std::string> publish(platform::RootedFiles&, std::span<const std::byte>);
} // namespace race::surface_pipeline
