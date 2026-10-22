#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace race::render {
inline constexpr std::uint32_t viewport_texture_max_dimension = 1024;
inline constexpr std::uint32_t viewport_texture_max_mips = 11;
inline constexpr std::size_t viewport_max_textures = 16;
inline constexpr std::size_t viewport_texture_frame_bytes = 32 * 1024 * 1024;
// Two frame slots independently own these bounds. GPU allocation alignment and
// upload row padding count against their respective limits, not the texel limit.
inline constexpr std::size_t viewport_texture_slot_gpu_bytes = 64 * 1024 * 1024;
inline constexpr std::size_t viewport_texture_slot_upload_bytes = 64 * 1024 * 1024;
enum class TextureEncoding : std::uint32_t { SrgbColor=1, LinearData=2 };
enum class TextureRole : std::uint32_t { Color=1, DiffuseReflectanceR=2 };
enum class TextureSampling : std::uint32_t { LinearClampMip0=1, LinearClampMips=2 };
struct ViewportTextureMip {
  std::uint32_t width{},height{};
  std::span<const std::byte> pixels;
};
// Copied opaque RGBA8 with explicit encoding, tightly packed top-to-bottom rows. An explicit full
// chain stores successive max(1,dimension/2) levels through 1x1 without padding.
// No asset identity, file handle, mutable alias or GPU resource escapes here.
class ViewportTexture {
  std::uint32_t width_{}, height_{}, mip_count_{1};
  bool complete_mip_chain_{};
  TextureEncoding encoding_;
  std::vector<std::byte> texels_;
  ViewportTexture(std::uint32_t, std::uint32_t, std::uint32_t, bool, TextureEncoding, std::vector<std::byte>);
  static Result<std::shared_ptr<const ViewportTexture>> create_impl(
      std::uint32_t, std::uint32_t, std::span<const std::byte>, bool, TextureEncoding);
public:
  static Result<std::shared_ptr<const ViewportTexture>> create(
      std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba8);
  static Result<std::shared_ptr<const ViewportTexture>> create_mips(
      std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba8_chain);
  static Result<std::shared_ptr<const ViewportTexture>> create_scalar_mips(
      std::uint32_t width, std::uint32_t height, std::span<const std::byte> rgba8_chain);
  ViewportTexture(const ViewportTexture &) = delete;
  TextureEncoding encoding() const noexcept { return encoding_; }
  std::uint32_t width() const noexcept { return width_; }
  std::uint32_t height() const noexcept { return height_; }
  // This flag records explicit full-chain admission, including a 1x1 chain.
  bool complete_mip_chain() const noexcept { return complete_mip_chain_; }
  std::uint32_t mip_count() const noexcept { return mip_count_; }
  std::span<const std::byte> pixels() const noexcept {
    return std::span<const std::byte>(texels_).first(static_cast<std::size_t>(width_)*height_*4);
  }
  std::span<const std::byte> texels() const noexcept { return texels_; }
  std::optional<ViewportTextureMip> mip(std::uint32_t level) const noexcept;
};
// Partial opaque diffuse profile. Linear factor multiplies the draw's existing
// linear tint and an optional sRGB color or linear red-channel reflectance.
// Scalar reflectance requires explicit complete-chain mip sampling; G/B are ignored.
// Color sampling is explicit and
// defaults to legacy LINEAR CLAMP mip 0, even when the texture contains a chain.
class ViewportMaterial {
  std::array<float,4> base_color_;
  std::shared_ptr<const ViewportTexture> texture_;
  TextureSampling sampling_;
  TextureRole texture_role_;
  ViewportMaterial(std::array<float,4>, std::shared_ptr<const ViewportTexture>, TextureSampling, TextureRole);
public:
  static Result<std::shared_ptr<const ViewportMaterial>> create(
      std::array<float,4> base_color = {1,1,1,1},
      std::shared_ptr<const ViewportTexture> texture = {},
      TextureSampling sampling = TextureSampling::LinearClampMip0,
      TextureRole role = TextureRole::Color);
  ViewportMaterial(const ViewportMaterial &) = delete;
  const std::array<float,4>& base_color() const noexcept { return base_color_; }
  const std::shared_ptr<const ViewportTexture>& texture() const noexcept { return texture_; }
  TextureSampling sampling() const noexcept { return sampling_; }
  TextureRole texture_role() const noexcept { return texture_role_; }
};
} // namespace race::render
