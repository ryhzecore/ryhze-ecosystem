#pragma once
#include "race/assets/assets.hpp"

// Executable-composition wire codec shared by typed and mesh-only consumers.
// Views borrow caller bytes only; each admitted project owns its complete closure.
namespace race::project_index {
inline constexpr std::size_t maximum_entries=32,maximum_bytes=8192;
inline constexpr std::uint32_t binary_version=1;
inline constexpr std::size_t binary_header_bytes=32,binary_entry_bytes=152,binary_checksum_bytes=32;
enum class Kind : std::uint8_t {Mesh=1,Texture=2,Material=3};
struct View {assets::AssetId id;Kind kind{Kind::Mesh};std::string_view recipe;assets::Digest revision;};
bool valid_index_leaf(std::string_view) noexcept;
// Validates and sorts caller-owned bounded scratch views, then allocates once.
Result<std::vector<std::byte>> encode(std::span<View>);
// Rejects every malformed record before any consumer can read a recipe.
Result<std::size_t> decode(std::span<const std::byte>,std::array<View,maximum_entries>&);
} // namespace race::project_index
