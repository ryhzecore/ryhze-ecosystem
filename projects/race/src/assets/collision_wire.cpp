#include "race/assets/collision.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>

namespace race::assets::collision {
namespace {
static_assert(sizeof(double)==8&&std::numeric_limits<double>::is_iec559);
std::uint64_t get(std::span<const std::byte> bytes,std::size_t at,std::size_t count=8) noexcept {
  std::uint64_t value{};
  for(std::size_t i=0;i<count;++i)value|=std::uint64_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
  return value;
}
void put(std::span<std::byte> bytes,std::size_t at,std::uint64_t value,std::size_t count=8) noexcept {
  for(std::size_t i=0;i<count;++i)bytes[at+i]=std::byte((value>>(8*i))&255);
}
Digest hash_at(std::span<const std::byte> bytes,std::size_t at) noexcept {
  Digest value;std::copy_n(bytes.begin()+at,value.size(),value.begin());return value;
}
} // namespace
Result<void> validate_primitive(const Primitive& value) noexcept {
  if(value.kind!=Kind::Sphere&&value.kind!=Kind::Box)
    return fail(ErrorCode::IncompatibleVersion,"unknown collision primitive");
  const double maximum=value.kind==Kind::Sphere?10:100;
  for(const auto dimension:value.dimensions_m)
    if(!std::isfinite(dimension)||dimension<.1||dimension>maximum)
      return fail(ErrorCode::InvalidInput,"collision dimensions outside analytic profile");
  if(value.kind==Kind::Sphere&&(value.dimensions_m[0]!=value.dimensions_m[1]||value.dimensions_m[0]!=value.dimensions_m[2]))
    return fail(ErrorCode::Unsupported,"collision sphere requires equal diameters");
  return {};
}
Result<void> validate_stamp(const Stamp& stamp) noexcept {
  if(stamp.source==Digest{}||stamp.settings==Digest{}||stamp.importer==Digest{})
    return fail(ErrorCode::InvalidInput,"collision provenance fingerprint missing");
  if(stamp.dependencies!=digest({}))return fail(ErrorCode::InvalidDependency,"analytic collision has no asset dependencies");
  if(stamp.representation==Representation::Imported){
    if(stamp.target||stamp.cooker!=Digest{})return fail(ErrorCode::IncompatibleVersion,"imported collision has cooked metadata");
  }else if(stamp.representation==Representation::Cooked){
    if(stamp.target!=portable_cpu_target||stamp.cooker==Digest{})return fail(ErrorCode::IncompatibleVersion,"unsupported cooked collision target");
  }else return fail(ErrorCode::IncompatibleVersion,"unknown collision representation");
  return {};
}
View::View(AssetId id,Stamp stamp,Digest revision,Primitive primitive,std::span<const std::byte> bytes)
  :id_(id),stamp_(stamp),revision_(revision),primitive_(primitive),bytes_(bytes){}
Result<View> validate(std::span<const std::byte> bytes) {
  if(bytes.size()!=product_bytes||std::memcmp(bytes.data(),"RACEAST1",8))
    return fail(ErrorCode::InvalidInput,"collision signature or exact byte length");
  if(get(bytes,8,4)!=1||get(bytes,12,4)!=header_bytes||get(bytes,16,4)!=native_kind||get(bytes,20,4)!=schema||
      get(bytes,56)||get(bytes,244,4)!=geometry_profile||get(bytes,248,4)!=payload_bytes||get(bytes,252,4))
    return fail(ErrorCode::IncompatibleVersion,"unsupported collision container, schema, profile or features");
  if(get(bytes,32)!=product_bytes||get(bytes,40)!=header_bytes||get(bytes,48)!=payload_bytes)
    return fail(ErrorCode::InvalidInput,"collision payload bounds");
  AssetId id{get(bytes,64),get(bytes,72)};
  if(!id.valid())return fail(ErrorCode::InvalidInput,"collision asset ID is zero");
  Stamp stamp;stamp.representation=static_cast<Representation>(get(bytes,24,4));stamp.target=static_cast<std::uint32_t>(get(bytes,28,4));
  stamp.source=hash_at(bytes,80);stamp.settings=hash_at(bytes,112);stamp.importer=hash_at(bytes,144);
  stamp.cooker=hash_at(bytes,176);stamp.dependencies=hash_at(bytes,208);
  auto admitted=validate_stamp(stamp);if(!admitted)return admitted.error();
  if(digest(bytes.first(product_bytes-checksum_bytes))!=hash_at(bytes,product_bytes-checksum_bytes))
    return fail(ErrorCode::InvalidInput,"collision checksum mismatch");
  Primitive primitive;primitive.kind=static_cast<Kind>(get(bytes,240,4));
  for(std::size_t i=0;i<3;++i)primitive.dimensions_m[i]=std::bit_cast<double>(get(bytes,header_bytes+8*i));
  auto geometry=validate_primitive(primitive);if(!geometry)return geometry.error();
  return View(id,stamp,digest(bytes),primitive,bytes);
}
Result<std::array<std::byte,product_bytes>> encode(AssetId id,const Stamp& stamp,const Primitive& primitive) noexcept {
  if(!id.valid())return fail(ErrorCode::InvalidInput,"collision asset ID is zero");
  auto admitted=validate_stamp(stamp);if(!admitted)return admitted.error();
  auto geometry=validate_primitive(primitive);if(!geometry)return geometry.error();
  std::array<std::byte,product_bytes> bytes{};
  std::memcpy(bytes.data(),"RACEAST1",8);put(bytes,8,1,4);put(bytes,12,header_bytes,4);put(bytes,16,native_kind,4);put(bytes,20,schema,4);
  put(bytes,24,static_cast<std::uint32_t>(stamp.representation),4);put(bytes,28,stamp.target,4);
  put(bytes,32,product_bytes);put(bytes,40,header_bytes);put(bytes,48,payload_bytes);
  put(bytes,64,id.high);put(bytes,72,id.low);
  const std::array hashes{stamp.source,stamp.settings,stamp.importer,stamp.cooker,stamp.dependencies};
  for(std::size_t i=0;i<hashes.size();++i)std::copy(hashes[i].begin(),hashes[i].end(),bytes.begin()+80+i*32);
  put(bytes,240,static_cast<std::uint32_t>(primitive.kind),4);put(bytes,244,geometry_profile,4);put(bytes,248,payload_bytes,4);
  for(std::size_t i=0;i<3;++i)put(bytes,header_bytes+8*i,std::bit_cast<std::uint64_t>(primitive.dimensions_m[i]));
  const auto checksum=digest(std::span<const std::byte>(bytes).first(product_bytes-checksum_bytes));
  std::copy(checksum.begin(),checksum.end(),bytes.end()-checksum_bytes);
  return bytes;
}
} // namespace race::assets::collision
