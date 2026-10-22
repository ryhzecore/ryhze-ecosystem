#pragma once
#include "race/assets/assets.hpp"
#include <optional>

namespace race::assets::surface {
inline constexpr std::size_t header_bytes=256,checksum_bytes=32,material_payload_bytes=72;
inline constexpr std::uint32_t portable_cpu_target=1;
enum class Representation : std::uint32_t { Imported=1,Cooked=2 };
enum class Encoding : std::uint32_t { SrgbColor=1,LinearData=2 };
enum class Model : std::uint32_t { OpaqueDiffuse=1 };
enum class Sampling : std::uint32_t { LinearClampMip0=1,LinearClampMips=2 };
enum class TextureRole : std::uint32_t { Color=1,DiffuseReflectanceR=2 };
struct Stamp {
  Representation representation{Representation::Imported};
  std::uint32_t target{};
  Digest source{},settings{},importer{},cooker{},dependencies{digest({})};
  friend bool operator==(const Stamp&,const Stamp&)=default;
};
struct Reference {
  AssetId asset;
  Digest revision{};
  bool valid() const noexcept;
  friend bool operator==(const Reference&,const Reference&)=default;
};
struct Material {
  Model model{Model::OpaqueDiffuse};
  std::array<double,3> base_color{1,1,1}; // Linear RGB multiplier, opaque.
  std::optional<Reference> base_color_texture;
  Sampling sampling{Sampling::LinearClampMip0};
  TextureRole texture_role{TextureRole::Color};
  friend bool operator==(const Material&,const Material&)=default;
};
struct Limits {std::size_t dimension{1024},product_bytes{8*1024*1024};};
Result<void> validate_limits(const Limits&) noexcept;
Result<void> validate_stamp(const Stamp&) noexcept;
// SHA-256 of the optional 48-byte little-endian ID/full-revision key; empty if
// absent. This is a dependency fingerprint, not a source or product revision.
Digest dependency_digest(const std::optional<Reference>&) noexcept;

// Validated borrowed immutable views. Caller retains unchanged original bytes
// throughout use. Runtime composition copies or transfers bytes into an owner
// before validation and retains that owner through every consumer's quiescence.
struct TextureMip {
  std::uint32_t width{},height{};
  std::span<const std::byte> pixels;
};
class TextureView {
  AssetId id_;
  Stamp stamp_;
  Digest revision_;
  std::uint32_t width_{},height_{},schema_{},mip_count_{};
  Encoding encoding_{};
  std::span<const std::byte> bytes_;
  TextureView(AssetId,Stamp,Digest,std::uint32_t,std::uint32_t,Encoding,std::uint32_t,std::uint32_t,std::span<const std::byte>);
  friend Result<TextureView> validate_texture(std::span<const std::byte>,const Limits&);
public:
  AssetId id() const noexcept{return id_;}
  const Stamp& stamp() const noexcept{return stamp_;}
  Digest revision() const noexcept{return revision_;}
  std::uint32_t width() const noexcept{return width_;}
  std::uint32_t height() const noexcept{return height_;}
  Encoding encoding() const noexcept{return encoding_;}
  std::uint32_t schema() const noexcept{return schema_;}
  std::uint32_t mip_count() const noexcept{return mip_count_;}
  // Tightly packed top-left RGBA8. pixels() always means the base raster;
  // texels() includes every level in descending size. Schema 2 is a complete
  // cooked opaque-sRGB chain; schema 3 is cooked opaque independent scalar RGB.
  // Schema 1 retains its mip-zero scalar/vector alpha policy.
  std::span<const std::byte> pixels() const noexcept{return bytes_.subspan(header_bytes,static_cast<std::size_t>(width_)*height_*4);}
  std::span<const std::byte> texels() const noexcept{return bytes_.subspan(header_bytes,bytes_.size()-header_bytes-checksum_bytes);}
  std::optional<TextureMip> mip(std::size_t level) const noexcept;
  std::span<const std::byte> bytes() const noexcept{return bytes_;}
};
class MaterialView {
  AssetId id_;
  Stamp stamp_;
  Digest revision_;
  Material material_;
  std::uint32_t schema_{};
  std::span<const std::byte> bytes_;
  MaterialView(AssetId,Stamp,Digest,Material,std::uint32_t,std::span<const std::byte>);
  friend Result<MaterialView> validate_material(std::span<const std::byte>,const Limits&);
public:
  AssetId id() const noexcept{return id_;}
  const Stamp& stamp() const noexcept{return stamp_;}
  Digest revision() const noexcept{return revision_;}
  const Material& material() const noexcept{return material_;}
  std::uint32_t schema() const noexcept{return schema_;}
  std::span<const std::byte> bytes() const noexcept{return bytes_;}
};
Result<TextureView> validate_texture(std::span<const std::byte>,const Limits& = {});
Result<MaterialView> validate_material(std::span<const std::byte>,const Limits& = {});
Result<std::vector<std::byte>> encode_texture(AssetId,const Stamp&,std::uint32_t width,std::uint32_t height,
    Encoding,std::span<const std::byte> pixels,const Limits& = {});
// Explicit schema 2 even for 1x1: cooked, opaque sRGB; each dimension halves
// with floor and clamps to one, through the final 1x1. Input contains all levels.
Result<std::vector<std::byte>> encode_texture_mips(AssetId,const Stamp&,std::uint32_t width,std::uint32_t height,
    Encoding,std::span<const std::byte> texels,const Limits& = {});
// Explicit schema 3: cooked, opaque LinearData; same complete dimensions as
// schema 2, with independent scalar RGB channels, never normalized vectors.
Result<std::vector<std::byte>> encode_texture_scalar_mips(AssetId,const Stamp&,std::uint32_t width,std::uint32_t height,
    Encoding,std::span<const std::byte> texels,const Limits& = {});
// Schema 1 retains optional linear-clamp-mip0 binding. Explicit schema 2 requires
// a pinned schema-2 texture and linear-clamp-mips. Schema 3 instead uses the
// pinned schema-3 LinearData R channel as diffuse reflectance. No PBR claim.
Result<std::vector<std::byte>> encode_material(AssetId,const Stamp&,const Material&,const Limits& = {});
// Exact pinned cooked texture for the declared color/scalar role, alpha255
// throughout. No fallback or
// partial dependency publication; caller retains the texture owner afterward.
Result<void> validate_binding(const MaterialView&,const TextureView&) noexcept;
} // namespace race::assets::surface
