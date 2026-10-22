#include "surface_checkpoint.hpp"
#include "authoring_json.hpp"
#include <algorithm>

namespace race::surface_checkpoint {
namespace {
namespace sf=assets::surface;
bool valid(const Definition& d) noexcept {
  return (d.kind==content_project::Kind::Texture||d.kind==content_project::Kind::Material)&&d.id.valid()&&
      d.index_revision!=assets::Digest{}&&d.recipe_revision!=assets::Digest{}&&d.source!=assets::Digest{}&&d.cooked_revision!=assets::Digest{};
}
Result<const surface_recipe::Snapshot*> project_pin(const Definition& d,const content_project::Snapshot& project){
  if(d.index_revision!=project.revision())return fail(ErrorCode::InvalidDependency,"Surface checkpoint project revision differs");
  auto entry=project.find(d.id);if(!entry)return entry.error();
  const auto* recipe=std::get_if<surface_recipe::Snapshot>(&entry.value()->recipe);
  if(entry.value()->reference.kind!=d.kind||!recipe||recipe->revision!=d.recipe_revision)
    return fail(ErrorCode::InvalidDependency,"Surface checkpoint kind or recipe revision differs");
  return recipe;
}
Result<std::vector<std::byte>> read(platform::RootedFiles& files,std::string_view leaf,std::size_t limit){
  auto bytes=files.read(leaf,limit);if(!bytes)return bytes.error();
  if(bytes.value().size()>limit||bytes.value().capacity()>limit)return fail(ErrorCode::Exhausted,"Surface checkpoint read capacity limit");
  return bytes;
}
Result<std::vector<std::byte>> product_pin(const Definition& d,const content_project::Snapshot& project,
    std::span<const std::byte> product,platform::RootedFiles& products){
  auto recipe=project_pin(d,project);if(!recipe)return recipe.error();
  auto info=surface_pipeline::profile_info(recipe.value()->definition.profile);if(!info)return info.error();
  auto expected=info.value().cooked;expected.source=d.source;
  expected.dependencies=sf::dependency_digest(recipe.value()->definition.texture);
  if(product.size()>maximum_product_bytes)return fail(ErrorCode::Exhausted,"Surface checkpoint product byte limit");
  if(d.kind==content_project::Kind::Texture){
    auto view=sf::validate_texture(product);if(!view)return view.error();
    if(info.value().material||view.value().id()!=d.id||view.value().revision()!=d.cooked_revision||view.value().stamp()!=expected||
        view.value().schema()!=info.value().cooked_schema||view.value().encoding()!=info.value().encoding)
      return fail(ErrorCode::InvalidDependency,"Surface checkpoint texture provenance differs");
    return std::vector<std::byte>{};
  }
  auto view=sf::validate_material(product);if(!view)return view.error();
  if(!info.value().material||view.value().id()!=d.id||view.value().revision()!=d.cooked_revision||view.value().stamp()!=expected||
      view.value().schema()!=info.value().cooked_schema||view.value().material().base_color_texture!=recipe.value()->definition.texture||
      view.value().material().sampling!=(info.value().mips?sf::Sampling::LinearClampMips:sf::Sampling::LinearClampMip0)||
      view.value().material().texture_role!=(recipe.value()->definition.profile==surface_pipeline::Profile::MaterialScalarMips?sf::TextureRole::DiffuseReflectanceR:sf::TextureRole::Color))
    return fail(ErrorCode::InvalidDependency,"Surface checkpoint material provenance differs");
  if(!recipe.value()->definition.texture)return std::vector<std::byte>{};
  auto texture=read(products,assets::hex(recipe.value()->definition.texture->revision)+".rasset",maximum_product_bytes);
  if(!texture)return texture.error();auto bound=sf::validate_texture(texture.value());if(!bound)return bound.error();
  auto validated=sf::validate_binding(view.value(),bound.value());if(!validated)return validated.error();
  return texture;
}
}
Result<std::string> encode(const Definition& d){
  if(!valid(d))return fail(ErrorCode::InvalidInput,"Surface checkpoint identity is invalid");
  try{return std::string("{\"schema\":1,\"kind\":")+std::to_string(static_cast<unsigned>(d.kind))+",\"asset\":\""+assets::id_text(d.id)+
      "\",\"index_revision\":\""+assets::hex(d.index_revision)+"\",\"recipe_revision\":\""+assets::hex(d.recipe_revision)+
      "\",\"source\":\""+assets::hex(d.source)+"\",\"cooked_revision\":\""+assets::hex(d.cooked_revision)+"\"}\n";
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface checkpoint encoding allocation");}
}
Result<Definition> decode(std::span<const std::byte> bytes){
  if(bytes.empty())return fail(ErrorCode::InvalidInput,"Empty surface checkpoint");
  if(bytes.size()>maximum_bytes)return fail(ErrorCode::Exhausted,"Surface checkpoint byte limit");
  authoring_json::Reader reader{{reinterpret_cast<const char*>(bytes.data()),bytes.size()}};
  constexpr std::array<std::string_view,7> keys{"schema","kind","asset","index_revision","recipe_revision","source","cooked_revision"};
  if(!reader.take('{'))return fail(ErrorCode::InvalidInput,"Surface checkpoint requires an object");
  Definition result{};unsigned seen{};
  for(unsigned n=0;n<keys.size();++n){
    if(n&&!reader.take(','))return fail(ErrorCode::InvalidInput,"Surface checkpoint fields missing");
    auto key=reader.string();if(!key)return key.error();const auto found=std::find(keys.begin(),keys.end(),key.value());
    if(found==keys.end())return fail(ErrorCode::InvalidInput,"Unknown surface checkpoint field");
    const auto index=static_cast<unsigned>(found-keys.begin());const unsigned bit=1U<<index;
    if(seen&bit)return fail(ErrorCode::Duplicate,"Duplicate surface checkpoint field");seen|=bit;
    if(!reader.take(':'))return fail(ErrorCode::InvalidInput,"Surface checkpoint field separator");
    if(index<2){auto number=reader.integer();if(!number)return number.error();
      if(!index){if(number.value()!=1)return fail(ErrorCode::IncompatibleVersion,"Unsupported surface checkpoint schema");}
      else{if(number.value()!=2&&number.value()!=3)return fail(ErrorCode::InvalidInput,"Surface checkpoint kind differs");result.kind=static_cast<content_project::Kind>(number.value());}}
    else{auto value=reader.string();if(!value)return value.error();
      if(index==2){auto id=assets::parse_id(value.value());if(!id)return id.error();result.id=id.value();}
      else{auto digest=authoring_json::hex(value.value());if(!digest)return digest.error();
        (index==3?result.index_revision:index==4?result.recipe_revision:index==5?result.source:result.cooked_revision)=digest.value();}}
  }
  if(seen!=127||!reader.take('}'))return fail(ErrorCode::InvalidInput,"Surface checkpoint fields missing or extra");
  reader.space();if(reader.offset!=reader.text.size()||!valid(result))return fail(ErrorCode::InvalidInput,"Surface checkpoint trailing data or invalid identity");
  return result;
}
Result<Published> capture(const content_project::Snapshot& project,std::span<const std::byte> cooked,
    platform::RootedFiles& products,platform::RootedFiles& checkpoints){
  try{
    if(cooked.size()>maximum_product_bytes)return fail(ErrorCode::Exhausted,"Surface checkpoint product byte limit");
    Definition definition{};
    auto texture=sf::validate_texture(cooked);
    if(texture){definition.kind=content_project::Kind::Texture;definition.id=texture.value().id();definition.source=texture.value().stamp().source;definition.cooked_revision=texture.value().revision();}
    else{auto material=sf::validate_material(cooked);if(!material)return material.error();
      definition.kind=content_project::Kind::Material;definition.id=material.value().id();definition.source=material.value().stamp().source;definition.cooked_revision=material.value().revision();}
    definition.index_revision=project.revision();auto entry=project.find(definition.id);if(!entry)return entry.error();
    definition.recipe_revision=entry.value()->reference.revision;
    auto closure=product_pin(definition,project,cooked,products);if(!closure)return closure.error();
    auto saved=read(products,assets::hex(definition.cooked_revision)+".rasset",maximum_product_bytes);if(!saved)return saved.error();
    if(!std::equal(saved.value().begin(),saved.value().end(),cooked.begin(),cooked.end()))return fail(ErrorCode::IoFailure,"Surface product readback mismatch");
    auto encoded=encode(definition);if(!encoded)return encoded.error();const auto raw=std::as_bytes(std::span(encoded.value()));
    const auto revision=assets::digest(raw);const auto leaf=assets::hex(revision)+".json";
    auto written=checkpoints.publish_immutable(leaf,raw);if(!written)return written.error();
    auto manifest=read(checkpoints,leaf,maximum_bytes);if(!manifest)return manifest.error();
    if(!std::equal(manifest.value().begin(),manifest.value().end(),raw.begin(),raw.end()))return fail(ErrorCode::IoFailure,"Surface checkpoint readback mismatch");
    return Published{definition,revision,leaf};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface checkpoint capture allocation");}
}
Result<Loaded> load(platform::RootedFiles& checkpoints,std::string_view leaf,const content_project::Snapshot& project,
    platform::RootedFiles& products){
  if(!platform::valid_content_leaf(leaf)||leaf.size()!=69||!leaf.ends_with(".json"))return fail(ErrorCode::InvalidInput,"Surface checkpoint requires checksum-named JSON");
  try{
    auto manifest=read(checkpoints,leaf,maximum_bytes);if(!manifest)return manifest.error();
    const auto revision=assets::digest(manifest.value());if(leaf!=assets::hex(revision)+".json")return fail(ErrorCode::InvalidInput,"Surface checkpoint filename checksum differs");
    auto definition=decode(manifest.value());if(!definition)return definition.error();
    auto recipe=project_pin(definition.value(),project);if(!recipe)return recipe.error();
    auto product=read(products,assets::hex(definition.value().cooked_revision)+".rasset",maximum_product_bytes);if(!product)return product.error();
    auto texture=product_pin(definition.value(),project,product.value(),products);if(!texture)return texture.error();
    if(manifest.value().capacity()>maximum_buffer_bytes-product.value().capacity()||
        texture.value().capacity()>maximum_buffer_bytes-product.value().capacity()-manifest.value().capacity())
      return fail(ErrorCode::Exhausted,"Surface checkpoint total buffer capacity limit");
    Loaded out;out.definition_=definition.value();out.revision_=revision;out.manifest_=std::move(manifest).value();
    out.product_=std::move(product).value();out.texture_=std::move(texture).value();return out;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Surface checkpoint loading allocation");}
}
void Loaded::swap(Loaded& other) noexcept {
  std::swap(definition_,other.definition_);std::swap(revision_,other.revision_);manifest_.swap(other.manifest_);product_.swap(other.product_);texture_.swap(other.texture_);
}
Loaded::Loaded(Loaded&& other) noexcept{swap(other);}
Loaded& Loaded::operator=(Loaded&& other) noexcept{if(this!=&other){Loaded moved(std::move(other));swap(moved);}return *this;}
} // namespace race::surface_checkpoint
