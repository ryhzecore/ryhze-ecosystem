#include "desktop_meshes.hpp"
#include <algorithm>

namespace race::desktop {
namespace m=assets::mesh;
namespace sf=assets::surface;
namespace {
constexpr std::size_t maximum_revisions=128,maximum_payload=64*1024*1024;
Result<m::View> cooked(std::span<const std::byte> bytes){
  auto view=m::validate(bytes);if(!view)return view.error();
  if(view.value().stamp().representation!=m::Representation::Cooked)
    return fail(ErrorCode::InvalidInput,"Native mesh is imported only; cook it before loading");
  return view;
}
}
MeshSet::MeshSet(std::shared_ptr<platform::RootedFiles> files,std::vector<ResolvedMesh> entries)
    :files_(std::move(files)),entries_(std::move(entries)){}
Result<const ResolvedMesh*> MeshSet::find(const MeshReference& ref) const{
  for(const auto& entry:entries_)if(entry.reference==ref)return &entry;
  return fail(ErrorCode::NotFound,"Scene mesh revision is not prepared in the selected assets folder");
}
Result<void> MeshSet::validate_instances(std::span<const MeshReference> refs) const{
  if(refs.size()>render::viewport_max_draws)return fail(ErrorCode::Exhausted,"Scene mesh instance capacity exceeded");
  std::size_t vertices{},indices{};
  for(const auto& ref:refs){
    auto entry=find(ref);if(!entry)return entry.error();const auto& g=*entry.value()->geometry;
    if(g.vertices().size()>render::viewport_mesh_frame_vertices-vertices||g.indices().size()>render::viewport_mesh_frame_indices-indices)
      return fail(ErrorCode::Exhausted,"Scene mesh instances exceed viewport geometry capacity");
    vertices+=g.vertices().size();indices+=g.indices().size();
  }
  return {};
}
Result<std::shared_ptr<const MeshSet>> MeshSet::prepare(std::shared_ptr<platform::RootedFiles> files,
    std::span<const MeshReference> refs,const std::shared_ptr<const MeshSet>& reuse,std::span<const MaterialReference> materials){
  try{
    // The Scene object cap bounds input traversal; distinct retained revisions
    // and payload have tighter limits independent of repeated instances.
    if(refs.size()>render::viewport_max_draws)return fail(ErrorCode::Exhausted,"Scene mesh instance capacity exceeded");
    if(!refs.empty()&&!files)return fail(ErrorCode::NotFound,"Choose an assets folder for this Scene's mesh references");
    std::vector<ResolvedMesh> entries;entries.reserve(std::min(refs.size(),maximum_revisions));
    std::unique_ptr<m::Store> store;
    std::size_t payload{};
    for(const auto& ref:refs){
      if(!ref.valid())return fail(ErrorCode::InvalidInput,"Invalid pinned mesh reference");
      if(std::any_of(entries.begin(),entries.end(),[&](const auto& e){return e.reference==ref;}))continue;
      if(entries.size()==maximum_revisions)return fail(ErrorCode::Exhausted,"Scene mesh revision capacity exceeded");
      ResolvedMesh entry;entry.reference=ref;
      const auto old=reuse&&reuse->files_==files ? reuse->find(ref) : Result<const ResolvedMesh*>{fail(ErrorCode::NotFound,"not cached")};
      if(old)entry=*old.value();
      else{
        auto bytes=files->read(assets::hex(ref.revision)+".rasset",m::Limits{}.product_bytes);if(!bytes)return bytes.error();
        auto view=cooked(bytes.value());if(!view)return view.error();
        if(view.value().id()!=ref.asset||view.value().revision()!=ref.revision)
          return fail(ErrorCode::InvalidInput,"Scene mesh asset ID or full revision does not match its product");
        const auto charge=view.value().vertex_count()*sizeof(render::GeometryVertex)+view.value().index_count()*sizeof(std::uint32_t);
        if(charge>maximum_payload-payload)return fail(ErrorCode::Exhausted,"Scene mesh geometry payload capacity exceeded");
        if(!store){
          auto allocator=TrackedAllocator::create({maximum_payload});if(!allocator)return allocator.error();
          auto created=m::Store::create({},allocator.value());if(!created)return created.error();store=std::move(created).value();
        }
        auto update=store->begin_update(ref.asset,view.value().stamp());if(!update)return update.error();
        auto published=store->publish(update.value(),bytes.value());if(!published)return published.error();
        auto lease=store->acquire(published.value());if(!lease)return lease.error();
        auto packet=copy_mesh_geometry(lease.value());if(!packet)return packet.error();
        entry.bounds=view.value().bounds();entry.geometry=std::move(packet).value();
        // Render packet now owns the copy. This temporary producer retains no
        // active payload between revisions, including two revisions of one ID.
        auto retired=store->retire(ref.asset);if(!retired)return retired.error();
      }
      const auto bytes=entry.geometry->vertices().size()*sizeof(render::GeometryVertex)+entry.geometry->indices().size()*sizeof(std::uint32_t);
      if(bytes>maximum_payload-payload)return fail(ErrorCode::Exhausted,"Scene mesh geometry payload capacity exceeded");
      payload+=bytes;entries.push_back(std::move(entry));
    }
    auto out=std::shared_ptr<MeshSet>(new MeshSet(std::move(files),std::move(entries)));
    auto loaded=out->load_materials(materials,reuse);if(!loaded)return loaded.error();
    return std::shared_ptr<const MeshSet>(std::move(out));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Scene mesh preparation allocation failed");}
}
Result<std::shared_ptr<const MeshSet>> MeshSet::extend(std::shared_ptr<const MeshSet> prior,
    std::span<const std::byte> exact_product){
  try{
    if(!prior)return fail(ErrorCode::InvalidInput,"Mesh extension requires a prepared dependency set");
    auto view=cooked(exact_product);if(!view)return view.error();
    const MeshReference ref{view.value().id(),view.value().revision()};
    if(prior->find(ref))return prior;
    if(std::none_of(prior->entries_.begin(),prior->entries_.end(),[&](const auto& e){return e.reference.asset==ref.asset;}))
      return fail(ErrorCode::InvalidInput,"Mesh extension must update an already prepared asset");
    if(prior->entries_.size()==maximum_revisions)return fail(ErrorCode::Exhausted,"Scene mesh revision capacity exceeded");
    std::size_t payload{};
    for(const auto& entry:prior->entries_){
      const auto bytes=entry.geometry->vertices().size()*sizeof(render::GeometryVertex)+entry.geometry->indices().size()*sizeof(std::uint32_t);
      if(bytes>maximum_payload-payload)return fail(ErrorCode::Exhausted,"Scene mesh geometry payload capacity exceeded");
      payload+=bytes;
    }
    const auto charge=view.value().vertex_count()*sizeof(render::GeometryVertex)+view.value().index_count()*sizeof(std::uint32_t);
    if(charge>maximum_payload-payload)return fail(ErrorCode::Exhausted,"Scene mesh geometry payload capacity exceeded");
    auto allocator=TrackedAllocator::create({maximum_payload});if(!allocator)return allocator.error();
    auto store=m::Store::create({},allocator.value());if(!store)return store.error();
    auto update=store.value()->begin_update(ref.asset,view.value().stamp());if(!update)return update.error();
    auto published=store.value()->publish(update.value(),exact_product);if(!published)return published.error();
    auto lease=store.value()->acquire(published.value());if(!lease)return lease.error();
    auto packet=copy_mesh_geometry(lease.value());if(!packet)return packet.error();
    std::vector<ResolvedMesh> entries;entries.reserve(prior->entries_.size()+1);
    entries.insert(entries.end(),prior->entries_.begin(),prior->entries_.end());
    entries.push_back({ref,view.value().bounds(),std::move(packet).value()});
    auto out=std::shared_ptr<MeshSet>(new MeshSet(prior->files_,std::move(entries)));
    out->textures_=prior->textures_;out->materials_=prior->materials_;
    return std::shared_ptr<const MeshSet>(std::move(out));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Scene mesh extension allocation failed");}
}
Result<const ResolvedMaterial*> MeshSet::find_material(const MaterialReference& ref) const{
  for(const auto& entry:materials_)if(entry.reference==ref)return &entry;
  return fail(ErrorCode::NotFound,"Scene material revision is not prepared in the selected assets folder");
}
Result<void> MeshSet::load_materials(std::span<const MaterialReference> refs,
    const std::shared_ptr<const MeshSet>& reuse,std::span<const std::byte> exact_product){
  if(refs.size()>render::viewport_max_draws)return fail(ErrorCode::Exhausted,"Scene material instance capacity exceeded");
  if(!refs.empty()&&!files_)return fail(ErrorCode::NotFound,"Choose an assets folder for this Scene's materials");
  const auto* cached=reuse&&reuse->files_==files_?reuse.get():nullptr;
  materials_.reserve(std::min(refs.size(),maximum_revisions));
  textures_.reserve(std::min(refs.size(),render::viewport_max_textures));
  std::optional<sf::MaterialView> supplied;
  if(!exact_product.empty()){
    auto view=sf::validate_material(exact_product);if(!view)return view.error();
    if(view.value().stamp().representation!=sf::Representation::Cooked)return fail(ErrorCode::InvalidInput,"Material must be cooked before loading");
    supplied=view.value();
  }
  std::size_t texture_bytes{};
  for(const auto& ref:refs){
    if(!ref.valid())return fail(ErrorCode::InvalidInput,"Invalid pinned material reference");
    if(find_material(ref))continue;
    if(materials_.size()==maximum_revisions)return fail(ErrorCode::Exhausted,"Scene material revision capacity exceeded");
    ResolvedMaterial entry;entry.reference=ref;
    auto old=cached?cached->find_material(ref):Result<const ResolvedMaterial*>{fail(ErrorCode::NotFound,"not cached")};
    std::vector<std::byte> bytes;
    std::optional<sf::MaterialView> view;
    if(old)entry=*old.value();
    else{
      if(supplied&&supplied->id()==ref.asset&&supplied->revision()==ref.revision)view=*supplied;
      else{
        auto read=files_->read(assets::hex(ref.revision)+".rasset",sf::Limits{}.product_bytes);if(!read)return read.error();if(read.value().capacity()>sf::Limits{}.product_bytes)return fail(ErrorCode::Exhausted,"Material source buffer capacity limit");bytes=std::move(read).value();
        auto valid=sf::validate_material(bytes);if(!valid)return valid.error();view=valid.value();
      }
      if(view->id()!=ref.asset||view->revision()!=ref.revision||view->stamp().representation!=sf::Representation::Cooked)
        return fail(ErrorCode::InvalidInput,"Material ID, full revision or cooked representation mismatch");
      if(view->material().base_color_texture){const auto& t=*view->material().base_color_texture;entry.texture_reference=MeshReference{t.asset,t.revision};}
    }
    std::shared_ptr<const render::ViewportTexture> texture;
    if(entry.texture_reference){
      const auto& target=*entry.texture_reference;
      auto found=std::find_if(textures_.begin(),textures_.end(),[&](const auto& t){return t.reference==target;});
      if(found!=textures_.end())texture=found->texture;
      else{
        if(textures_.size()==render::viewport_max_textures)return fail(ErrorCode::Exhausted,"Scene texture revision capacity exceeded");
        if(cached){auto retained=std::find_if(cached->textures_.begin(),cached->textures_.end(),[&](const auto& t){return t.reference==target;});if(retained!=cached->textures_.end())texture=retained->texture;}
        if(!texture){
          auto read=files_->read(assets::hex(target.revision)+".rasset",sf::Limits{}.product_bytes);if(!read)return read.error();
          if(read.value().capacity()>sf::Limits{}.product_bytes)return fail(ErrorCode::Exhausted,"Texture source buffer capacity limit");
          auto valid=sf::validate_texture(read.value());if(!valid)return valid.error();
          const auto& t=valid.value();
          if(t.id()!=target.asset||t.revision()!=target.revision||t.stamp().representation!=sf::Representation::Cooked||
              (t.encoding()!=sf::Encoding::SrgbColor&&!(t.schema()==3&&t.encoding()==sf::Encoding::LinearData)))
            return fail(ErrorCode::InvalidInput,"Material texture requires an exact cooked color or scalar-mip product");
          if(t.texels().size()>render::viewport_texture_frame_bytes-texture_bytes)return fail(ErrorCode::Exhausted,"Scene texture payload capacity exceeded");
          if(view){
            const auto expected=view->material().texture_role==sf::TextureRole::DiffuseReflectanceR?sf::Encoding::LinearData:sf::Encoding::SrgbColor;
            if(t.encoding()!=expected)return fail(ErrorCode::InvalidInput,"Material texture encoding does not match its declared role");
            auto bound=sf::validate_binding(*view,t);if(!bound)return bound.error();
          }
          // Preserve the validated schema even for a complete 1x1 chain.
          auto packet=t.schema()==3?render::ViewportTexture::create_scalar_mips(t.width(),t.height(),t.texels()):
              t.schema()==2?render::ViewportTexture::create_mips(t.width(),t.height(),t.texels()):
              render::ViewportTexture::create(t.width(),t.height(),t.pixels());
          if(!packet)return packet.error();texture=std::move(packet).value();
        }
        if(texture->texels().size()>render::viewport_texture_frame_bytes-texture_bytes)return fail(ErrorCode::Exhausted,"Scene texture payload capacity exceeded");
        texture_bytes+=texture->texels().size();textures_.push_back({target,texture});
      }
    }
    if(!entry.material){
      const auto& c=view->material().base_color;
      const auto sampling=view->material().sampling==sf::Sampling::LinearClampMips?
          render::TextureSampling::LinearClampMips:render::TextureSampling::LinearClampMip0;
      // Render admission also checks an already-retained texture against this
      // material's profile, so deduplication never upgrades a legacy texture.
      const auto role=view->material().texture_role==sf::TextureRole::DiffuseReflectanceR?
          render::TextureRole::DiffuseReflectanceR:render::TextureRole::Color;
      auto packet=render::ViewportMaterial::create({float(c[0]),float(c[1]),float(c[2]),1},texture,sampling,role);
      if(!packet)return packet.error();entry.material=std::move(packet).value();
    }
    materials_.push_back(std::move(entry));
  }
  return {};
}
Result<std::shared_ptr<const MeshSet>> MeshSet::extend_material(std::shared_ptr<const MeshSet> prior,std::span<const std::byte> exact_product){
  try{
    if(!prior)return fail(ErrorCode::InvalidInput,"Material extension requires a prepared dependency set");
    auto view=sf::validate_material(exact_product);if(!view)return view.error();
    if(view.value().stamp().representation!=sf::Representation::Cooked)return fail(ErrorCode::InvalidInput,"Material must be cooked before loading");
    const MaterialReference ref{view.value().id(),view.value().revision()};
    if(prior->find_material(ref))return prior;
    if(prior->materials_.size()==maximum_revisions)return fail(ErrorCode::Exhausted,"Scene material revision capacity exceeded");
    std::vector<MaterialReference> refs;refs.reserve(prior->materials_.size()+1);
    for(const auto& material:prior->materials_)refs.push_back(material.reference);refs.push_back(ref);
    auto out=std::shared_ptr<MeshSet>(new MeshSet(prior->files_,prior->entries_));
    auto loaded=out->load_materials(refs,prior,exact_product);if(!loaded)return loaded.error();
    return std::shared_ptr<const MeshSet>(std::move(out));
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Scene material extension allocation failed");}
}
Result<void> MeshSet::validate_scene(const scene::Document& doc) const{
  auto refs=mesh_references(doc);if(!refs)return refs.error();
  auto admitted=validate_instances(refs.value());if(!admitted)return admitted;
  std::size_t mesh_index{};
  for(const auto& object:doc.objects){
    const bool mesh=std::any_of(object.components.begin(),object.components.end(),[](const auto& c){return c.type==mesh_type;});
    const ResolvedMesh* geometry{};
    if(mesh){auto entry=find(refs.value()[mesh_index++]);if(!entry)return entry.error();geometry=entry.value();}
    auto material=object_material(object);if(!material)return material.error();
    if(material.value()){
      auto entry=find_material(*material.value());if(!entry)return entry.error();
      if(geometry&&entry.value()->material->texture()&&!geometry->geometry->has_uv())
        return fail(ErrorCode::InvalidInput,"Textured materials require mesh UV coordinates");
    }
  }
  return {};
}
Result<MaterialReference> material_file_reference(platform::RootedFiles& files,std::string_view leaf){
  if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(".rasset"))return fail(ErrorCode::InvalidInput,"Select a native material leaf in the assets folder");
  try{
    auto bytes=files.read(leaf,sf::Limits{}.product_bytes);if(!bytes)return bytes.error();
    auto view=sf::validate_material(bytes.value());if(!view)return view.error();
    if(view.value().stamp().representation!=sf::Representation::Cooked||leaf!=assets::hex(view.value().revision())+".rasset")
      return fail(ErrorCode::InvalidInput,"Material must be cooked with its full revision filename");
    return MaterialReference{view.value().id(),view.value().revision()};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Native material selection allocation failed");}
}
Result<MeshReference> mesh_file_reference(platform::RootedFiles& files,std::string_view leaf){
  if(!platform::valid_content_leaf(leaf)||!leaf.ends_with(".rasset"))return fail(ErrorCode::InvalidInput,"Select a native mesh leaf in the assets folder");
  try{
    auto bytes=files.read(leaf,m::Limits{}.product_bytes);if(!bytes)return bytes.error();
    auto view=cooked(bytes.value());if(!view)return view.error();
    if(leaf!=assets::hex(view.value().revision())+".rasset")return fail(ErrorCode::InvalidInput,"Native mesh filename must match its full revision hash");
    return MeshReference{view.value().id(),view.value().revision()};
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Native mesh selection allocation failed");}
}
Result<assets::mesh::Bounds> local_bounds(const ObjectRow& row,const MeshSet* meshes){
  if(row.mesh){
    if(!meshes)return fail(ErrorCode::NotFound,"Mesh dependencies have not been loaded");
    auto entry=meshes->find(*row.mesh);if(!entry)return entry.error();return entry.value()->bounds;
  }
  return m::Bounds{{-row.dimensions.x/2,-row.dimensions.y/2,-row.dimensions.z/2},
                   {row.dimensions.x/2,row.dimensions.y/2,row.dimensions.z/2}};
}
Result<render::DrawPrimitive> mesh_draw(const ObjectRow& row,const MeshSet* meshes,bool selected){
  render::DrawPrimitive draw;draw.id=row.id;draw.world=row.world.m;draw.selected=selected;
  draw.color={float(row.color.x),float(row.color.y),float(row.color.z),1};
  if(row.mesh){
    if(!meshes)return fail(ErrorCode::NotFound,"Mesh dependencies have not been loaded");
    auto entry=meshes->find(*row.mesh);if(!entry)return entry.error();draw.geometry=entry.value()->geometry;
  }else for(std::size_t r=0;r<3;++r){draw.world[r*4]*=row.dimensions.x;draw.world[r*4+1]*=row.dimensions.y;draw.world[r*4+2]*=row.dimensions.z;}
  if(row.material){
    if(!meshes)return fail(ErrorCode::NotFound,"Material dependencies have not been loaded");
    auto entry=meshes->find_material(*row.material);if(!entry)return entry.error();draw.material=entry.value()->material;
    if(draw.material->texture()&&draw.geometry&&!draw.geometry->has_uv())return fail(ErrorCode::InvalidInput,"Textured materials require mesh UV coordinates");
  }
  return draw;
}
Result<void> apply_lighting(render::RenderFrame& frame,const LightingSettings& settings){
  frame.physical=render::PhysicalLighting{};
  if(!settings.output||settings.output->profile!=render::LightingProfile::PhysicalDiffuseV1){
    frame.lighting_profile=render::LightingProfile::LegacyViewportV1;return {};
  }
  const auto& output=*settings.output;render::PhysicalLighting physical;
  if(settings.light&&settings.light->enabled){
    physical.direction_to_light={settings.light->direction.x,settings.light->direction.y,settings.light->direction.z};
    physical.illuminance_lux=settings.light->illuminance_lux;
  }else{physical.direction_to_light={0,1,0};physical.illuminance_lux=0;}
  physical.environment_cd_m2=output.environment_cd_m2;
  physical.background_cd_m2={output.background_cd_m2.x,output.background_cd_m2.y,output.background_cd_m2.z};
  physical.exposure_stops=output.exposure_stops;physical.reference_luminance_cd_m2=output.reference_luminance_cd_m2;physical.target=output.target;
  auto resolved=render::resolve_physical_lighting(physical);if(!resolved)return resolved.error();
  frame.lighting_profile=render::LightingProfile::PhysicalDiffuseV1;frame.physical=physical;return {};
}
Result<render::RenderFrame> scene_frame(const Document& doc,const MeshSet* meshes,
    const render::ViewportCamera& camera,bool play,std::uint64_t tick){
  try{
    auto extraction=doc.extract(play);if(!extraction)return extraction.error();
    const auto& rows=extraction.value().rows;
    if(rows.size()>render::viewport_max_draws)return fail(ErrorCode::Exhausted,"Scene draw capacity exceeded");
    render::RenderFrame frame;frame.camera=camera;frame.scene_generation=doc.generation(play);
    frame.origin_revision=doc.origin(play).revision;frame.simulation_tick=tick;frame.draws.reserve(rows.size());
    for(const auto& row:rows)if(row.visible){auto draw=mesh_draw(row,meshes);if(!draw)return draw.error();frame.draws.push_back(std::move(draw).value());}
    auto lit=apply_lighting(frame,extraction.value().lighting);if(!lit)return lit.error();
    return frame;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Scene frame copy allocation failed");}
}
} // namespace race::desktop
