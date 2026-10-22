#pragma once
#include "race/assets/assets.hpp"

namespace race::assets::collision {
// Independent analytic collision geometry. No render mesh/LOD, body mass,
// solver instance, SDK type or serialized SDK state is part of this product.
inline constexpr std::size_t header_bytes=256, checksum_bytes=32, payload_bytes=24;
inline constexpr std::size_t product_bytes=header_bytes+payload_bytes+checksum_bytes;
inline constexpr std::uint32_t native_kind=4, schema=1, geometry_profile=1, portable_cpu_target=1;
enum class Kind : std::uint32_t { Sphere=1, Box=2 };
enum class Representation : std::uint32_t { Imported=1, Cooked=2 };
struct Stamp {
  Representation representation{Representation::Imported};
  std::uint32_t target{};
  Digest source{}, settings{}, importer{}, cooker{}, dependencies{digest({})};
  friend bool operator==(const Stamp&,const Stamp&)=default;
};
struct Primitive {
  Kind kind{Kind::Box};
  // Full dimensions in metres, positive scale already baked. Spheres use the
  // same diameter on all axes. Body/motion admission may impose tighter bounds.
  std::array<double,3> dimensions_m{1,1,1};
  friend bool operator==(const Primitive&,const Primitive&)=default;
};
Result<void> validate_primitive(const Primitive&) noexcept;
Result<void> validate_stamp(const Stamp&) noexcept;
class View {
  AssetId id_;
  Stamp stamp_;
  Digest revision_;
  Primitive primitive_;
  std::span<const std::byte> bytes_;
  View(AssetId,Stamp,Digest,Primitive,std::span<const std::byte>);
  friend Result<View> validate(std::span<const std::byte>);
public:
  AssetId id() const noexcept{return id_;}
  const Stamp& stamp() const noexcept{return stamp_;}
  Digest revision() const noexcept{return revision_;}
  const Primitive& primitive() const noexcept{return primitive_;}
  // Borrowed immutable bytes: the caller retains their owner through all use.
  std::span<const std::byte> bytes() const noexcept{return bytes_;}
};
Result<View> validate(std::span<const std::byte>);
// Fixed-size, allocation-free encoding; the returned array owns every byte.
Result<std::array<std::byte,product_bytes>> encode(AssetId,const Stamp&,const Primitive&) noexcept;
} // namespace race::assets::collision
