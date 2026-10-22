#include "content_project.hpp"
#include <algorithm>

namespace race::content_project {
namespace {
bool less(assets::AssetId a,assets::AssetId b){return a.high<b.high||(a.high==b.high&&a.low<b.low);}
Result<Recipe> resolve(const project_index::View& ref,platform::RootedFiles& files){
  if(ref.kind==Kind::Mesh){
    auto recipe=mesh_recipe::load(files,ref.recipe);if(!recipe)return recipe.error();
    return Recipe{std::move(recipe).value()};
  }
  auto recipe=surface_recipe::load(files,ref.recipe);if(!recipe)return recipe.error();
  auto profile=surface_pipeline::profile_info(recipe.value().definition.profile);if(!profile)return profile.error();
  if(profile.value().material!=(ref.kind==Kind::Material))
    return fail(ErrorCode::InvalidDependency,"Project kind differs from the surface recipe profile");
  return Recipe{std::move(recipe).value()};
}
}
bool valid_new_index_leaf(std::string_view value) noexcept{return valid_index_leaf(value);}
Result<std::vector<std::byte>> encode_binary(std::span<const Reference> references){
  if(references.size()>maximum_entries)return fail(ErrorCode::Exhausted,"Binary project recipe count limit");
  std::array<project_index::View,maximum_entries> views{};
  for(std::size_t n=0;n<references.size();++n)views[n]={references[n].id,references[n].kind,references[n].recipe,references[n].revision};
  return project_index::encode(std::span(views).first(references.size()));
}
Result<Snapshot> Snapshot::open(std::span<const std::byte> bytes,platform::RootedFiles& files,std::size_t limit){
  if(!limit||limit>maximum_retained_bytes)return fail(ErrorCode::InvalidInput,"Typed project retained limit outside profile");
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty typed project index");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Typed project index byte limit");
  try{
    std::array<project_index::View,maximum_entries> references{};
    auto parsed=project_index::decode(bytes,references);if(!parsed)return parsed.error();
    auto views=std::span(references).first(parsed.value());
    Snapshot result;result.schema_=2;
    const auto charge=[&](std::size_t count){
      if(result.retained_>limit||count>limit-result.retained_)return false;
      result.retained_+=count;return true;
    };
    if(result.retained_>limit||bytes.size()>limit-result.retained_)
      return fail(ErrorCode::Exhausted,"Typed project retained byte limit");
    result.bytes_.assign(bytes.begin(),bytes.end());
    if(result.bytes_.capacity()>maximum_bytes||!charge(result.bytes_.capacity()))
      return fail(ErrorCode::Exhausted,"Typed project raw capacity limit");
    if(views.size()>(limit-result.retained_)/sizeof(Resolved))return fail(ErrorCode::Exhausted,"Typed project entry storage limit");
    result.entries_.reserve(views.size());
    if(result.entries_.capacity()>maximum_entries||result.entries_.capacity()>(limit-result.retained_)/sizeof(Resolved)||
        !charge(result.entries_.capacity()*sizeof(Resolved)))return fail(ErrorCode::Exhausted,"Typed project entry capacity limit");
    for(const auto& ref:views){
      auto recipe=resolve(ref,files);if(!recipe)return recipe.error();
      const auto matches=std::visit([&](const auto& value){return value.definition.id==ref.id&&value.revision==ref.revision;},recipe.value());
      if(!matches)return fail(ErrorCode::InvalidDependency,"Typed project recipe identity or revision mismatch");
      result.entries_.push_back({{ref.id,ref.kind,std::string(ref.recipe),ref.revision},std::move(recipe).value()});
      const auto& owned=result.entries_.back();
      if(!charge(owned.reference.recipe.capacity()+1))
        return fail(ErrorCode::Exhausted,"Typed project reference capacity limit");
      const auto charged=std::visit([&](const auto& value){
        return charge(value.definition.source.capacity()+1)&&charge(value.bytes.capacity());
      },owned.recipe);
      if(!charged)return fail(ErrorCode::Exhausted,"Typed project recipe retained capacity limit");
    }
    result.revision_=assets::digest(bytes);return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Typed project admission allocation");}
}
void Snapshot::swap(Snapshot& other) noexcept{
  bytes_.swap(other.bytes_);entries_.swap(other.entries_);std::swap(revision_,other.revision_);
  std::swap(retained_,other.retained_);std::swap(schema_,other.schema_);
}
Snapshot::Snapshot(Snapshot&& other) noexcept:Snapshot(){swap(other);}
Snapshot& Snapshot::operator=(Snapshot&& other) noexcept{
  if(this!=&other){Snapshot moved(std::move(other));swap(moved);}return *this;
}
Result<const Resolved*> Snapshot::find(assets::AssetId id) const noexcept{
  const auto found=std::lower_bound(entries_.begin(),entries_.end(),id,[](const Resolved& entry,assets::AssetId value){return less(entry.reference.id,value);});
  if(found!=entries_.end()&&found->reference.id==id)return &*found;
  return fail(ErrorCode::NotFound,"Typed project asset identity not indexed");
}
Result<Snapshot> decode(std::span<const std::byte> bytes,platform::RootedFiles& files,std::size_t limit){
  return Snapshot::open(bytes,files,limit);
}
Result<Snapshot> load(platform::RootedFiles& files,std::string_view index_leaf,std::size_t limit){
  if(!valid_index_leaf(index_leaf))return fail(ErrorCode::InvalidInput,"Project requires a canonical .raceproject leaf");
  try{
    auto bytes=files.read(index_leaf,maximum_bytes);if(!bytes)return bytes.error();
    if(bytes.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Typed project read capacity limit");
    return Snapshot::open(bytes.value(),files,limit);
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Typed project read allocation");}
}
} // namespace race::content_project
