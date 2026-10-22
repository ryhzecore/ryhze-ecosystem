#include "project_index.hpp"
#include <algorithm>
#include <cstring>

namespace race::project_index {
namespace {
bool less(assets::AssetId a,assets::AssetId b){return a.high<b.high||(a.high==b.high&&a.low<b.low);}
bool leaf(std::string_view value){return platform::valid_content_leaf(value)&&value.ends_with(".json");}
std::string_view kind_name(Kind kind){
  switch(kind){case Kind::Mesh:return "mesh";case Kind::Texture:return "texture";case Kind::Material:return "material";}
  return {};
}
constexpr std::string_view magic="RACEPRJ1";
static_assert(binary_header_bytes+maximum_entries*binary_entry_bytes+binary_checksum_bytes<=maximum_bytes&&maximum_bytes<=UINT32_MAX);
bool binary_magic(std::span<const std::byte> bytes){
  return bytes.size()>=magic.size()&&std::memcmp(bytes.data(),magic.data(),magic.size())==0;
}
std::uint32_t u32(std::span<const std::byte> bytes,std::size_t at){
  std::uint32_t value{};for(unsigned n=0;n<4;++n)value|=std::to_integer<std::uint32_t>(bytes[at+n])<<(n*8);return value;
}
std::uint64_t u64(std::span<const std::byte> bytes,std::size_t at){
  std::uint64_t value{};for(unsigned n=0;n<8;++n)value|=std::to_integer<std::uint64_t>(bytes[at+n])<<(n*8);return value;
}
void put32(std::span<std::byte> bytes,std::size_t at,std::uint32_t value){
  for(unsigned n=0;n<4;++n)bytes[at+n]=std::byte((value>>(n*8))&255U);
}
void put64(std::span<std::byte> bytes,std::size_t at,std::uint64_t value){
  for(unsigned n=0;n<8;++n)bytes[at+n]=std::byte((value>>(n*8))&255U);
}
Result<void> validate(std::span<const View> entries){
  for(std::size_t n=0;n<entries.size();++n){
    const auto& entry=entries[n];
    if(!entry.id.valid()||kind_name(entry.kind).empty()||!leaf(entry.recipe)||entry.revision==assets::Digest{})
      return fail(ErrorCode::InvalidInput,"Invalid typed project recipe reference");
    for(std::size_t j=0;j<n;++j)if(entry.id==entries[j].id||entry.recipe==entries[j].recipe)
      return fail(ErrorCode::Duplicate,"Duplicate typed project identity or recipe leaf");
  }
  return {};
}
} // namespace
Result<std::size_t> decode(std::span<const std::byte> bytes,std::array<View,maximum_entries>& entries){
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Binary project index byte limit");
  if(!binary_magic(bytes))return fail(ErrorCode::IncompatibleVersion,"Project requires RACEPRJ1 binary encoding");
  if(bytes.size()<binary_header_bytes+binary_checksum_bytes)
    return fail(ErrorCode::InvalidInput,"Truncated binary project header or checksum");
  if(u32(bytes,8)!=binary_version)return fail(ErrorCode::IncompatibleVersion,"Unsupported binary project version");
  if(u32(bytes,12)!=binary_header_bytes||u32(bytes,24)!=binary_entry_bytes||u32(bytes,28)!=0)
    return fail(ErrorCode::InvalidInput,"Invalid binary project header");
  const auto count=u32(bytes,20);
  if(count>maximum_entries)return fail(ErrorCode::Exhausted,"Binary project recipe count limit");
  const auto payload=binary_header_bytes+std::size_t(count)*binary_entry_bytes;
  if(bytes.size()!=payload+binary_checksum_bytes||u32(bytes,16)!=bytes.size())
    return fail(ErrorCode::InvalidInput,"Binary project byte count mismatch");
  const auto checksum=assets::digest(bytes.first(payload));
  if(!std::equal(checksum.begin(),checksum.end(),bytes.begin()+payload))
    return fail(ErrorCode::InvalidInput,"Binary project checksum mismatch");
  for(std::size_t n=0;n<count;++n){
    const auto at=binary_header_bytes+n*binary_entry_bytes;
    const auto kind=u32(bytes,at+48),length=u32(bytes,at+52);
    // Check the full wire integer before narrowing the public enum.
    if(kind<1||kind>3||!length||length>96)
      return fail(ErrorCode::InvalidInput,"Invalid binary project kind or recipe length");
    const auto padding=bytes.subspan(at+56+length,96-length);
    if(std::any_of(padding.begin(),padding.end(),[](std::byte value){return value!=std::byte{};}))
      return fail(ErrorCode::InvalidInput,"Noncanonical binary project leaf padding");
    auto& entry=entries[n];entry.id={u64(bytes,at),u64(bytes,at+8)};entry.kind=static_cast<Kind>(kind);
    std::copy_n(bytes.begin()+at+16,entry.revision.size(),entry.revision.begin());
    entry.recipe={reinterpret_cast<const char*>(bytes.data()+at+56),length};
  }
  const auto views=std::span(entries).first(count);auto checked=validate(views);if(!checked)return checked.error();
  for(std::size_t n=1;n<views.size();++n)if(!less(views[n-1].id,views[n].id))
    return fail(ErrorCode::InvalidInput,"Binary project entries are not sorted");
  return count;
}
Result<std::vector<std::byte>> encode(std::span<View> entries){
  if(entries.size()>maximum_entries)return fail(ErrorCode::Exhausted,"Binary project recipe count limit");
  try{
    auto checked=validate(entries);if(!checked)return checked.error();
    std::sort(entries.begin(),entries.end(),[](const View& a,const View& b){return less(a.id,b.id);});
    const auto payload=binary_header_bytes+entries.size()*binary_entry_bytes;
    std::vector<std::byte> result(payload+binary_checksum_bytes); // One bounded allocation, including canonical zero padding.
    std::memcpy(result.data(),magic.data(),magic.size());put32(result,8,binary_version);put32(result,12,binary_header_bytes);
    put32(result,16,static_cast<std::uint32_t>(result.size()));put32(result,20,static_cast<std::uint32_t>(entries.size()));put32(result,24,binary_entry_bytes);
    for(std::size_t n=0;n<entries.size();++n){
      const auto at=binary_header_bytes+n*binary_entry_bytes;const auto& entry=entries[n];
      put64(result,at,entry.id.high);put64(result,at+8,entry.id.low);
      std::copy(entry.revision.begin(),entry.revision.end(),result.begin()+at+16);
      put32(result,at+48,static_cast<std::uint32_t>(entry.kind));put32(result,at+52,static_cast<std::uint32_t>(entry.recipe.size()));
      std::memcpy(result.data()+at+56,entry.recipe.data(),entry.recipe.size());
    }
    const auto checksum=assets::digest(std::span<const std::byte>(result).first(payload));
    std::copy(checksum.begin(),checksum.end(),result.begin()+payload);return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Binary project encoding allocation");}
}
bool valid_index_leaf(std::string_view value) noexcept{
  return platform::valid_content_leaf(value)&&value.ends_with(".raceproject");
}
} // namespace race::project_index
