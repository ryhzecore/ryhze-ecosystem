#include "mesh_project.hpp"
#include <algorithm>

namespace race::mesh_project {
namespace {
bool less(assets::AssetId a,assets::AssetId b){return a.high<b.high||(a.high==b.high&&a.low<b.low);}
}
Result<std::vector<std::byte>> encode(std::span<const Reference> references){
  if(references.size()>maximum_entries)return fail(ErrorCode::Exhausted,"Project recipe count limit");
  std::array<project_index::View,maximum_entries> views{};
  for(std::size_t n=0;n<references.size();++n)views[n]={references[n].id,project_index::Kind::Mesh,references[n].recipe,references[n].revision};
  return project_index::encode(std::span(views).first(references.size()));
}
Result<Snapshot> Snapshot::open(std::span<const std::byte> bytes,platform::RootedFiles& files,std::size_t limit){
  if(limit>maximum_retained_bytes||!limit)return fail(ErrorCode::InvalidInput,"Project retained limit outside profile");
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty project index");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Project index byte limit");
  try{
    std::array<project_index::View,maximum_entries> references{};auto parsed=project_index::decode(bytes,references);if(!parsed)return parsed.error();
    auto views=std::span(references).first(parsed.value());
    for(const auto& ref:views)if(ref.kind!=project_index::Kind::Mesh)
      return fail(ErrorCode::InvalidDependency,"Mesh-only project contains a non-mesh reference");
    Snapshot result;result.retained_=sizeof(Snapshot);
    auto charge=[&](std::size_t count){if(result.retained_>limit||count>limit-result.retained_)return false;result.retained_+=count;return true;};
    if(result.retained_>limit||bytes.size()>limit-result.retained_)return fail(ErrorCode::Exhausted,"Project retained byte limit");
    result.bytes_.assign(bytes.begin(),bytes.end());if(result.bytes_.capacity()>maximum_bytes||!charge(result.bytes_.capacity()))return fail(ErrorCode::Exhausted,"Project raw capacity limit");
    if(parsed.value()>(limit-result.retained_)/sizeof(Resolved))return fail(ErrorCode::Exhausted,"Project entry storage limit");
    result.entries_.reserve(parsed.value());
    if(result.entries_.capacity()>maximum_entries||result.entries_.capacity()>(limit-result.retained_)/sizeof(Resolved)||!charge(result.entries_.capacity()*sizeof(Resolved)))return fail(ErrorCode::Exhausted,"Project entry capacity limit");
    for(const auto& ref:views){
      auto recipe=mesh_recipe::load(files,ref.recipe);if(!recipe)return recipe.error();
      if(recipe.value().definition.id!=ref.id||recipe.value().revision!=ref.revision)return fail(ErrorCode::InvalidDependency,"Project recipe identity or revision mismatch");
      std::string name(ref.recipe);
      result.entries_.push_back({{ref.id,std::move(name),ref.revision},std::move(recipe).value()});
      const auto& owned=result.entries_.back();
      if(!charge(owned.reference.recipe.capacity()+1)||!charge(owned.recipe.definition.source.capacity()+1)||!charge(owned.recipe.bytes.capacity()))return fail(ErrorCode::Exhausted,"Project recipe retained capacity limit");
    }
    result.revision_=assets::digest(bytes);return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Project admission allocation");}
}
void Snapshot::swap(Snapshot& other) noexcept{
  bytes_.swap(other.bytes_);entries_.swap(other.entries_);std::swap(revision_,other.revision_);std::swap(retained_,other.retained_);
}
Snapshot::Snapshot(Snapshot&& other) noexcept:Snapshot(){swap(other);}
Snapshot& Snapshot::operator=(Snapshot&& other) noexcept{
  if(this!=&other){Snapshot moved(std::move(other));swap(moved);}return *this;
}
Result<const Resolved*> Snapshot::find(assets::AssetId id) const noexcept{
  auto found=std::lower_bound(entries_.begin(),entries_.end(),id,[](const Resolved& entry,assets::AssetId value){return less(entry.reference.id,value);});
  if(found!=entries_.end()&&found->reference.id==id)return &*found;
  return fail(ErrorCode::NotFound,"Project asset identity not indexed");
}
Result<Snapshot> load(platform::RootedFiles& files,std::string_view index_leaf,std::size_t limit){
  if(!valid_index_leaf(index_leaf))return fail(ErrorCode::InvalidInput,"Project requires a canonical .raceproject leaf");
  try{
    auto bytes=files.read(index_leaf,maximum_bytes);if(!bytes)return bytes.error();
    if(bytes.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Project read capacity limit");
    return Snapshot::open(bytes.value(),files,limit);
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Project read allocation");}
}
} // namespace race::mesh_project
