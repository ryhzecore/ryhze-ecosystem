#include "surface_recipe.hpp"
#include "authoring_json.hpp"
#include <algorithm>

namespace race::surface_recipe {
namespace {
namespace s=assets::surface;
constexpr std::array<std::string_view,14> keys{
  "schema","asset","source","profile","asset_container","asset_schema","target",
  "import_settings","settings","importer","cooker","dependencies","texture_asset","texture_revision"};
Result<surface_pipeline::ProfileInfo> admitted(const Definition& definition){
  auto info=surface_pipeline::profile_info(definition.profile);if(!info)return info.error();
  if(!definition.id.valid()||!platform::valid_content_leaf(definition.source)||!definition.source.ends_with(info.value().source_extension))
    return fail(ErrorCode::InvalidInput,"Surface recipe requires stable identity and canonical profile source");
  if(definition.texture&&!definition.texture->valid())return fail(ErrorCode::InvalidInput,"Surface recipe texture key is incomplete");
  if((!info.value().material&&definition.texture)||(info.value().material&&info.value().mips&&!definition.texture))
    return fail(ErrorCode::InvalidInput,"Surface recipe texture key is incompatible with profile");
  return info;
}
Result<void> product(const Definition& definition,const surface_pipeline::ProfileInfo& info,
    assets::Digest source,std::span<const std::byte> bytes,bool cooked){
  auto expected=cooked?info.cooked:info.imported;expected.source=source;
  expected.dependencies=s::dependency_digest(definition.texture);
  const auto schema=cooked?info.cooked_schema:info.imported_schema;
  if(info.material){
    auto view=s::validate_material(bytes);if(!view)return view.error();
    if(view.value().id()!=definition.id||view.value().schema()!=schema||view.value().stamp()!=expected||
        view.value().material().base_color_texture!=definition.texture||
        view.value().material().texture_role!=(definition.profile==Profile::MaterialScalarMips?s::TextureRole::DiffuseReflectanceR:s::TextureRole::Color)||
        view.value().material().sampling!=(info.mips?s::Sampling::LinearClampMips:s::Sampling::LinearClampMip0))
      return fail(ErrorCode::InvalidDependency,"Material product differs from exact recipe profile or dependency");
  }else{
    auto view=s::validate_texture(bytes);if(!view)return view.error();
    if(view.value().id()!=definition.id||view.value().schema()!=schema||view.value().stamp()!=expected||view.value().encoding()!=info.encoding)
      return fail(ErrorCode::InvalidDependency,"Texture product differs from exact recipe profile");
  }
  return {};
}
}
Result<std::string> encode(const Definition& definition){
  auto checked=admitted(definition);if(!checked)return checked.error();const auto& info=checked.value();
  try{
    const auto texture=definition.texture.value_or(s::Reference{});
    std::string result="{\"schema\":1,\"asset\":\""+assets::id_text(definition.id)+"\",\"source\":\""+definition.source+
      "\",\"profile\":\""+std::string(info.name)+"\",\"asset_container\":1,\"asset_schema\":"+std::to_string(info.cooked_schema)+
      ",\"target\":1,\"import_settings\":\""+assets::hex(info.imported.settings)+"\",\"settings\":\""+assets::hex(info.cooked.settings)+
      "\",\"importer\":\""+assets::hex(info.imported.importer)+"\",\"cooker\":\""+assets::hex(info.cooked.cooker)+
      "\",\"dependencies\":\""+assets::hex(s::dependency_digest(definition.texture))+"\",\"texture_asset\":\""+assets::id_text(texture.asset)+
      "\",\"texture_revision\":\""+assets::hex(texture.revision)+"\"}\n";
    if(result.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface recipe encoded byte limit");return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface recipe encoding allocation");}
}
Result<Snapshot> decode(std::span<const std::byte> bytes){
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty surface recipe");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface recipe byte limit");
  try{
    authoring_json::Reader reader{{reinterpret_cast<const char*>(bytes.data()),bytes.size()}};
    if(!reader.take('{'))return fail(ErrorCode::InvalidInput,"Surface recipe requires one flat object");
    Definition definition{};std::array<std::uint32_t,4> versions{};std::array<assets::Digest,6> hashes{};
    assets::AssetId texture{};unsigned seen{};
    for(unsigned field=0;field<keys.size();++field){
      if(field&&!reader.take(','))return fail(ErrorCode::InvalidInput,"Surface recipe fields missing or malformed");
      auto key=reader.string();if(!key)return key.error();const auto found=std::find(keys.begin(),keys.end(),key.value());
      if(found==keys.end())return fail(ErrorCode::InvalidInput,"Unknown surface recipe field");
      const auto index=static_cast<unsigned>(found-keys.begin()),bit=1U<<index;
      if(seen&bit)return fail(ErrorCode::Duplicate,"Duplicate surface recipe field");seen|=bit;
      if(!reader.take(':'))return fail(ErrorCode::InvalidInput,"Surface recipe field separator missing");
      if(index==0||(index>=4&&index<=6)){
        auto value=reader.integer();if(!value)return value.error();versions[index==0?0:index-3]=value.value();
      }else{
        auto value=reader.string();if(!value)return value.error();
        if(index==1||index==12){auto id=assets::parse_id(value.value());if(!id)return id.error();(index==1?definition.id:texture)=id.value();}
        else if(index==2)definition.source=value.value();
        else if(index==3){auto profile=surface_pipeline::parse_profile(value.value());if(!profile)return profile.error();definition.profile=profile.value();}
        else{auto digest=authoring_json::hex(value.value());if(!digest)return digest.error();hashes[index==13?5:index-7]=digest.value();}
      }
    }
    if(seen!=16383||!reader.take('}'))return fail(ErrorCode::InvalidInput,"Surface recipe fields incomplete or extra");
    reader.space();if(reader.offset!=reader.text.size())return fail(ErrorCode::InvalidInput,"Surface recipe trailing data");
    if(texture.valid()||hashes[5]!=assets::Digest{})definition.texture=s::Reference{texture,hashes[5]};
    auto checked=admitted(definition);if(!checked)return checked.error();const auto& info=checked.value();
    if(versions!=std::array<std::uint32_t,4>{1,1,info.cooked_schema,1})
      return fail(ErrorCode::IncompatibleVersion,"Unsupported surface recipe/container/schema/target tuple");
    const std::array expected{info.imported.settings,info.cooked.settings,info.imported.importer,info.cooked.cooker,s::dependency_digest(definition.texture)};
    if(!std::equal(expected.begin(),expected.end(),hashes.begin()))
      return fail(ErrorCode::IncompatibleVersion,"Surface recipe build fingerprint or dependency is unsupported");
    Snapshot result{std::move(definition),assets::digest(bytes),{bytes.begin(),bytes.end()}};
    if(result.bytes.capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface recipe retained capacity limit");return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface recipe decoding allocation");}
}
Result<Snapshot> load(platform::RootedFiles& files,std::string_view leaf){
  if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(".json"))return fail(ErrorCode::InvalidInput,"Surface recipe requires a canonical JSON leaf");
  try{
    auto bytes=files.read(leaf,maximum_bytes);if(!bytes)return bytes.error();
    if(bytes.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface recipe read capacity limit");return decode(bytes.value());
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface recipe read allocation");}
}
Result<Built> build(const Snapshot& snapshot,std::span<const std::byte> source,platform::RootedFiles* dependencies){
  try{
    if(snapshot.bytes.capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface recipe snapshot capacity limit");
    auto decoded=decode(snapshot.bytes);if(!decoded)return decoded.error();
    if(decoded.value().revision!=snapshot.revision||decoded.value().definition!=snapshot.definition)
      return fail(ErrorCode::InvalidDependency,"Surface recipe snapshot differs from exact admitted bytes");
    const auto& definition=decoded.value().definition;const auto info=surface_pipeline::profile_info(definition.profile).value();
    if(source.empty())return fail(ErrorCode::InvalidInput,"Empty surface recipe source");
    if(source.size()>(info.material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes))
      return fail(ErrorCode::Exhausted,"Surface recipe source byte limit");
    const bool bmp=info.source_extension==".bmp";
    const bool bmp_header=source.size()>=2&&source[0]==std::byte{'B'}&&source[1]==std::byte{'M'};
    if(!info.material&&bmp!=bmp_header)return fail(ErrorCode::IncompatibleVersion,"Surface source signature differs from explicit recipe profile");
    auto imported=info.material?surface_pipeline::import_material(definition.id,source):
        bmp?surface_pipeline::import_texture(definition.id,source,info.encoding):surface_pipeline::import_ppm(definition.id,source,info.encoding);
    if(!imported)return imported.error();
    if(imported.value().capacity()>s::Limits{}.product_bytes)return fail(ErrorCode::Exhausted,"Surface imported product capacity limit");
    const auto source_revision=assets::digest(source);
    auto admitted_import=product(definition,info,source_revision,imported.value(),false);if(!admitted_import)return admitted_import.error();
    auto cooked=!info.material&&info.mips?(info.encoding==s::Encoding::LinearData?surface_pipeline::cook_scalar_mips(imported.value()):surface_pipeline::cook_mips(imported.value())):surface_pipeline::cook(imported.value(),dependencies);
    if(!cooked)return cooked.error();
    if(cooked.value().capacity()>s::Limits{}.product_bytes)return fail(ErrorCode::Exhausted,"Surface cooked product capacity limit");
    auto admitted_cook=product(definition,info,source_revision,cooked.value(),true);if(!admitted_cook)return admitted_cook.error();
    return Built{std::move(imported).value(),std::move(cooked).value()};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface recipe build allocation");}
}
} // namespace race::surface_recipe
