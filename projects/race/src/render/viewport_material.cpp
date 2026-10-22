#include "race/render/viewport_material.hpp"
#include <algorithm>
#include <cmath>

namespace race::render {
ViewportTexture::ViewportTexture(std::uint32_t width, std::uint32_t height,
                                 std::uint32_t count, bool complete, TextureEncoding encoding, std::vector<std::byte> texels)
    : width_(width), height_(height), mip_count_(count), complete_mip_chain_(complete), encoding_(encoding), texels_(std::move(texels)) {}
Result<std::shared_ptr<const ViewportTexture>> ViewportTexture::create_impl(
    std::uint32_t width, std::uint32_t height, std::span<const std::byte> texels, bool complete, TextureEncoding encoding) {
  if (width > viewport_texture_max_dimension || height > viewport_texture_max_dimension)
    return fail(ErrorCode::Exhausted, "viewport texture dimension exceeds 1024");
  if (!width || !height)
    return fail(ErrorCode::InvalidInput, "viewport texture dimensions must be nonzero");
  std::size_t bytes=static_cast<std::size_t>(width)*height*4;
  std::uint32_t count=1;
  if (complete) for (auto w=width,h=height;w>1||h>1;++count) {
    w=std::max(1u,w/2); h=std::max(1u,h/2);
    // The dimension limit bounds the full sum to 5,592,404 bytes and 11 levels.
    bytes+=static_cast<std::size_t>(w)*h*4;
  }
  if (texels.size()!=bytes)
    return fail(ErrorCode::InvalidInput, "viewport texture requires exact RGBA8 level bytes");
  for (std::size_t i=3;i<texels.size();i+=4)
    if (texels[i]!=std::byte{255})
      return fail(ErrorCode::InvalidInput, "viewport texture must be opaque at every mip");
  try {
    return std::shared_ptr<const ViewportTexture>(new ViewportTexture(
        width,height,count,complete,encoding,{texels.begin(),texels.end()}));
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "viewport texture packet allocation"); }
}
Result<std::shared_ptr<const ViewportTexture>> ViewportTexture::create(
    std::uint32_t width, std::uint32_t height, std::span<const std::byte> pixels) {
  return create_impl(width,height,pixels,false,TextureEncoding::SrgbColor);
}
Result<std::shared_ptr<const ViewportTexture>> ViewportTexture::create_mips(
    std::uint32_t width, std::uint32_t height, std::span<const std::byte> texels) {
  return create_impl(width,height,texels,true,TextureEncoding::SrgbColor);
}
Result<std::shared_ptr<const ViewportTexture>> ViewportTexture::create_scalar_mips(
    std::uint32_t width, std::uint32_t height, std::span<const std::byte> texels) {
  return create_impl(width,height,texels,true,TextureEncoding::LinearData);
}
std::optional<ViewportTextureMip> ViewportTexture::mip(std::uint32_t level) const noexcept {
  if (level>=mip_count_) return std::nullopt;
  auto width=width_,height=height_; std::size_t offset{};
  for (std::uint32_t i=0;i<level;++i) {
    offset+=static_cast<std::size_t>(width)*height*4;
    width=std::max(1u,width/2); height=std::max(1u,height/2);
  }
  return ViewportTextureMip{width,height,std::span<const std::byte>(texels_).subspan(
      offset,static_cast<std::size_t>(width)*height*4)};
}
ViewportMaterial::ViewportMaterial(std::array<float,4> color,
                                   std::shared_ptr<const ViewportTexture> texture, TextureSampling sampling, TextureRole role)
    : base_color_(color), texture_(std::move(texture)), sampling_(sampling), texture_role_(role) {}
Result<std::shared_ptr<const ViewportMaterial>> ViewportMaterial::create(
    std::array<float,4> color, std::shared_ptr<const ViewportTexture> texture, TextureSampling sampling, TextureRole role) {
  if (color[3] != 1 || !std::all_of(color.begin(), color.end(), [](float value) {
        return std::isfinite(value) && value >= 0 && value <= 1;
      })) return fail(ErrorCode::InvalidInput, "viewport material requires opaque linear unit factor");
  if (sampling!=TextureSampling::LinearClampMip0 && sampling!=TextureSampling::LinearClampMips)
    return fail(ErrorCode::IncompatibleVersion, "unsupported viewport texture sampling");
  if (sampling==TextureSampling::LinearClampMips && (!texture || !texture->complete_mip_chain()))
    return fail(ErrorCode::InvalidInput, "mip sampling requires an explicitly complete texture chain");
  if (role!=TextureRole::Color && role!=TextureRole::DiffuseReflectanceR)
    return fail(ErrorCode::IncompatibleVersion, "unsupported viewport texture role");
  if (role==TextureRole::DiffuseReflectanceR && (!texture || texture->encoding()!=TextureEncoding::LinearData ||
      sampling!=TextureSampling::LinearClampMips || !texture->complete_mip_chain()))
    return fail(ErrorCode::InvalidInput, "scalar reflectance requires linear-data complete-chain mip sampling");
  if (role==TextureRole::Color && texture && texture->encoding()!=TextureEncoding::SrgbColor)
    return fail(ErrorCode::InvalidInput, "color material requires sRGB texture encoding");
  try {
    return std::shared_ptr<const ViewportMaterial>(new ViewportMaterial(color,std::move(texture),sampling,role));
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "viewport material packet allocation"); }
}
} // namespace race::render
