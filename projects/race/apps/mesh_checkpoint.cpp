#include "mesh_checkpoint_internal.hpp"
#include "authoring_json.hpp"
#include <algorithm>

namespace race::mesh_checkpoint {
namespace {
Result<void> limits_check(const assets::mesh::Limits& limits){
  auto checked=assets::mesh::validate_limits(limits);if(!checked)return checked.error();
  if(limits.product_bytes>maximum_product_bytes)return fail(ErrorCode::InvalidInput,"Checkpoint product limit outside profile");return {};
}
bool valid(const Definition& d){return d.id.valid()&&d.index_revision!=assets::Digest{}&&d.recipe_revision!=assets::Digest{}&&d.source!=assets::Digest{}&&d.cooked_revision!=assets::Digest{};}
detail::ProjectPinView project_view(const mesh_project::Snapshot& project) noexcept {
  return {project.revision(),&project,[](const void* owner,assets::AssetId id) noexcept -> Result<detail::RecipePin> {
    auto entry=static_cast<const mesh_project::Snapshot*>(owner)->find(id);if(!entry)return entry.error();
    return detail::RecipePin{entry.value()->recipe.revision,entry.value()->recipe.definition.profile};
  }};
}
Result<void> product_pin(const Definition& d,const assets::mesh::View& view,mesh_pipeline::Profile profile){
  auto imported=mesh_pipeline::profile_import_stamp(profile,d.source);if(!imported)return imported.error();
  const auto expected=mesh_pipeline::cook_stamp(imported.value());
  if(view.id()!=d.id||view.revision()!=d.cooked_revision||view.stamp()!=expected||
      view.has_uv()!=(profile==mesh_pipeline::Profile::GeometryUv))
    return fail(ErrorCode::InvalidDependency,"Checkpoint cooked product provenance differs");return {};
}
}
Result<std::string> encode(const Definition& d){
  if(!valid(d))return fail(ErrorCode::InvalidInput,"Checkpoint identity is invalid");
  try{return std::string("{\"schema\":1,\"asset\":\"")+assets::id_text(d.id)+"\",\"index_revision\":\""+assets::hex(d.index_revision)+
    "\",\"recipe_revision\":\""+assets::hex(d.recipe_revision)+"\",\"source\":\""+assets::hex(d.source)+"\",\"cooked_revision\":\""+assets::hex(d.cooked_revision)+"\"}\n";
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Checkpoint encoding allocation");}
}
Result<Definition> decode(std::span<const std::byte> bytes){
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty checkpoint");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Checkpoint byte limit");
  authoring_json::Reader reader{{reinterpret_cast<const char*>(bytes.data()),bytes.size()}};
  constexpr std::array<std::string_view,6> keys{"schema","asset","index_revision","recipe_revision","source","cooked_revision"};
  if(!reader.take('{'))return fail(ErrorCode::InvalidInput,"Checkpoint requires an object");
  Definition result{};unsigned seen{};
  for(unsigned n=0;n<keys.size();++n){
    if(n&&!reader.take(','))return fail(ErrorCode::InvalidInput,"Checkpoint fields missing");
    auto key=reader.string();if(!key)return key.error();auto found=std::find(keys.begin(),keys.end(),key.value());
    if(found==keys.end())return fail(ErrorCode::InvalidInput,"Unknown checkpoint field");
    const auto index=static_cast<unsigned>(found-keys.begin());const unsigned bit=1U<<index;
    if(seen&bit)return fail(ErrorCode::Duplicate,"Duplicate checkpoint field");seen|=bit;
    if(!reader.take(':'))return fail(ErrorCode::InvalidInput,"Checkpoint field separator");
    if(!index){auto schema=reader.integer();if(!schema)return schema.error();if(schema.value()!=1)return fail(ErrorCode::IncompatibleVersion,"Unsupported checkpoint schema");}
    else{
      auto value=reader.string();if(!value)return value.error();
      if(index==1){auto id=assets::parse_id(value.value());if(!id)return id.error();result.id=id.value();}
      else{auto hash=authoring_json::hex(value.value());if(!hash)return hash.error();
        (index==2?result.index_revision:index==3?result.recipe_revision:index==4?result.source:result.cooked_revision)=hash.value();}
    }
  }
  if(seen!=63||!reader.take('}'))return fail(ErrorCode::InvalidInput,"Checkpoint fields missing or extra");
  reader.space();if(reader.offset!=reader.text.size()||!valid(result))return fail(ErrorCode::InvalidInput,"Checkpoint trailing data or invalid identity");return result;
}
Result<Published> detail::Core::capture(ProjectPinView project,assets::mesh::Lease lease,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints,const assets::mesh::Limits& limits){
  auto checked=limits_check(limits);if(!checked)return checked.error();
  if(!lease.operator->())return fail(ErrorCode::InvalidInput,"Checkpoint requires a live cooked lease");
  try{
    const auto& view=lease->view();auto entry=project.find(view.id());if(!entry)return entry.error();
    Definition definition{view.id(),project.revision,entry.value().revision,view.stamp().source,view.revision()};
    const auto profile=entry.value().profile;
    auto admitted=product_pin(definition,view,profile);if(!admitted)return admitted.error();
    if(view.bytes().size()>limits.product_bytes)return fail(ErrorCode::Exhausted,"Checkpoint product byte limit");
    auto readback=products.read(assets::hex(view.revision())+".rasset",limits.product_bytes);if(!readback)return readback.error();
    if(readback.value().capacity()>limits.product_bytes)return fail(ErrorCode::Exhausted,"Checkpoint product read capacity limit");
    // Exact equality inherits validation from the retained immutable Store lease.
    // Keep malformed/foreign readback diagnostics on the exceptional slow path.
    if(!std::equal(readback.value().begin(),readback.value().end(),view.bytes().begin(),view.bytes().end())){
      auto validated=assets::mesh::validate(readback.value(),limits);if(!validated)return validated.error();
      auto pin=product_pin(definition,validated.value(),profile);if(!pin)return pin.error();
      return fail(ErrorCode::IoFailure,"Checkpoint product readback mismatch");
    }
    if(view.vertex_count()>limits.vertices||view.index_count()>limits.indices)
      return fail(ErrorCode::Exhausted,"Checkpoint geometry limit");
    auto encoded=encode(definition);if(!encoded)return encoded.error();const auto bytes=std::as_bytes(std::span(encoded.value()));
    const auto revision=assets::digest(bytes);const auto leaf=assets::hex(revision)+".json";
    auto written=checkpoints.publish_immutable(leaf,bytes);if(!written)return written.error();
    auto saved=checkpoints.read(leaf,maximum_bytes);if(!saved)return saved.error();
    if(saved.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Checkpoint readback capacity limit");
    if(!std::equal(saved.value().begin(),saved.value().end(),bytes.begin(),bytes.end()))return fail(ErrorCode::IoFailure,"Checkpoint readback mismatch");
    return Published{definition,revision,leaf};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Checkpoint capture allocation");}
}
Result<Loaded> detail::Core::load(platform::RootedFiles& checkpoints,std::string_view leaf,ProjectPinView project,
    platform::RootedFiles& products,const assets::mesh::Limits& limits){
  auto checked=limits_check(limits);if(!checked)return checked.error();
  if(!platform::valid_content_leaf(leaf)||leaf.size()!=69||!leaf.ends_with(".json"))return fail(ErrorCode::InvalidInput,"Checkpoint requires a checksum-named JSON leaf");
  try{
    auto raw=checkpoints.read(leaf,maximum_bytes);if(!raw)return raw.error();
    if(raw.value().capacity()>maximum_bytes)return fail(ErrorCode::Exhausted,"Checkpoint read capacity limit");
    auto decoded=decode(raw.value());if(!decoded)return decoded.error();const auto revision=assets::digest(raw.value());
    if(leaf!=assets::hex(revision)+".json")return fail(ErrorCode::InvalidInput,"Checkpoint filename checksum differs");
    if(decoded.value().index_revision!=project.revision)return fail(ErrorCode::InvalidDependency,"Checkpoint project revision differs");
    auto entry=project.find(decoded.value().id);if(!entry)return entry.error();
    if(decoded.value().recipe_revision!=entry.value().revision)return fail(ErrorCode::InvalidDependency,"Checkpoint recipe revision differs");
    auto product=products.read(assets::hex(decoded.value().cooked_revision)+".rasset",limits.product_bytes);if(!product)return product.error();
    if(product.value().capacity()>limits.product_bytes)return fail(ErrorCode::Exhausted,"Checkpoint product read capacity limit");
    auto view=assets::mesh::validate(product.value(),limits);if(!view)return view.error();
    auto pin=product_pin(decoded.value(),view.value(),entry.value().profile);if(!pin)return pin.error();
    Loaded result;result.definition_=decoded.value();result.revision_=revision;
    result.manifest_=std::move(raw).value();result.product_=std::move(product).value();return result;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Checkpoint loading allocation");}
}
Result<Published> capture(const mesh_project::Snapshot& project,assets::mesh::Lease lease,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints,const assets::mesh::Limits& limits){
  return detail::Core::capture(project_view(project),std::move(lease),products,checkpoints,limits);
}
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,const mesh_project::Snapshot& project,
    platform::RootedFiles& products,const assets::mesh::Limits& limits){
  return detail::Core::load(checkpoints,leaf,project_view(project),products,limits);
}
void Loaded::swap(Loaded& other) noexcept{
  std::swap(definition_,other.definition_);std::swap(revision_,other.revision_);manifest_.swap(other.manifest_);product_.swap(other.product_);
}
Loaded::Loaded(Loaded&& other) noexcept{swap(other);}
Loaded& Loaded::operator=(Loaded&& other) noexcept{if(this!=&other){Loaded moved(std::move(other));swap(moved);}return *this;}
} // namespace race::mesh_checkpoint
