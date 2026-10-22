#include "mesh_recipe.hpp"
#include <algorithm>
#include "authoring_json.hpp"

namespace race::mesh_recipe {
namespace {
constexpr std::array<std::string_view,10> keys{"schema","asset","source","mesh_container","mesh_schema","target","settings","importer","cooker","dependencies"};
const assets::mesh::Stamp& compiled_profile(mesh_pipeline::Profile profile){
  // Immutable hashes of the compiled profile, independent of source or recipe
  // input. Reuse only these constants; every incoming recipe is still validated.
  static const auto geometry=mesh_pipeline::cook_stamp(mesh_pipeline::import_stamp({}));
  static const auto uv=mesh_pipeline::cook_stamp(mesh_pipeline::profile_import_stamp(mesh_pipeline::Profile::GeometryUv,{}).value());
  return profile==mesh_pipeline::Profile::GeometryUv?uv:geometry;
}
bool valid_source(std::string_view source){return platform::valid_content_leaf(source)&&source.ends_with(".obj");}
using authoring_json::Reader;
using authoring_json::hex;
}
Result<std::string> encode(const Definition& recipe){
  auto admitted=mesh_pipeline::profile_import_stamp(recipe.profile,{});if(!admitted)return admitted.error();
  if(!recipe.id.valid()||!valid_source(recipe.source))return fail(ErrorCode::InvalidInput,"Recipe requires stable identity and canonical OBJ source");
  try{
    const auto& profile=compiled_profile(recipe.profile);
    const auto schema=recipe.profile==mesh_pipeline::Profile::GeometryUv?"2":"1";
    return std::string("{\"schema\":")+schema+",\"asset\":\""+assets::id_text(recipe.id)+"\",\"source\":\""+recipe.source+
      "\",\"mesh_container\":1,\"mesh_schema\":"+schema+",\"target\":1,\"settings\":\""+assets::hex(profile.settings)+
      "\",\"importer\":\""+assets::hex(profile.importer)+"\",\"cooker\":\""+assets::hex(profile.cooker)+
      "\",\"dependencies\":\""+assets::hex(profile.dependencies)+"\"}\n";
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Recipe encoding allocation");}
}
Result<Snapshot> decode(std::span<const std::byte> bytes){
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty recipe");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Recipe byte limit");
  try{
    Reader reader{{reinterpret_cast<const char*>(bytes.data()),bytes.size()}};
    if(!reader.take('{'))return fail(ErrorCode::InvalidInput,"Recipe requires one flat object");
    unsigned seen{};assets::AssetId id{};std::string_view source;
    // Store all fields before choosing the explicit profile: JSON field order
    // cannot change the tuple that is admitted, and hashes never select it.
    std::array<std::uint32_t,4> versions{};
    std::array<assets::Digest,4> fingerprints{};
    for(unsigned field=0;field<keys.size();++field){
      if(field&&!reader.take(','))return fail(ErrorCode::InvalidInput,"Recipe fields missing or malformed");
      auto key=reader.string();if(!key)return key.error();
      const auto found=std::find(keys.begin(),keys.end(),key.value());
      if(found==keys.end())return fail(ErrorCode::InvalidInput,"Unknown recipe field");
      const auto index=static_cast<unsigned>(found-keys.begin());const unsigned bit=1U<<index;
      if(seen&bit)return fail(ErrorCode::Duplicate,"Duplicate recipe field");seen|=bit;
      if(!reader.take(':'))return fail(ErrorCode::InvalidInput,"Recipe field separator missing");
      if(index==0||(index>=3&&index<=5)){
        auto number=reader.integer();if(!number)return number.error();
        versions[index==0?0:index-2]=number.value();
      }else{
        auto value=reader.string();if(!value)return value.error();
        if(index==1){auto parsed=assets::parse_id(value.value());if(!parsed)return parsed.error();if(!parsed.value().valid())return fail(ErrorCode::InvalidInput,"Recipe asset identity is zero");id=parsed.value();}
        else if(index==2){if(!valid_source(value.value()))return fail(ErrorCode::InvalidInput,"Recipe source is not a canonical OBJ leaf");source=value.value();}
        else{
          auto parsed=hex(value.value());if(!parsed)return parsed.error();
          fingerprints[index-6]=parsed.value();
        }
      }
    }
    if(seen!=1023||!reader.take('}'))return fail(ErrorCode::InvalidInput,"Recipe fields incomplete or extra");
    reader.space();if(reader.offset!=reader.text.size())return fail(ErrorCode::InvalidInput,"Recipe trailing data");
    if((versions[0]!=1&&versions[0]!=2)||versions[1]!=1||versions[2]!=versions[0]||versions[3]!=1)
      return fail(ErrorCode::IncompatibleVersion,"Unsupported recipe/container/schema/target tuple");
    const auto selected=versions[0]==2?mesh_pipeline::Profile::GeometryUv:mesh_pipeline::Profile::Geometry;
    const auto& profile=compiled_profile(selected);
    if(fingerprints!=std::array{profile.settings,profile.importer,profile.cooker,profile.dependencies})
      return fail(ErrorCode::IncompatibleVersion,"Recipe build fingerprint is unsupported");
    Snapshot result{{id,std::string(source),selected},assets::digest(bytes),{bytes.begin(),bytes.end()}};
    if(result.bytes.capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Recipe retained capacity limit");return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Recipe decoding allocation");}
}
Result<Snapshot> load(platform::RootedFiles& files,std::string_view leaf){
  if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(".json"))return fail(ErrorCode::InvalidInput,"Recipe requires a canonical JSON leaf");
  try{auto bytes=files.read(leaf,maximum_bytes);if(!bytes)return bytes.error();if(bytes.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Recipe read capacity limit");return decode(bytes.value());}
  catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Recipe read allocation");}
}
} // namespace race::mesh_recipe
