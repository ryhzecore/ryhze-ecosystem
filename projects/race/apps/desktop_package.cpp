#include "desktop_package.hpp"
#include <algorithm>
#include <cstring>

namespace race::desktop {
namespace p=assets::pak;
namespace {
namespace sf=assets::surface;
struct TypedKey {p::Key key;p::Kind kind;};
Result<void> add_key(std::vector<TypedKey>& keys,TypedKey key){
  for(const auto& entry:keys)if(entry.key==key.key){
    if(entry.kind!=key.kind)return fail(ErrorCode::InvalidDependency,"One pinned product cannot have two package kinds");return {};
  }
  if(keys.size()>=p::Limits{}.entries-1)return fail(ErrorCode::Exhausted,"Package closure entry capacity exceeded");
  keys.push_back(key);return {};
}
Result<std::vector<TypedKey>> keys(const scene::Document& doc){
  auto meshes=mesh_references(doc);if(!meshes)return meshes.error();
  auto materials=material_references(doc);if(!materials)return materials.error();
  std::vector<TypedKey> out;
  for(const auto& ref:meshes.value()){auto added=add_key(out,{{ref.asset,ref.revision},p::Kind::Mesh});if(!added)return added.error();}
  for(const auto& ref:materials.value()){auto added=add_key(out,{{ref.asset,ref.revision},p::Kind::Material});if(!added)return added.error();}
  return out;
}
std::vector<p::Key> plain_keys(const std::vector<TypedKey>& keys){
  std::vector<p::Key> out;out.reserve(keys.size());for(const auto& entry:keys)out.push_back(entry.key);
  std::sort(out.begin(),out.end(),p::less);return out;
}
Result<std::vector<TypedKey>> product_dependencies(const TypedKey& expected,std::span<const std::byte> bytes,std::uint32_t& schema){
  schema=1;
  if(expected.kind==p::Kind::Mesh){
    auto view=assets::mesh::validate(bytes);if(!view)return view.error();schema=view.value().has_uv()?2u:1u;
    if(view.value().id()!=expected.key.id||view.value().revision()!=expected.key.revision||view.value().stamp().representation!=assets::mesh::Representation::Cooked)
      return fail(ErrorCode::InvalidInput,"Package mesh ID, revision or cooked representation mismatch");
  }else if(expected.kind==p::Kind::Texture){
    auto view=sf::validate_texture(bytes);if(!view)return view.error();schema=view.value().schema();
    if(view.value().id()!=expected.key.id||view.value().revision()!=expected.key.revision||view.value().stamp().representation!=sf::Representation::Cooked)
      return fail(ErrorCode::InvalidInput,"Package texture ID, revision or cooked representation mismatch");
  }else if(expected.kind==p::Kind::Material){
    auto view=sf::validate_material(bytes);if(!view)return view.error();schema=view.value().schema();
    if(view.value().id()!=expected.key.id||view.value().revision()!=expected.key.revision||view.value().stamp().representation!=sf::Representation::Cooked)
      return fail(ErrorCode::InvalidInput,"Package material ID, revision or cooked representation mismatch");
    if(view.value().material().base_color_texture){const auto& ref=*view.value().material().base_color_texture;
      return std::vector<TypedKey>{{{ref.asset,ref.revision},p::Kind::Texture}};}
  }else return fail(ErrorCode::Unsupported,"Unsupported desktop package product kind");
  return std::vector<TypedKey>{};
}
bool cooked_scene(std::span<const std::byte> b){return b.size()>=8&&!std::memcmp(b.data(),"RACESCN1",8);}
}
namespace {
Result<PreparedPackage> prepare_owned_package(Result<p::Package> package,const scene::Schemas& schemas){
  try{
    if(!package)return package.error();
    const p::Entry* scene_entry{};
    for(const auto& entry:package.value().entries())if(entry.kind==p::Kind::Scene){
      if(scene_entry)return fail(ErrorCode::Unsupported,"desktop package requires exactly one Scene");scene_entry=&entry;
    }
    if(!scene_entry)return fail(ErrorCode::NotFound,"desktop package Scene missing");
    auto lease=package.value().acquire(scene_entry->key);if(!lease)return lease.error();
    if(!cooked_scene(lease.value().bytes()))return fail(ErrorCode::InvalidInput,"package Scene must be cooked RACESCN1");
    auto files=package.value().files();if(!files)return files.error();
    auto scene=Document::decode(lease.value().bytes(),schemas);if(!scene)return scene.error();
    auto expected=keys(scene.value());if(!expected)return expected.error();
    if(scene_entry->dependencies!=plain_keys(expected.value()))return fail(ErrorCode::InvalidDependency,"Package Scene dependency list differs from its native references");
    for(std::size_t i=0;i<expected.value().size();++i){
      const auto target=expected.value()[i];auto product=package.value().acquire(target.key);if(!product)return product.error();
      if(product.value().entry().kind!=target.kind)return fail(ErrorCode::InvalidDependency,"Package native reference kind mismatch");
      std::uint32_t schema{};auto deps=product_dependencies(target,product.value().bytes(),schema);if(!deps)return deps.error();
      if(product.value().entry().schema!=schema)return fail(ErrorCode::InvalidDependency,"Package schema differs from its native product schema");
      if(product.value().entry().dependencies!=plain_keys(deps.value()))return fail(ErrorCode::InvalidDependency,"Package material dependencies differ from its native references");
      for(const auto& dep:deps.value()){auto added=add_key(expected.value(),dep);if(!added)return added.error();}
    }
    if(package.value().entries().size()!=expected.value().size()+1)return fail(ErrorCode::InvalidDependency,"Package must contain exactly the Scene's transitive native closure");
    auto refs=mesh_references(scene.value());if(!refs)return refs.error();
    auto materials=material_references(scene.value());if(!materials)return materials.error();
    auto meshes=MeshSet::prepare(files.value(),refs.value(),{},materials.value());if(!meshes)return meshes.error();
    auto instances=meshes.value()->validate_scene(scene.value());if(!instances)return instances.error();
    return PreparedPackage{std::move(scene).value(),std::move(meshes).value(),scene_entry->key};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package preparation allocation");}
}
} // namespace
Result<PreparedPackage> prepare_package(std::span<const std::byte> bytes,const scene::Schemas& schemas){
  return prepare_owned_package(p::Package::open(bytes),schemas);
}
Result<PreparedPackage> read_package(platform::Files& files,std::string_view path,const scene::Schemas& schemas){
  try{
    auto bytes=files.read(path,p::Limits{}.package_bytes);if(!bytes)return bytes.error();
    return prepare_owned_package(p::Package::open_owned(std::move(bytes).value()),schemas);
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package read allocation");}
}
Result<std::vector<std::byte>> pack_scene(platform::RootedFiles& files,std::string_view leaf,
                                       PersistentId id,const scene::Schemas& schemas){
  if(!id.valid()||!platform::valid_content_leaf(leaf)||!leaf.ends_with(".rscene"))return fail(ErrorCode::InvalidInput,"package requires a Scene ID and cooked Scene leaf");
  try{
    auto scene_bytes=files.read(leaf,p::Limits{}.chunk_bytes);if(!scene_bytes)return scene_bytes.error();
    if(!cooked_scene(scene_bytes.value()))return fail(ErrorCode::InvalidInput,"package Scene must be cooked RACESCN1");
    // Decode the exact owned bytes that will be packaged, without constructing
    // and copying an otherwise unused intermediate container.
    auto decoded=Document::decode(scene_bytes.value(),schemas);if(!decoded)return decoded.error();
    auto dependencies=keys(decoded.value());if(!dependencies)return dependencies.error();
    const auto direct=plain_keys(dependencies.value());
    struct Product{TypedKey target;std::vector<std::byte> bytes;std::vector<p::Key> dependencies;std::uint32_t schema;};
    std::vector<Product> products;products.reserve(p::Limits{}.entries-1);
    std::size_t retained=scene_bytes.value().size();
    for(std::size_t i=0;i<dependencies.value().size();++i){
      const auto target=dependencies.value()[i];
      auto product=files.read(assets::hex(target.key.revision)+".rasset",std::min(p::Limits{}.chunk_bytes,p::Limits{}.package_bytes-retained));if(!product)return product.error();
      std::uint32_t schema{};auto child=product_dependencies(target,product.value(),schema);if(!child)return child.error();
      retained+=product.value().size();
      for(const auto& dep:child.value()){auto added=add_key(dependencies.value(),dep);if(!added)return added.error();}
      products.push_back({target,std::move(product).value(),plain_keys(child.value()),schema});
    }
    std::vector<p::Input> inputs;inputs.reserve(products.size()+1);inputs.push_back({id,p::Kind::Scene,scene_bytes.value(),direct});
    for(const auto& product:products)inputs.push_back({product.target.key.id,product.target.kind,product.bytes,product.dependencies,product.schema});
    auto bytes=p::encode(inputs);if(!bytes)return bytes.error();
    // Encoded output now owns these bytes. Release cooker staging before
    // preparing consumer geometry; no leases borrow these temporary products.
    std::vector<p::Input>{}.swap(inputs);std::vector<Product>{}.swap(products);
    auto prepared=prepare_package(bytes.value(),schemas);if(!prepared)return prepared.error();return bytes;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package cooking allocation");}
}
Result<std::string> publish_package(platform::RootedFiles& files,std::span<const std::byte> bytes,const scene::Schemas& schemas){
  try{
    // Validation has quiesced before immutable publication/readback; retain no
    // unused geometry or snapshot during the second file buffer allocation.
    {auto prepared=prepare_package(bytes,schemas);if(!prepared)return prepared.error();}
    auto leaf=assets::hex(assets::digest(bytes))+".rpak";
    auto published=files.publish_immutable(leaf,bytes);if(!published)return published.error();
    auto verified=files.read(leaf,p::Limits{}.package_bytes);if(!verified)return verified.error();
    if(!std::equal(bytes.begin(),bytes.end(),verified.value().begin(),verified.value().end()))return fail(ErrorCode::IoFailure,"package publication readback mismatch");
    return leaf;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"package publication allocation");}
}
} // namespace race::desktop
