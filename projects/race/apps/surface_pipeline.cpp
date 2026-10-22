#include "surface_pipeline.hpp"
#include "authoring_json.hpp"
#include "surface_srgb_lut.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace race::surface_pipeline {
namespace {
namespace s = assets::surface;
assets::Digest fingerprint(std::string_view text) { return assets::digest(std::as_bytes(std::span(text))); }
constexpr std::array<std::string_view,11> profile_names{
  "ppm-srgb","ppm-linear","ppm-srgb-mips","bmp-srgb","bmp-linear","bmp-srgb-mips","material","material-mips","ppm-scalar-mips","bmp-scalar-mips","material-scalar-mips"};
s::Stamp imported_stamp(std::span<const std::byte> source, bool material, s::Encoding encoding, s::Sampling sampling = s::Sampling::LinearClampMip0, s::TextureRole role = s::TextureRole::Color) {
  const auto profile=material?(role==s::TextureRole::DiffuseReflectanceR?Profile::MaterialScalarMips:
      sampling==s::Sampling::LinearClampMips?Profile::MaterialMips:Profile::Material):
      encoding==s::Encoding::SrgbColor?Profile::PpmSrgb:Profile::PpmLinear;
  auto stamp=profile_info(profile).value().imported;stamp.source=assets::digest(source);return stamp;
}
// Bounded BMP24 source profile. Parse bytes explicitly; native structs carry
// platform layout/alignment and are not a file format. No allocation precedes
// all header, extent, dimension and decoded-product checks below.
Result<std::vector<std::byte>> import_bmp(assets::AssetId id, std::span<const std::byte> source, s::Encoding encoding) {
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "Texture asset identity is zero");
  if (source.size() > source_bytes) return fail(ErrorCode::Exhausted, "BMP source byte limit");
  if (encoding != s::Encoding::SrgbColor && encoding != s::Encoding::LinearData)
    return fail(ErrorCode::Unsupported, "Unknown source texture encoding");
  if (source.size() < 54) return fail(ErrorCode::InvalidInput, "BMP header is truncated");
  const auto u16 = [&](std::size_t at) {
    return std::to_integer<std::uint32_t>(source[at]) | (std::to_integer<std::uint32_t>(source[at + 1]) << 8);
  };
  const auto u32 = [&](std::size_t at) { return u16(at) | (u16(at + 2) << 16); };
  if (u16(0) != 0x4d42) return fail(ErrorCode::Unsupported, "BMP signature must be BM");
  if (u16(6) || u16(8)) return fail(ErrorCode::InvalidInput, "BMP reserved fields must be zero");
  if (u32(2) != source.size()) return fail(ErrorCode::InvalidInput, "BMP declared file size differs from source");
  if (u32(14) != 40) return fail(ErrorCode::Unsupported, "BMP requires a 40-byte INFOHEADER");
  if (u32(10) != 54) return fail(ErrorCode::InvalidInput, "BMP pixel offset must be 54");
  if (u16(26) != 1) return fail(ErrorCode::InvalidInput, "BMP planes must be one");
  if (u16(28) != 24) return fail(ErrorCode::Unsupported, "BMP profile requires 24-bit pixels");
  if (u32(30) != 0) return fail(ErrorCode::Unsupported, "BMP profile requires BI_RGB");
  if (u32(46) || u32(50)) return fail(ErrorCode::Unsupported, "BMP profile does not admit color tables");
  const auto width = u32(18), stored_height = u32(22);
  if (!width || (width & 0x80000000U)) return fail(ErrorCode::InvalidInput, "BMP width must be positive");
  if (!stored_height) return fail(ErrorCode::InvalidInput, "BMP height must be nonzero");
  if (stored_height == 0x80000000U) return fail(ErrorCode::InvalidInput, "BMP height magnitude is invalid");
  const bool top_down = (stored_height & 0x80000000U) != 0;
  const auto height = top_down ? 0U - stored_height : stored_height;
  if (width > 1024 || height > 1024) return fail(ErrorCode::Exhausted, "Texture dimension limit is 1024");
  // Dimensions are bounded before multiplication. Wide arithmetic then bounds
  // both the complete source raster and the native output before narrowing.
  const auto stride = (static_cast<std::uint64_t>(width) * 3 + 3) & ~std::uint64_t{3};
  const auto raster_bytes = stride * height;
  const auto pixels = static_cast<std::uint64_t>(width) * height;
  if (raster_bytes != source.size() - 54) return fail(ErrorCode::InvalidInput, "BMP raster length mismatch or trailing data");
  if (u32(34) && u32(34) != raster_bytes) return fail(ErrorCode::InvalidInput, "BMP declared image size differs from raster");
  if (pixels > (s::Limits{}.product_bytes - s::header_bytes - s::checksum_bytes) / 4)
    return fail(ErrorCode::Exhausted, "BMP decoded texture byte limit");
  try {
    std::vector<std::byte> rgba(static_cast<std::size_t>(pixels * 4));
    for (std::uint32_t y = 0; y < height; ++y) {
      const auto input_row = static_cast<std::size_t>(54 + stride * (top_down ? y : height - 1 - y));
      const auto output_row = static_cast<std::size_t>(y) * width * 4;
      for (std::uint32_t x = 0; x < width; ++x) {
        const auto input = input_row + static_cast<std::size_t>(x) * 3;
        const auto output = output_row + static_cast<std::size_t>(x) * 4;
        rgba[output] = source[input + 2]; rgba[output + 1] = source[input + 1];
        rgba[output + 2] = source[input]; rgba[output + 3] = std::byte{255};
      }
    }
    auto stamp=profile_info(encoding==s::Encoding::SrgbColor?Profile::BmpSrgb:Profile::BmpLinear).value().imported;
    stamp.source=assets::digest(source);
    return s::encode_texture(id, stamp, width, height, encoding, rgba);
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "Texture import allocation"); }
}
// Integer area coverage uses target-grid units: a source texel spans dst_width
// units and a destination texel spans src_width units (likewise for height).
// Every odd edge participates; each output has <= 3x3 contributors. With the
// 1024 bound, weights sum to <= 2^20 and Q24 weighted sums fit in uint64_t.
template<bool Scalar>
void downsample(std::span<const std::byte> source, std::uint32_t width, std::uint32_t height,
                std::span<std::byte> destination) {
  const auto next_width = std::max(1U, width / 2), next_height = std::max(1U, height / 2);
  const std::uint64_t area = static_cast<std::uint64_t>(width) * height;
  for (std::uint32_t y = 0; y < next_height; ++y) {
    const auto top = y * height, bottom = (y + 1) * height;
    const auto first_y = top / next_height, last_y = (bottom + next_height - 1) / next_height;
    for (std::uint32_t x = 0; x < next_width; ++x) {
      const auto left = x * width, right = (x + 1) * width;
      const auto first_x = left / next_width, last_x = (right + next_width - 1) / next_width;
      std::array<std::uint64_t, 3> sum{};
      for (auto sy = first_y; sy < last_y; ++sy) {
        const auto wy = std::min(bottom, (sy + 1) * next_height) - std::max(top, sy * next_height);
        for (auto sx = first_x; sx < last_x; ++sx) {
          const auto wx = std::min(right, (sx + 1) * next_width) - std::max(left, sx * next_width);
          const auto weight = static_cast<std::uint64_t>(wx) * wy;
          const auto at = (static_cast<std::size_t>(sy) * width + sx) * 4;
          for (std::size_t c = 0; c < 3; ++c) {
            const auto code = std::to_integer<unsigned char>(source[at + c]);
            if constexpr (Scalar) sum[c] += code * weight;
            else sum[c] += srgb_q24::decode[code] * weight;
          }
        }
      }
      const auto at = (static_cast<std::size_t>(y) * next_width + x) * 4;
      for (std::size_t c = 0; c < 3; ++c) {
        const auto linear = (sum[c] + area / 2) / area;
        if constexpr (Scalar) destination[at + c] = static_cast<std::byte>(linear);
        else {
          const auto encoded = std::upper_bound(srgb_q24::thresholds.begin(), srgb_q24::thresholds.end(), linear) - srgb_q24::thresholds.begin();
          destination[at + c] = static_cast<std::byte>(encoded);
        }
      }
      destination[at + 3] = std::byte{255};
    }
  }
}
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
struct PpmReader {
  std::string_view text; std::size_t at{};
  Result<std::string_view> token() {
    for (;;) {
      while (at < text.size() && whitespace(text[at])) {
        if (at >= 4096) return fail(ErrorCode::Exhausted, "PPM header exceeds 4096 bytes");
        ++at;
      }
      if (at >= 4096) return fail(ErrorCode::Exhausted, "PPM header exceeds 4096 bytes");
      if (at == text.size() || text[at] != '#') break;
      while (at < text.size() && text[at] != '\n' && text[at] != '\r') {
        if (at >= 4096) return fail(ErrorCode::Exhausted, "PPM comment exceeds header budget");
        const auto c = static_cast<unsigned char>(text[at++]);
        if (c != '\t' && (c < 32 || c > 126)) return fail(ErrorCode::InvalidInput, "PPM comments require ASCII");
      }
    }
    const auto start = at;
    while (at < text.size() && !whitespace(text[at]) && text[at] != '#') {
      if (at >= 4096 || at - start >= 16) return fail(ErrorCode::Exhausted, "PPM header token limit");
      ++at;
    }
    if (start == at) return fail(ErrorCode::InvalidInput, "Missing PPM header token");
    return text.substr(start, at - start);
  }
  Result<std::uint32_t> integer() {
    auto value = token(); if (!value) return value.error();
    std::uint32_t result{};
    const auto parsed = std::from_chars(value.value().data(), value.value().data() + value.value().size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.value().data() + value.value().size())
      return fail(ErrorCode::InvalidInput, "PPM requires unsigned decimal dimensions and max value");
    return result;
  }
};
Result<double> scalar(authoring_json::Reader& reader) {
  reader.space(); const auto first = reader.offset;
  // Exact JSON number grammar; from_chars alone would accept non-JSON spellings.
  if (reader.offset < reader.text.size() && reader.text[reader.offset] == '-') ++reader.offset;
  const auto digits = reader.offset;
  while (reader.offset < reader.text.size() && reader.text[reader.offset] >= '0' && reader.text[reader.offset] <= '9') ++reader.offset;
  if (digits == reader.offset || (reader.offset - digits > 1 && reader.text[digits] == '0'))
    return fail(ErrorCode::InvalidInput, "Material requires strict JSON numbers");
  if (reader.offset < reader.text.size() && reader.text[reader.offset] == '.') {
    const auto fraction = ++reader.offset;
    while (reader.offset < reader.text.size() && reader.text[reader.offset] >= '0' && reader.text[reader.offset] <= '9') ++reader.offset;
    if (reader.offset == fraction) return fail(ErrorCode::InvalidInput, "Material fraction requires digits");
  }
  if (reader.offset < reader.text.size() && (reader.text[reader.offset] == 'e' || reader.text[reader.offset] == 'E')) {
    ++reader.offset;
    if (reader.offset < reader.text.size() && (reader.text[reader.offset] == '+' || reader.text[reader.offset] == '-')) ++reader.offset;
    const auto exponent = reader.offset;
    while (reader.offset < reader.text.size() && reader.text[reader.offset] >= '0' && reader.text[reader.offset] <= '9') ++reader.offset;
    if (reader.offset == exponent) return fail(ErrorCode::InvalidInput, "Material exponent requires digits");
  }
  if (reader.offset - first > 64) return fail(ErrorCode::Exhausted, "Material number token limit");
  double value{};
  const auto parsed = std::from_chars(reader.text.data() + first, reader.text.data() + reader.offset, value);
  if (parsed.ec != std::errc{} || parsed.ptr != reader.text.data() + reader.offset || !std::isfinite(value) || value < 0 || value > 1)
    return fail(ErrorCode::InvalidInput, "Material linear base color must be in [0,1]");
  return value == 0 ? 0.0 : value;
}
Result<std::optional<s::Reference>> reference(authoring_json::Reader& reader) {
  reader.space();
  if (reader.text.substr(reader.offset, 4) == "null") { reader.offset += 4; return std::optional<s::Reference>{}; }
  if (!reader.take('{')) return fail(ErrorCode::InvalidInput, "Material texture requires null or a pinned reference");
  s::Reference result; unsigned seen{};
  for (unsigned i = 0; i < 2; ++i) {
    if (i && !reader.take(',')) return fail(ErrorCode::InvalidInput, "Material texture reference is incomplete");
    auto key = reader.string(); if (!key) return key.error();
    const unsigned bit = key.value() == "asset" ? 1U : key.value() == "revision" ? 2U : 0U;
    if (!bit || (seen & bit) || !reader.take(':')) return fail(ErrorCode::InvalidInput, "Invalid material texture reference fields");
    seen |= bit; auto text = reader.string(); if (!text) return text.error();
    if (bit == 1) { auto id = assets::parse_id(text.value()); if (!id) return id.error(); result.asset = id.value(); }
    else { auto hash = authoring_json::hex(text.value()); if (!hash) return hash.error(); result.revision = hash.value(); }
  }
  if (seen != 3 || !reader.take('}') || !result.valid()) return fail(ErrorCode::InvalidInput, "Material texture pin is invalid");
  return std::optional<s::Reference>{result};
}
}
Result<ProfileInfo> profile_info(Profile profile) noexcept {
  const auto index=static_cast<unsigned>(profile);
  if(index<1||index>profile_names.size())return fail(ErrorCode::Unsupported,"Unknown surface recipe profile");
  // One immutable compiled table is shared by direct tools and strict recipes.
  // SHA-256 input strings are the existing wire provenance contract.
  static const auto profiles=[]() noexcept {
    std::array<ProfileInfo,11> result{};
    for(std::size_t n=0;n<result.size();++n){
      const auto profile=static_cast<Profile>(n+1);
      const bool scalar_material=profile==Profile::MaterialScalarMips;
      const bool scalar=scalar_material||profile==Profile::PpmScalarMips||profile==Profile::BmpScalarMips;
      auto& info=result[n];info.name=profile_names[n];
      info.material=scalar_material||profile==Profile::Material||profile==Profile::MaterialMips;
      info.mips=scalar||profile==Profile::PpmSrgbMips||profile==Profile::BmpSrgbMips||profile==Profile::MaterialMips;
      const bool bmp=profile==Profile::BmpSrgb||profile==Profile::BmpLinear||profile==Profile::BmpSrgbMips||profile==Profile::BmpScalarMips;
      const bool linear=scalar||profile==Profile::PpmLinear||profile==Profile::BmpLinear;
      info.source_extension=info.material?".json":bmp?".bmp":".ppm";
      info.encoding=linear?s::Encoding::LinearData:s::Encoding::SrgbColor;
      info.imported_schema=scalar_material?3:info.material&&info.mips?2:1;info.cooked_schema=scalar?3:info.mips?2:1;
      info.imported.importer=fingerprint(info.material?(scalar_material?"race-material-json-importer-3":info.mips?"race-material-json-importer-2":"race-material-json-importer-1"):
          bmp?"race-bmp-bgr24-info40-importer-1":"race-ppm-p6-importer-1");
      info.imported.settings=fingerprint(info.material?(scalar_material?
          "race-opaque-diffuse-3:linear-rgb;linear-clamp-mips;complete-cooked-linear-scalar-texture;diffuse-reflectance-r":info.mips?
          "race-opaque-diffuse-2:linear-rgb;linear-clamp-mips;complete-cooked-srgb-texture":
          "race-opaque-diffuse-1:linear-rgb;linear-clamp-mip0"):
          bmp?(linear?"race-bmp-1:rgba8;top-left;linear-data;opaque":"race-bmp-1:rgba8;top-left;srgb;opaque"):
          linear?"race-ppm-1:rgba8;top-left;linear-data;opaque":"race-ppm-1:rgba8;top-left;srgb;opaque");
      info.cooked=info.imported;info.cooked.representation=s::Representation::Cooked;info.cooked.target=s::portable_cpu_target;
      if(!info.material&&info.mips)info.cooked.settings=fingerprint(scalar?
          "race-rgba8-scalar-mips-1:top-left;opaque-linear-data;independent-rgb;area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-half-up":
          "race-rgba8-mips-1:top-left;opaque-srgb;q24-decode-nearest-half-up;area-overlap-integer;floor-half-to-1;recursive-rgba8;mean-q24-half-up;encode-midpoint-ceil");
      info.cooked.cooker=fingerprint(info.material?(scalar_material?
          "race-opaque-diffuse-portable-cpu-cooker-3:diffuse-reflectance-r;linear-clamp-mips":info.mips?
          "race-opaque-diffuse-portable-cpu-cooker-2:linear-clamp-mips":"race-opaque-diffuse-portable-cpu-cooker-1"):
          scalar?"race-rgba8-portable-cpu-scalar-mips-cooker-1":info.mips?"race-rgba8-portable-cpu-mips-cooker-1:srgb-q24-lut-1":"race-rgba8-portable-cpu-cooker-1");
    }
    return result;
  }();
  return profiles[index-1];
}
Result<Profile> parse_profile(std::string_view name) noexcept {
  const auto found=std::find(profile_names.begin(),profile_names.end(),name);
  if(found==profile_names.end())return fail(ErrorCode::Unsupported,"Unknown surface recipe profile");
  return static_cast<Profile>(found-profile_names.begin()+1);
}
Result<std::vector<std::byte>> import_ppm(assets::AssetId id, std::span<const std::byte> source, s::Encoding encoding) {
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "Texture asset identity is zero");
  if (source.empty()) return fail(ErrorCode::InvalidInput, "Empty PPM source");
  if (source.size() > source_bytes) return fail(ErrorCode::Exhausted, "PPM source byte limit");
  if (encoding != s::Encoding::SrgbColor && encoding != s::Encoding::LinearData)
    return fail(ErrorCode::Unsupported, "Unknown source texture encoding");
  PpmReader reader{{reinterpret_cast<const char*>(source.data()), source.size()}};
  auto magic = reader.token(); if (!magic) return magic.error();
  if (magic.value() != "P6") return fail(ErrorCode::Unsupported, "Only binary P6 PPM images are supported");
  auto width = reader.integer(); if (!width) return width.error();
  auto height = reader.integer(); if (!height) return height.error();
  auto maximum = reader.integer(); if (!maximum) return maximum.error();
  if (maximum.value() != 255) return fail(ErrorCode::Unsupported, "PPM profile requires max value 255");
  if (!width.value() || !height.value()) return fail(ErrorCode::InvalidInput, "Texture dimensions must be nonzero");
  if (width.value() > 1024 || height.value() > 1024) return fail(ErrorCode::Exhausted, "Texture dimension limit is 1024");
  if (reader.at == source.size() || !whitespace(reader.text[reader.at])) return fail(ErrorCode::InvalidInput, "Missing PPM raster separator");
  const auto separator = reader.text[reader.at++];
  if (separator == '\r' && reader.at < source.size() && reader.text[reader.at] == '\n') ++reader.at;
  const auto pixels = static_cast<std::size_t>(width.value()) * height.value();
  if (source.size() - reader.at != pixels * 3) return fail(ErrorCode::InvalidInput, "PPM raster length mismatch or trailing data");
  try {
    std::vector<std::byte> rgba(pixels * 4);
    for (std::size_t n = 0; n < pixels; ++n) {
      std::copy_n(source.begin() + reader.at + n * 3, 3, rgba.begin() + n * 4); rgba[n * 4 + 3] = std::byte{255};
    }
    return s::encode_texture(id, imported_stamp(source, false, encoding), width.value(), height.value(), encoding, rgba);
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "Texture import allocation"); }
}
Result<std::vector<std::byte>> import_texture(assets::AssetId id, std::span<const std::byte> source, s::Encoding encoding) {
  if (source.size() >= 2 && source[0] == std::byte{'B'} && source[1] == std::byte{'M'})
    return import_bmp(id, source, encoding);
  return import_ppm(id, source, encoding);
}
Result<std::vector<std::byte>> import_material(assets::AssetId id, std::span<const std::byte> source) {
  if (source.empty()) return fail(ErrorCode::InvalidInput, "Empty material source");
  if (source.size() > material_source_bytes) return fail(ErrorCode::Exhausted, "Material source byte limit");
  authoring_json::Reader reader{{reinterpret_cast<const char*>(source.data()), source.size()}};
  if (!reader.take('{')) return fail(ErrorCode::InvalidInput, "Material source requires one JSON object");
  s::Material material; unsigned seen{}; std::uint32_t schema{};
  for (unsigned i = 0; ; ++i) {
    if (i == 6) return fail(ErrorCode::InvalidInput, "Extra material fields");
    if (i && !reader.take(',')) return fail(ErrorCode::InvalidInput, "Material fields incomplete");
    auto key = reader.string(); if (!key) return key.error();
    const unsigned bit = key.value() == "schema" ? 1U : key.value() == "model" ? 2U : key.value() == "base_color" ? 4U : key.value() == "base_color_texture" ? 8U : key.value() == "sampling" ? 16U : key.value() == "texture_role" ? 32U : 0U;
    if (!bit || (seen & bit) || !reader.take(':')) return fail(ErrorCode::InvalidInput, "Unknown, duplicate or malformed material field");
    seen |= bit;
    if (bit == 1) {
      auto version = reader.integer(); if (!version) return version.error(); schema = version.value();
      if (schema != 1 && schema != 2 && schema != 3) return fail(ErrorCode::IncompatibleVersion, "Unsupported material authoring schema");
    }
    else if (bit == 2) { auto model = reader.string(); if (!model) return model.error(); if (model.value() != "opaque-diffuse") return fail(ErrorCode::Unsupported, "Unsupported material model"); }
    else if (bit == 4) {
      if (!reader.take('[')) return fail(ErrorCode::InvalidInput, "Material base color requires three numbers");
      for (std::size_t c = 0; c < 3; ++c) {
        if (c && !reader.take(',')) return fail(ErrorCode::InvalidInput, "Material base color requires three numbers");
        auto value = scalar(reader); if (!value) return value.error(); material.base_color[c] = value.value();
      }
      if (!reader.take(']')) return fail(ErrorCode::InvalidInput, "Material base color requires three numbers");
    } else if (bit == 8) { auto texture = reference(reader); if (!texture) return texture.error(); material.base_color_texture = texture.value(); }
    else if (bit == 16) {
      auto sampling = reader.string(); if (!sampling) return sampling.error();
      if (sampling.value() != "linear-clamp-mips") return fail(ErrorCode::Unsupported, "Unsupported material sampling profile");
      material.sampling = s::Sampling::LinearClampMips;
    } else {
      auto role = reader.string(); if (!role) return role.error();
      if (role.value() != "diffuse-reflectance-r") return fail(ErrorCode::Unsupported, "Unsupported material texture role");
      material.texture_role = s::TextureRole::DiffuseReflectanceR;
    }
    if (reader.take('}')) break;
  }
  if (seen != (schema == 1 ? 15U : schema == 2 ? 31U : 63U)) return fail(ErrorCode::InvalidInput, "Material fields incomplete or incompatible with schema");
  reader.space(); if (reader.offset != reader.text.size()) return fail(ErrorCode::InvalidInput, "Material trailing data");
  auto stamp = imported_stamp(source, true, s::Encoding::SrgbColor, material.sampling, material.texture_role);
  stamp.dependencies = s::dependency_digest(material.base_color_texture);
  return s::encode_material(id, stamp, material);
}
Result<std::vector<std::byte>> cook(std::span<const std::byte> bytes, platform::RootedFiles* dependencies) {
  auto texture = s::validate_texture(bytes);
  if (texture) {
    const auto& view = texture.value(); auto stamp = view.stamp();
    if (stamp.representation != s::Representation::Imported) return fail(ErrorCode::InvalidInput, "Cook requires an imported texture");
    stamp.representation = s::Representation::Cooked; stamp.target = 1; stamp.cooker = profile_info(Profile::PpmSrgb).value().cooked.cooker;
    return s::encode_texture(view.id(), stamp, view.width(), view.height(), view.encoding(), view.pixels());
  }
  auto material = s::validate_material(bytes); if (!material) return material.error();
  const auto& view = material.value(); auto stamp = view.stamp();
  if (stamp.representation != s::Representation::Imported) return fail(ErrorCode::InvalidInput, "Cook requires an imported material");
  if (view.material().base_color_texture) {
    if (!dependencies) return fail(ErrorCode::InvalidInput, "Material cooking requires its texture root");
    try {
      const auto leaf = assets::hex(view.material().base_color_texture->revision) + ".rasset";
      auto owned = dependencies->read(leaf, s::Limits{}.product_bytes); if (!owned) return owned.error();
      if (owned.value().capacity() > s::Limits{}.product_bytes) return fail(ErrorCode::Exhausted, "Material dependency buffer capacity limit");
      auto dependency = s::validate_texture(owned.value()); if (!dependency) return dependency.error();
      auto binding = s::validate_binding(view, dependency.value()); if (!binding) return binding.error();
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "Material dependency allocation"); }
  }
  stamp.representation = s::Representation::Cooked; stamp.target = 1;
  stamp.cooker = profile_info(view.material().texture_role == s::TextureRole::DiffuseReflectanceR ? Profile::MaterialScalarMips :
      view.material().sampling == s::Sampling::LinearClampMips ?
      Profile::MaterialMips : Profile::Material).value().cooked.cooker;
  return s::encode_material(view.id(), stamp, view.material());
}
namespace {
template<bool Scalar>
Result<std::vector<std::byte>> cook_mip_profile(std::span<const std::byte> bytes) {
  auto texture = s::validate_texture(bytes); if (!texture) return texture.error();
  const auto& view = texture.value();
  if (view.stamp().representation != s::Representation::Imported)
    return fail(ErrorCode::InvalidInput, "Mip cooking requires an imported texture");
  if (view.encoding() != (Scalar ? s::Encoding::LinearData : s::Encoding::SrgbColor))
    return fail(ErrorCode::IncompatibleVersion, "Mip cooking requires the explicitly selected source encoding");
  for (std::size_t i = 3; i < view.pixels().size(); i += 4) if (view.pixels()[i] != std::byte{255})
    return fail(ErrorCode::Unsupported, "Mip cooking requires opaque alpha");
  auto width = view.width(), height = view.height();
  std::size_t total = view.pixels().size();
  constexpr auto texel_limit = s::Limits{}.product_bytes - s::header_bytes - s::checksum_bytes;
  while (width != 1 || height != 1) {
    width = std::max(1U, width / 2); height = std::max(1U, height / 2);
    const auto level_bytes = static_cast<std::size_t>(width) * height * 4;
    if (level_bytes > texel_limit - total) return fail(ErrorCode::Exhausted, "Mip chain byte limit");
    total += level_bytes;
  }
  try {
    // One bounded chain scratch plus the encoded output; no per-level allocation.
    std::vector<std::byte> chain(total);
    std::copy(view.pixels().begin(), view.pixels().end(), chain.begin());
    width = view.width(); height = view.height(); std::size_t offset{};
    while (width != 1 || height != 1) {
      const auto level_bytes = static_cast<std::size_t>(width) * height * 4;
      const auto next_width = std::max(1U, width / 2), next_height = std::max(1U, height / 2);
      const auto next_bytes = static_cast<std::size_t>(next_width) * next_height * 4;
      downsample<Scalar>(std::span<const std::byte>(chain).subspan(offset, level_bytes), width, height,
                 std::span<std::byte>(chain).subspan(offset + level_bytes, next_bytes));
      offset += level_bytes; width = next_width; height = next_height;
    }
    auto stamp = view.stamp(); stamp.representation = s::Representation::Cooked; stamp.target = s::portable_cpu_target;
    const auto compiled=profile_info(Scalar?Profile::PpmScalarMips:Profile::PpmSrgbMips).value();
    stamp.settings = compiled.cooked.settings; stamp.cooker = compiled.cooked.cooker;
    if constexpr (Scalar) return s::encode_texture_scalar_mips(view.id(), stamp, view.width(), view.height(), view.encoding(), chain);
    else return s::encode_texture_mips(view.id(), stamp, view.width(), view.height(), view.encoding(), chain);
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "Texture mip cooking allocation"); }
}
} // namespace
Result<std::vector<std::byte>> cook_mips(std::span<const std::byte> bytes) { return cook_mip_profile<false>(bytes); }
Result<std::vector<std::byte>> cook_scalar_mips(std::span<const std::byte> bytes) { return cook_mip_profile<true>(bytes); }
Result<std::string> publish(platform::RootedFiles& files, std::span<const std::byte> bytes) {
  const auto texture = s::validate_texture(bytes);
  if (!texture) { auto material = s::validate_material(bytes); if (!material) return material.error(); }
  try {
    const auto leaf = assets::hex(assets::digest(bytes)) + ".rasset";
    auto written = files.publish_immutable(leaf, bytes); if (!written) return written.error();
    auto readback = files.read(leaf, s::Limits{}.product_bytes); if (!readback) return readback.error();
    if (readback.value().capacity() > s::Limits{}.product_bytes) return fail(ErrorCode::Exhausted, "Surface readback buffer capacity limit");
    if (!std::equal(bytes.begin(), bytes.end(), readback.value().begin(), readback.value().end()))
      return fail(ErrorCode::InvalidInput, "Published surface readback differs");
    return leaf;
  } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "Surface publication allocation"); }
}
} // namespace race::surface_pipeline
