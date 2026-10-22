#include "race/assets/surface.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace race::assets::surface {
namespace {
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559);
std::uint64_t get(std::span<const std::byte> b,std::size_t at,std::size_t n=8) noexcept {
  std::uint64_t value{};for(std::size_t i=0;i<n;++i)value|=std::uint64_t(std::to_integer<unsigned char>(b[at+i]))<<(i*8);return value;
}
void put(std::span<std::byte> b,std::size_t at,std::uint64_t value,std::size_t n=8) noexcept {
  for(std::size_t i=0;i<n;++i)b[at+i]=std::byte((value>>(i*8))&255);
}
Digest hash_at(std::span<const std::byte> b,std::size_t at) noexcept {
  Digest value;std::copy_n(b.begin()+at,value.size(),value.begin());return value;
}
void key_put(std::span<std::byte> b,std::size_t at,const Reference& ref) noexcept {
  put(b,at,ref.asset.high);put(b,at+8,ref.asset.low);std::copy(ref.revision.begin(),ref.revision.end(),b.begin()+at+16);
}
Reference key_at(std::span<const std::byte> b,std::size_t at) noexcept {
  return {{get(b,at),get(b,at+8)},hash_at(b,at+16)};
}
bool known(Encoding encoding) noexcept{return encoding==Encoding::SrgbColor||encoding==Encoding::LinearData;}
Result<void> material_valid(const Material& value) noexcept {
  if(value.texture_role!=TextureRole::Color&&value.texture_role!=TextureRole::DiffuseReflectanceR)
    return fail(ErrorCode::IncompatibleVersion,"unsupported material texture role");
  if(value.texture_role==TextureRole::DiffuseReflectanceR&&value.sampling!=Sampling::LinearClampMips)
    return fail(ErrorCode::IncompatibleVersion,"scalar diffuse reflectance requires complete mip sampling");
  if(value.model!=Model::OpaqueDiffuse)return fail(ErrorCode::IncompatibleVersion,"unsupported material model");
  for(const auto factor:value.base_color)if(!std::isfinite(factor)||factor<0||factor>1)
    return fail(ErrorCode::InvalidInput,"material base colour must be finite linear RGB in [0,1]");
  if(value.base_color_texture&&!value.base_color_texture->valid())return fail(ErrorCode::InvalidInput,"material texture pin is incomplete");
  if(value.sampling!=Sampling::LinearClampMip0&&value.sampling!=Sampling::LinearClampMips)
    return fail(ErrorCode::IncompatibleVersion,"unsupported material sampling profile");
  if(value.sampling==Sampling::LinearClampMips&&!value.base_color_texture)
    return fail(ErrorCode::InvalidDependency,"mip-filtered material requires a texture pin");
  return {};
}
Result<Stamp> envelope(std::span<const std::byte> b,std::uint32_t kind,const Limits& limits) noexcept {
  auto admitted=validate_limits(limits);if(!admitted)return admitted.error();
  if(b.size()>limits.product_bytes)return fail(ErrorCode::Exhausted,"surface product byte limit");
  if(b.size()<header_bytes+checksum_bytes||std::memcmp(b.data(),"RACEAST1",8))return fail(ErrorCode::InvalidInput,"surface signature or size");
  if(get(b,8,4)!=1||get(b,12,4)!=header_bytes||get(b,16,4)!=kind||(get(b,20,4)!=1&&get(b,20,4)!=2&&get(b,20,4)!=3)||get(b,56))
    return fail(ErrorCode::IncompatibleVersion,"unsupported surface container, kind, schema or features");
  if(get(b,32)!=b.size()||get(b,40)!=header_bytes||get(b,48)!=b.size()-header_bytes-checksum_bytes)
    return fail(ErrorCode::InvalidInput,"surface payload bounds or trailing data");
  if(!AssetId{get(b,64),get(b,72)}.valid())return fail(ErrorCode::InvalidInput,"surface asset ID is zero");
  Stamp stamp;stamp.representation=static_cast<Representation>(get(b,24,4));stamp.target=static_cast<std::uint32_t>(get(b,28,4));
  stamp.source=hash_at(b,80);stamp.settings=hash_at(b,112);stamp.importer=hash_at(b,144);
  stamp.cooker=hash_at(b,176);stamp.dependencies=hash_at(b,208);
  auto valid=validate_stamp(stamp);if(!valid)return valid.error();
  if(digest(b.first(b.size()-checksum_bytes))!=hash_at(b,b.size()-checksum_bytes))return fail(ErrorCode::InvalidInput,"surface checksum mismatch");
  return stamp;
}
void header(std::span<std::byte> b,std::uint32_t kind,AssetId id,const Stamp& stamp,std::uint32_t schema=1) noexcept {
  std::memcpy(b.data(),"RACEAST1",8);put(b,8,1,4);put(b,12,header_bytes,4);put(b,16,kind,4);put(b,20,schema,4);
  put(b,24,static_cast<std::uint32_t>(stamp.representation),4);put(b,28,stamp.target,4);
  put(b,32,b.size());put(b,40,header_bytes);put(b,48,b.size()-header_bytes-checksum_bytes);
  put(b,64,id.high);put(b,72,id.low);
  const std::array hashes{stamp.source,stamp.settings,stamp.importer,stamp.cooker,stamp.dependencies};
  for(std::size_t i=0;i<hashes.size();++i)std::copy(hashes[i].begin(),hashes[i].end(),b.begin()+80+i*32);
}
void checksum(std::span<std::byte> b) noexcept {
  const auto value=digest(std::span<const std::byte>(b).first(b.size()-checksum_bytes));std::copy(value.begin(),value.end(),b.end()-checksum_bytes);
}
struct TextureShape {std::size_t bytes{};std::uint32_t levels{};};
Result<TextureShape> texture_shape(std::uint32_t width,std::uint32_t height,Encoding encoding,const Limits& limits,bool mips) noexcept {
  if(!width||!height)return fail(ErrorCode::InvalidInput,"texture dimensions must be nonzero");
  if(width>limits.dimension||height>limits.dimension)return fail(ErrorCode::Exhausted,"texture dimension limit");
  if(!known(encoding))return fail(ErrorCode::IncompatibleVersion,"unsupported texture encoding");
  TextureShape result{header_bytes+checksum_bytes,0};
  // Dimensions are at most 1024. Per-level products fit even on 32-bit hosts,
  // and admission precedes each addition. Complete chains contain at most 11 levels.
  for(;;){
    const auto level_bytes=static_cast<std::size_t>(width)*height*4;
    if(level_bytes>limits.product_bytes-result.bytes)return fail(ErrorCode::Exhausted,"texture product byte limit");
    result.bytes+=level_bytes;++result.levels;
    if(!mips||(width==1&&height==1))break;
    width=std::max(1U,width/2);height=std::max(1U,height/2);
  }
  return result;
}
Result<void> mip_profile(const Stamp& stamp,Encoding encoding,std::span<const std::byte> texels,std::uint32_t schema) noexcept {
  if(stamp.representation!=Representation::Cooked||encoding!=(schema==3?Encoding::LinearData:Encoding::SrgbColor))
    return fail(ErrorCode::IncompatibleVersion,"complete texture mip profile requires its cooked encoding");
  for(std::size_t i=3;i<texels.size();i+=4)if(texels[i]!=std::byte{255})
    return fail(ErrorCode::Unsupported,"complete texture mip profile requires opaque alpha");
  return {};
}
} // namespace
bool Reference::valid() const noexcept{return asset.valid()&&revision!=Digest{};}
Result<void> validate_limits(const Limits& limits) noexcept {
  if(!limits.dimension||limits.dimension>1024||limits.product_bytes<header_bytes+4+checksum_bytes||limits.product_bytes>8*1024*1024)
    return fail(ErrorCode::InvalidInput,"invalid surface limits");return {};
}
Result<void> validate_stamp(const Stamp& stamp) noexcept {
  if(stamp.source==Digest{}||stamp.settings==Digest{}||stamp.importer==Digest{})return fail(ErrorCode::InvalidInput,"surface provenance fingerprint missing");
  if(stamp.representation==Representation::Imported){
    if(stamp.target||stamp.cooker!=Digest{})return fail(ErrorCode::IncompatibleVersion,"imported surface has cooked target or cooker");
  }else if(stamp.representation==Representation::Cooked){
    if(stamp.target!=portable_cpu_target||stamp.cooker==Digest{})return fail(ErrorCode::IncompatibleVersion,"unsupported cooked surface profile");
  }else return fail(ErrorCode::IncompatibleVersion,"unknown surface representation");
  return {};
}
Digest dependency_digest(const std::optional<Reference>& ref) noexcept {
  if(!ref)return digest({});std::array<std::byte,48> key{};key_put(key,0,*ref);return digest(key);
}
TextureView::TextureView(AssetId id,Stamp stamp,Digest revision,std::uint32_t width,std::uint32_t height,Encoding encoding,
    std::uint32_t schema,std::uint32_t mip_count,std::span<const std::byte> bytes)
    :id_(id),stamp_(stamp),revision_(revision),width_(width),height_(height),schema_(schema),mip_count_(mip_count),encoding_(encoding),bytes_(bytes){}
std::optional<TextureMip> TextureView::mip(std::size_t level) const noexcept {
  if(level>=mip_count_)return {};
  auto width=width_,height=height_;std::size_t offset=header_bytes;
  for(std::size_t n=0;n<level;++n){offset+=static_cast<std::size_t>(width)*height*4;width=std::max(1U,width/2);height=std::max(1U,height/2);}
  return TextureMip{width,height,bytes_.subspan(offset,static_cast<std::size_t>(width)*height*4)};
}
MaterialView::MaterialView(AssetId id,Stamp stamp,Digest revision,Material material,std::uint32_t schema,std::span<const std::byte> bytes)
    :id_(id),stamp_(stamp),revision_(revision),material_(material),schema_(schema),bytes_(bytes){}
Result<TextureView> validate_texture(std::span<const std::byte> bytes,const Limits& limits) {
  auto stamp=envelope(bytes,2,limits);if(!stamp)return stamp.error();
  const auto width=static_cast<std::uint32_t>(get(bytes,240,4)),height=static_cast<std::uint32_t>(get(bytes,244,4));
  const auto schema=static_cast<std::uint32_t>(get(bytes,20,4));
  const auto encoding=static_cast<Encoding>(get(bytes,248,4));
  auto shape=texture_shape(width,height,encoding,limits,schema!=1);if(!shape)return shape.error();
  if(get(bytes,252,4)!=shape.value().levels)return fail(ErrorCode::IncompatibleVersion,"texture mip count does not match its complete profile");
  if(bytes.size()!=shape.value().bytes)return fail(ErrorCode::InvalidInput,"texture raster or mip-chain length mismatch");
  if(stamp.value().dependencies!=digest({}))return fail(ErrorCode::InvalidDependency,"texture profile has no dependencies");
  if(schema!=1){auto valid=mip_profile(stamp.value(),encoding,bytes.subspan(header_bytes,bytes.size()-header_bytes-checksum_bytes),schema);if(!valid)return valid.error();}
  return TextureView({get(bytes,64),get(bytes,72)},stamp.value(),digest(bytes),width,height,encoding,schema,shape.value().levels,bytes);
}
Result<MaterialView> validate_material(std::span<const std::byte> bytes,const Limits& limits) {
  auto stamp=envelope(bytes,3,limits);if(!stamp)return stamp.error();
  if(bytes.size()!=header_bytes+material_payload_bytes+checksum_bytes)return fail(ErrorCode::InvalidInput,"material payload length mismatch");
  const auto count=get(bytes,244,4);
  const auto schema=static_cast<std::uint32_t>(get(bytes,20,4));
  if(count>1||get(bytes,248,4)!=(schema==1?1U:2U)||get(bytes,252,4)!=(schema==3?2U:0U))
    return fail(ErrorCode::IncompatibleVersion,"unsupported material binding, sampler or texture role");
  Material material;material.model=static_cast<Model>(get(bytes,240,4));material.sampling=static_cast<Sampling>(get(bytes,248,4));
  material.texture_role=schema==3?TextureRole::DiffuseReflectanceR:TextureRole::Color;
  for(std::size_t i=0;i<3;++i)material.base_color[i]=std::bit_cast<double>(get(bytes,header_bytes+i*8));
  const auto ref=key_at(bytes,header_bytes+24);
  if(count)material.base_color_texture=ref;
  else if(ref.asset.valid()||ref.revision!=Digest{})return fail(ErrorCode::InvalidInput,"absent material binding must be all zero");
  auto valid=material_valid(material);if(!valid)return valid.error();
  if(stamp.value().dependencies!=dependency_digest(material.base_color_texture))return fail(ErrorCode::InvalidDependency,"material dependency fingerprint mismatch");
  return MaterialView({get(bytes,64),get(bytes,72)},stamp.value(),digest(bytes),material,schema,bytes);
}
namespace {
Result<std::vector<std::byte>> encode_texture_profile(AssetId id,const Stamp& stamp,std::uint32_t width,std::uint32_t height,Encoding encoding,
    std::span<const std::byte> texels,const Limits& limits,std::uint32_t schema) {
  auto admitted=validate_limits(limits);if(!admitted)return admitted.error();
  auto valid=validate_stamp(stamp);if(!valid)return valid.error();
  if(!id.valid())return fail(ErrorCode::InvalidInput,"texture asset ID is zero");
  if(stamp.dependencies!=digest({}))return fail(ErrorCode::InvalidDependency,"texture profile has no dependencies");
  auto shape=texture_shape(width,height,encoding,limits,schema!=1);if(!shape)return shape.error();
  if(texels.size()!=shape.value().bytes-header_bytes-checksum_bytes)return fail(ErrorCode::InvalidInput,"texture input raster or mip-chain length mismatch");
  if(schema!=1){auto profile=mip_profile(stamp,encoding,texels,schema);if(!profile)return profile.error();}
  try{
    std::vector<std::byte> bytes(shape.value().bytes);header(bytes,2,id,stamp,schema);
    put(bytes,240,width,4);put(bytes,244,height,4);put(bytes,248,static_cast<std::uint32_t>(encoding),4);put(bytes,252,shape.value().levels,4);
    std::copy(texels.begin(),texels.end(),bytes.begin()+header_bytes);checksum(bytes);return bytes;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"texture encode allocation");}
}
} // namespace
Result<std::vector<std::byte>> encode_texture(AssetId id,const Stamp& stamp,std::uint32_t width,std::uint32_t height,Encoding encoding,
    std::span<const std::byte> pixels,const Limits& limits) {
  return encode_texture_profile(id,stamp,width,height,encoding,pixels,limits,1);
}
Result<std::vector<std::byte>> encode_texture_mips(AssetId id,const Stamp& stamp,std::uint32_t width,std::uint32_t height,Encoding encoding,
    std::span<const std::byte> texels,const Limits& limits) {
  return encode_texture_profile(id,stamp,width,height,encoding,texels,limits,2);
}
Result<std::vector<std::byte>> encode_texture_scalar_mips(AssetId id,const Stamp& stamp,std::uint32_t width,std::uint32_t height,Encoding encoding,
    std::span<const std::byte> texels,const Limits& limits) {
  return encode_texture_profile(id,stamp,width,height,encoding,texels,limits,3);
}
Result<std::vector<std::byte>> encode_material(AssetId id,const Stamp& stamp,const Material& material,const Limits& limits) {
  auto admitted=validate_limits(limits);if(!admitted)return admitted.error();
  auto valid=validate_stamp(stamp);if(!valid)return valid.error();
  if(!id.valid())return fail(ErrorCode::InvalidInput,"material asset ID is zero");
  auto semantic=material_valid(material);if(!semantic)return semantic.error();
  if(stamp.dependencies!=dependency_digest(material.base_color_texture))return fail(ErrorCode::InvalidDependency,"material dependency fingerprint mismatch");
  constexpr auto size=header_bytes+material_payload_bytes+checksum_bytes;
  if(size>limits.product_bytes)return fail(ErrorCode::Exhausted,"material product byte limit");
  try{
    const auto schema=material.texture_role==TextureRole::DiffuseReflectanceR?3U:material.sampling==Sampling::LinearClampMips?2U:1U;
    std::vector<std::byte> bytes(size);header(bytes,3,id,stamp,schema);
    put(bytes,240,static_cast<std::uint32_t>(material.model),4);put(bytes,244,material.base_color_texture?1:0,4);
    put(bytes,248,static_cast<std::uint32_t>(material.sampling),4);
    if(schema==3)put(bytes,252,static_cast<std::uint32_t>(material.texture_role),4);
    for(std::size_t i=0;i<3;++i)put(bytes,header_bytes+i*8,std::bit_cast<std::uint64_t>(material.base_color[i]));
    if(material.base_color_texture)key_put(bytes,header_bytes+24,*material.base_color_texture);
    checksum(bytes);return bytes;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"material encode allocation");}
}
Result<void> validate_binding(const MaterialView& material,const TextureView& texture) noexcept {
  const auto& pin=material.material().base_color_texture;
  if(!pin||pin->asset!=texture.id()||pin->revision!=texture.revision())return fail(ErrorCode::InvalidDependency,"material texture ID or full revision mismatch");
  const bool scalar=material.material().texture_role==TextureRole::DiffuseReflectanceR;
  if(texture.stamp().representation!=Representation::Cooked||texture.encoding()!=(scalar?Encoding::LinearData:Encoding::SrgbColor))
    return fail(ErrorCode::IncompatibleVersion,"material requires the cooked encoding declared by its texture role");
  if(material.material().sampling==Sampling::LinearClampMips&&texture.schema()!=(scalar?3U:2U))
    return fail(ErrorCode::IncompatibleVersion,"mip-filtered material requires its exact complete texture schema");
  const auto pixels=texture.texels();for(std::size_t i=3;i<pixels.size();i+=4)if(pixels[i]!=std::byte{255})
    return fail(ErrorCode::Unsupported,"opaque diffuse texture alpha must be 255");
  return {};
}
} // namespace race::assets::surface
