#include "desktop_scene.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace race::desktop {
namespace {
constexpr std::uint64_t id_namespace=0x52414345424f5801;
constexpr std::string_view codec_identity="race-desktop-light-output-schema-1";
constexpr std::string_view surface_codec_identity="race-desktop-surface-schema-1";
scene::ComponentSchema box_schema(){return {box_type,1,"Box",{
  {1,"dimensions",scene::Vec3{1,1,1},.01,1000,true},
  {2,"color",scene::Vec3{.28,.52,.78},0,1,true}}};}
scene::ComponentSchema mesh_schema(){return {mesh_type,1,"Mesh",{
  {1,"asset",PersistentId{}},{2,"revisionLow",PersistentId{}},{3,"revisionHigh",PersistentId{}},
  {4,"color",scene::Vec3{.28,.52,.78},0,1,true}}};}
scene::ComponentSchema material_schema(){return {material_type,1,"Material",{
  {1,"asset",PersistentId{}},{2,"revisionLow",PersistentId{}},{3,"revisionHigh",PersistentId{}}}};}
// P03.04 settings: field bounds are the declared photometric bounds of PhysicalDiffuseV1.
scene::ComponentSchema light_schema(){return {light_type,1,"DirectionalLight",{
  {1,"direction",scene::Vec3{.4,.8,.3},-1e6,1e6,true},
  {2,"illuminanceLux",500.0,0,render::physical_illuminance_max_lux,true},
  {3,"enabled",true,-1e300,1e300,true}}};}
scene::ComponentSchema output_schema(){return {output_type,1,"RenderOutput",{
  {1,"profile",std::int64_t{2},1,2,true},
  {2,"environmentCdM2",20.0,0,render::physical_environment_max_cd_m2,true},
  {3,"exposureStops",0.0,-render::physical_exposure_max_stops,render::physical_exposure_max_stops,true},
  {4,"referenceLuminanceCdM2",100.0,render::physical_reference_min_cd_m2,render::physical_reference_max_cd_m2,true},
  {5,"displayTarget",std::int64_t{1},1,2,true},
  {6,"paperWhiteNits",200.0,render::physical_hdr_paper_white_min_nits,render::physical_hdr_peak_max_nits,true},
  {7,"peakNits",1000.0,render::physical_hdr_paper_white_min_nits,render::physical_hdr_peak_max_nits,true},
  {8,"backgroundCdM2",scene::Vec3{3,4,6.4},0,render::physical_background_max_cd_m2,true}}};}
template<class T> Result<T> typed_field(const scene::Component& component,scene::FieldId id,T fallback,const char* message){
  for(const auto& field:component.fields)if(field.id==id){
    const auto* value=std::get_if<T>(&field.value);if(!value)return fail(ErrorCode::InvalidInput,message);return *value;
  }
  return fallback; // Normalised Scenes carry every field; raw candidates fall back to the schema default.
}
Result<DirectionalLightSettings> light_from(const scene::Component& c){
  if(c.version!=1)return fail(ErrorCode::IncompatibleVersion,"Unsupported DirectionalLight component version");
  DirectionalLightSettings out;const auto defaults=DirectionalLightSettings{};
  auto direction=typed_field(c,1,defaults.direction,"DirectionalLight direction has the wrong type");if(!direction)return direction.error();
  auto lux=typed_field(c,2,defaults.illuminance_lux,"DirectionalLight illuminance has the wrong type");if(!lux)return lux.error();
  auto enabled=typed_field(c,3,defaults.enabled,"DirectionalLight enabled flag has the wrong type");if(!enabled)return enabled.error();
  out.direction=direction.value();out.illuminance_lux=lux.value();out.enabled=enabled.value();return out;
}
Result<RenderOutputSettings> output_from(const scene::Component& c){
  if(c.version!=1)return fail(ErrorCode::IncompatibleVersion,"Unsupported RenderOutput component version");
  RenderOutputSettings out;const auto d=RenderOutputSettings{};
  auto profile=typed_field(c,1,std::int64_t(d.profile),"RenderOutput profile has the wrong type");if(!profile)return profile.error();
  auto environment=typed_field(c,2,d.environment_cd_m2,"RenderOutput environment has the wrong type");if(!environment)return environment.error();
  auto stops=typed_field(c,3,d.exposure_stops,"RenderOutput exposure has the wrong type");if(!stops)return stops.error();
  auto reference=typed_field(c,4,d.reference_luminance_cd_m2,"RenderOutput reference luminance has the wrong type");if(!reference)return reference.error();
  auto target=typed_field(c,5,std::int64_t(d.target),"RenderOutput display target has the wrong type");if(!target)return target.error();
  auto paper=typed_field(c,6,d.paper_white_nits,"RenderOutput paper white has the wrong type");if(!paper)return paper.error();
  auto peak=typed_field(c,7,d.peak_nits,"RenderOutput peak has the wrong type");if(!peak)return peak.error();
  auto background=typed_field(c,8,d.background_cd_m2,"RenderOutput background has the wrong type");if(!background)return background.error();
  if(profile.value()<1||profile.value()>2)return fail(ErrorCode::Unsupported,"Unknown RenderOutput lighting profile");
  if(target.value()<1||target.value()>2)return fail(ErrorCode::Unsupported,"Unknown RenderOutput display target");
  out.profile=static_cast<render::LightingProfile>(profile.value());out.environment_cd_m2=environment.value();out.exposure_stops=stops.value();
  out.reference_luminance_cd_m2=reference.value();out.target=static_cast<render::OutputTarget>(target.value());
  out.paper_white_nits=paper.value();out.peak_nits=peak.value();out.background_cd_m2=background.value();return out;
}
scene::Component light_component(const DirectionalLightSettings& l){
  return {light_type,1,{{1,l.direction},{2,l.illuminance_lux},{3,l.enabled}}};
}
scene::Component output_component(const RenderOutputSettings& o){
  return {output_type,1,{{1,std::int64_t(o.profile)},{2,o.environment_cd_m2},{3,o.exposure_stops},{4,o.reference_luminance_cd_m2},
    {5,std::int64_t(o.target)},{6,o.paper_white_nits},{7,o.peak_nits},{8,o.background_cd_m2}}};
}
bool within(double value,double low,double high){return std::isfinite(value)&&value>=low&&value<=high;}
PersistentId half(const std::array<std::byte,32>& digest,std::size_t offset){
  PersistentId out;
  for(std::size_t i=0;i<8;++i){out.high|=std::uint64_t(std::to_integer<unsigned>(digest[offset+i]))<<(8*i);out.low|=std::uint64_t(std::to_integer<unsigned>(digest[offset+8+i]))<<(8*i);}
  return out;
}
void put_half(std::array<std::byte,32>& digest,std::size_t offset,PersistentId id){
  for(std::size_t i=0;i<8;++i){digest[offset+i]=std::byte((id.high>>(8*i))&255);digest[offset+8+i]=std::byte((id.low>>(8*i))&255);}
}
Result<MeshReference> reference(const scene::Component& component){
  if(component.version!=1)return fail(ErrorCode::IncompatibleVersion,"Unsupported render-reference component version");
  MeshReference ref; bool asset{},first{},second{};
  for(const auto& field:component.fields){
    if(field.id>=1&&field.id<=3){
      const auto* value=std::get_if<PersistentId>(&field.value);
      if(!value)return fail(ErrorCode::InvalidInput,"Render reference field has the wrong type");
      if(field.id==1){ref.asset=*value;asset=true;}
      if(field.id==2){put_half(ref.revision,0,*value);first=true;}
      if(field.id==3){put_half(ref.revision,16,*value);second=true;}
    }
  }
  if(!asset||!first||!second||!ref.valid())return fail(ErrorCode::InvalidInput,"Render reference requires a stable asset and full nonzero revision");
  return ref;
}
Result<void> frozen() { return fail(ErrorCode::Busy,"Stop Play before editing the authoring scene"); }
}
bool MeshReference::valid() const noexcept {return asset.valid()&&std::any_of(revision.begin(),revision.end(),[](auto b){return b!=std::byte{};});}
Result<void> validate_lighting(const LightingSettings& s){
  if(s.light){
    const auto& l=*s.light;
    if(!scene::finite(l.direction)||std::hypot(l.direction.x,l.direction.y,l.direction.z)<1e-10)
      return fail(ErrorCode::InvalidInput,"DirectionalLight direction must be a finite nonzero vector");
    if(!within(l.illuminance_lux,0,render::physical_illuminance_max_lux))return fail(ErrorCode::InvalidInput,"DirectionalLight illuminance outside the declared lux bounds");
  }
  if(s.output){
    const auto& o=*s.output;
    if(o.profile!=render::LightingProfile::LegacyViewportV1&&o.profile!=render::LightingProfile::PhysicalDiffuseV1)return fail(ErrorCode::Unsupported,"Unknown RenderOutput lighting profile");
    if(o.target!=render::OutputTarget::DisplaySdr&&o.target!=render::OutputTarget::DisplayScrgb)return fail(ErrorCode::Unsupported,"Unknown RenderOutput display target");
    if(!within(o.environment_cd_m2,0,render::physical_environment_max_cd_m2))return fail(ErrorCode::InvalidInput,"RenderOutput environment luminance outside the declared bounds");
    if(!within(o.exposure_stops,-render::physical_exposure_max_stops,render::physical_exposure_max_stops))return fail(ErrorCode::InvalidInput,"RenderOutput exposure outside the declared stop range");
    if(!within(o.reference_luminance_cd_m2,render::physical_reference_min_cd_m2,render::physical_reference_max_cd_m2))return fail(ErrorCode::InvalidInput,"RenderOutput reference luminance outside the declared bounds");
    if(!within(o.peak_nits,render::physical_hdr_paper_white_min_nits,render::physical_hdr_peak_max_nits)||!within(o.paper_white_nits,render::physical_hdr_paper_white_min_nits,o.peak_nits))
      return fail(ErrorCode::InvalidInput,"RenderOutput requires 1 <= paper white <= peak <= 10000 nits");
    if(!scene::finite(o.background_cd_m2))return fail(ErrorCode::InvalidInput,"RenderOutput background must be finite");
    for(const double c:{o.background_cd_m2.x,o.background_cd_m2.y,o.background_cd_m2.z})
      if(!within(c,0,render::physical_background_max_cd_m2))return fail(ErrorCode::InvalidInput,"RenderOutput background radiance outside the declared bounds");
  }
  return {};
}
Result<LightingSettings> lighting_settings(const scene::Document& doc){
  try{
    LightingSettings out;
    for(const auto& object:doc.objects)for(const auto& c:object.components){
      if(c.type==light_type){
        if(out.light)return fail(ErrorCode::InvalidInput,"A Scene carries at most one DirectionalLight component");
        auto light=light_from(c);if(!light)return light.error();out.light=light.value();out.light_object=object.id;
      }else if(c.type==output_type){
        if(out.output)return fail(ErrorCode::InvalidInput,"A Scene carries at most one RenderOutput component");
        auto output=output_from(c);if(!output)return output.error();out.output=output.value();out.output_object=object.id;
      }
    }
    if(out.light&&out.output&&out.light_object!=out.output_object)return fail(ErrorCode::InvalidInput,"Lighting settings must share one settings object");
    auto valid=validate_lighting(out);if(!valid)return valid.error();return out;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Lighting settings extraction allocation failed");}
}
Result<std::vector<ObjectRow>> preview_objects(std::vector<ObjectRow> rows,const ObjectRow& preview){
  try{
    const scene::Limits limits;
    if(rows.size()>limits.objects)return fail(ErrorCode::Exhausted,"Preview object capacity exceeded");
    auto valid=scene::validate(preview.local);if(!valid)return valid.error();
    const auto none=rows.size();
    std::vector<std::size_t> order;order.reserve(rows.size());
    for(std::size_t i=0;i<rows.size();++i){
      if(!rows[i].id.valid())return fail(ErrorCode::InvalidInput,"Preview object has no identity");
      order.push_back(i);
    }
    const auto less=[](PersistentId a,PersistentId b){return a.high<b.high||(a.high==b.high&&a.low<b.low);};
    std::sort(order.begin(),order.end(),[&](auto a,auto b){return less(rows[a].id,rows[b].id);});
    for(std::size_t i=1;i<order.size();++i)if(rows[order[i-1]].id==rows[order[i]].id)
      return fail(ErrorCode::Duplicate,"Duplicate preview object identity");
    const auto find=[&](PersistentId id){
      const auto it=std::lower_bound(order.begin(),order.end(),id,[&](auto i,auto key){return less(rows[i].id,key);});
      return it!=order.end()&&rows[*it].id==id?*it:none;
    };
    const auto selected=find(preview.id);
    if(selected==none||rows[selected].handle!=preview.handle||rows[selected].parent!=preview.parent)
      return fail(ErrorCode::StaleHandle,"Preview identity or parent changed");
    struct Node{std::size_t parent,first,next,depth{};bool affected{};};
    std::vector<Node> nodes(rows.size(),Node{none,none,none});
    std::vector<std::size_t> queue;queue.reserve(rows.size());
    for(std::size_t i=0;i<rows.size();++i){
      if(!rows[i].parent.valid()){queue.push_back(i);continue;}
      const auto parent=find(rows[i].parent);
      if(parent==none)return fail(ErrorCode::InvalidDependency,"Preview parent is missing");
      nodes[i].parent=parent;nodes[i].next=nodes[parent].first;nodes[parent].first=i;
    }
    for(std::size_t head=0;head<queue.size();++head){
      const auto i=queue[head];auto& node=nodes[i];
      node.depth=node.parent==none?1:nodes[node.parent].depth+1;
      if(node.depth>limits.hierarchy_depth)return fail(ErrorCode::Exhausted,"Preview hierarchy depth exceeded");
      node.affected=i==selected||(node.parent!=none&&nodes[node.parent].affected);
      if(node.affected){
        if(i==selected)rows[i].local=preview.local;
        const auto local=scene::affine(rows[i].local);if(!local)return local.error();
        if(node.parent==none)rows[i].world=local.value();
        else {const auto world=scene::compose(rows[node.parent].world,local.value());if(!world)return world.error();rows[i].world=world.value();}
      }
      for(auto child=node.first;child!=none;child=nodes[child].next)queue.push_back(child);
    }
    if(queue.size()!=rows.size())return fail(ErrorCode::Cycle,"Preview hierarchy cycle");
    return rows;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Preview composition allocation failed");}
}
namespace {
Result<std::vector<MeshReference>> component_references(const scene::Document& doc,scene::TypeId target){
  try{
    const scene::Limits limits;
    if(doc.objects.size()>limits.objects)return fail(ErrorCode::Exhausted,"Scene object capacity exceeded");
    std::size_t components{};std::vector<MeshReference> out;
    for(const auto& object:doc.objects){
      if(object.components.size()>scene::max_types||object.components.size()>limits.components-components)
        return fail(ErrorCode::Exhausted,"Scene component capacity exceeded");
      components+=object.components.size();bool box{},mesh{},material{};
      for(const auto& component:object.components){
        if(component.fields.size()>scene::max_fields)return fail(ErrorCode::Exhausted,"Scene field capacity exceeded");
        if(component.type==box_type){if(box)return fail(ErrorCode::InvalidInput,"Duplicate Box component");box=true;}
        if(component.type==mesh_type||component.type==material_type){
          auto& present=component.type==mesh_type?mesh:material;
          if(present)return fail(ErrorCode::InvalidInput,"Duplicate render reference component");present=true;
          auto ref=reference(component);if(!ref)return ref.error();
          if(component.type==target)out.push_back(ref.value());
        }
      }
      if(box&&mesh)return fail(ErrorCode::InvalidInput,"An object cannot contain both Box and Mesh components");
      if(material&&!box&&!mesh)return fail(ErrorCode::InvalidInput,"Material requires a Box or Mesh component");
    }
    return out;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Scene reference extraction allocation failed");}
}
}
Result<std::vector<MeshReference>> mesh_references(const scene::Document& doc){return component_references(doc,mesh_type);}
Result<std::vector<MaterialReference>> material_references(const scene::Document& doc){return component_references(doc,material_type);}
Result<std::optional<MaterialReference>> object_material(const scene::Object& object){
  std::optional<MaterialReference> out;
  for(const auto& c:object.components)if(c.type==material_type){
    if(out)return fail(ErrorCode::InvalidInput,"Duplicate Material component");
    auto ref=reference(c);if(!ref)return ref.error();out=ref.value();
  }
  return out;
}
scene::World& Document::world(bool play) const { return *(play && play_ ? play_ : world_); }
Result<std::unique_ptr<Document>> Document::create() {
  try {
    auto memory=TrackedAllocator::create({256u*1024u*1024u}); if(!memory) return memory.error();
    auto out=std::unique_ptr<Document>(new Document(std::move(memory).value()));
    auto schemas=scene::Schemas::create({box_schema(),mesh_schema(),material_schema(),light_schema(),output_schema()});
    if(!schemas) return schemas.error(); out->schemas_=std::move(schemas).value();
    auto w=scene::World::create(out->memory_,out->schemas_); if(!w) return w.error();
    out->world_=std::move(w).value();
    auto id=out->add_box(); if(!id) return id.error();
    auto rows=out->objects(); if(!rows) return rows.error();
    auto row=rows.value().front(); row.local.translation.y=.5;
    auto edit=out->edit(row,out->generation()); if(!edit) return edit.error();
    auto clear=out->world_->clear_history(); if(!clear) return clear.error();
    return out;
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"desktop document allocation failed"); }
}
Document::~Document() {
  if(play_) { auto stopped=play_->stop(std::chrono::milliseconds{1000}); (void)stopped; }
  if(world_) { auto stopped=world_->stop(std::chrono::milliseconds{1000}); (void)stopped; }
}
Result<std::vector<ObjectRow>> Document::objects(bool play) const {
  auto extraction=extract(play);if(!extraction)return extraction.error();return std::move(extraction.value().rows);
}
Result<LightingSettings> Document::lighting(bool play) const {
  auto extraction=extract(play);if(!extraction)return extraction.error();return std::move(extraction.value().lighting);
}
Result<SceneExtraction> Document::extract(bool play) const {
  try {
    std::lock_guard lock(extraction_mutex_);
    auto view=world(play).read(); if(!view) return view.error();
    const bool dynamic_play=play&&play_;
    auto& cache=authoring_cache_;
    const auto generation=view.value().generation();
    if(!dynamic_play&&cache.valid&&cache.generation==generation) return SceneExtraction{cache.rows,cache.lighting};
    SceneExtraction extraction; auto& rows=extraction.rows; rows.reserve(view.value().objects().size());
    for(auto handle:view.value().objects()) {
      auto object=view.value().object(handle); if(!object) return object.error();
      auto transform=view.value().world_transform(handle); if(!transform) return transform.error();
      ObjectRow row; row.id=object.value().id; row.handle=handle; row.world=transform.value();
      for(auto& c:object.value().components) {
        if(c.type==light_type) {
          if(extraction.lighting.light) return fail(ErrorCode::InvalidInput,"A Scene carries at most one DirectionalLight component");
          auto light=light_from(c); if(!light) return light.error();
          extraction.lighting.light=light.value(); extraction.lighting.light_object=row.id; row.lighting=true; continue;
        }
        if(c.type==output_type) {
          if(extraction.lighting.output) return fail(ErrorCode::InvalidInput,"A Scene carries at most one RenderOutput component");
          auto output=output_from(c); if(!output) return output.error();
          extraction.lighting.output=output.value(); extraction.lighting.output_object=row.id; row.lighting=true; continue;
        }
        for(auto& f:c.fields) {
        if(c.type==scene::transform_type) {
          if(f.id==scene::translation_field) row.local.translation=std::get<scene::Vec3>(f.value);
          if(f.id==scene::rotation_field) row.local.rotation=std::get<scene::Quat>(f.value);
          if(f.id==scene::scale_field) row.local.scale=std::get<scene::Vec3>(f.value);
          if(f.id==scene::parent_field) row.parent=std::get<scene::Reference>(f.value).object;
        } else if(c.type==mesh_type) {
          row.visible=true;
          if(!row.mesh){auto ref=reference(c);if(!ref)return ref.error();row.mesh=ref.value();}
          if(f.id==4)row.color=std::get<scene::Vec3>(f.value);
        } else if(c.type==material_type) {
          if(!row.material){auto ref=reference(c);if(!ref)return ref.error();row.material=ref.value();}
        } else if(c.type==box_type) {
          row.visible=true;
          if(f.id==1) row.dimensions=std::get<scene::Vec3>(f.value);
          if(f.id==2) row.color=std::get<scene::Vec3>(f.value);
        }
        }
      }
      rows.push_back(row);
    }
    if(extraction.lighting.light&&extraction.lighting.output&&extraction.lighting.light_object!=extraction.lighting.output_object)
      return fail(ErrorCode::InvalidInput,"Lighting settings must share one settings object");
    auto valid=validate_lighting(extraction.lighting); if(!valid) return valid.error();
    if(dynamic_play) return extraction;
    // Build both owned copies before publication. On any failure the previous
    // cache remains intact, but its old generation can never satisfy this view.
    auto returned=extraction;
    cache.rows.swap(extraction.rows);cache.lighting=extraction.lighting;cache.generation=generation;cache.valid=true;
    return returned;
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"desktop extraction allocation failed"); }
}
Result<void> Document::set_lighting(const LightingSettings& requested,std::uint64_t expected){
  try{
    if(play_)return frozen();
    if(expected!=generation())return fail(ErrorCode::StaleHandle,"Scene changed during this edit");
    auto valid=validate_lighting(requested);if(!valid)return valid;
    PersistentId existing;scene::ObjectHandle handle;bool has_light{},has_output{};
    {
      auto view=world_->read();if(!view)return view.error();
      for(auto h:view.value().objects()){
        auto object=view.value().object(h);if(!object)return object.error();
        for(const auto& c:object.value().components)if(c.type==light_type||c.type==output_type){
          if(existing.valid()&&existing!=object.value().id)return fail(ErrorCode::InvalidInput,"Lighting settings span more than one object");
          existing=object.value().id;handle=h;(c.type==light_type?has_light:has_output)=true;
        }
      }
    }
    scene::Transaction tx;
    if(!requested.light&&!requested.output){
      if(!existing.valid())return {};
      tx.commands={scene::Command::destroy(existing,handle)};return world_->apply(tx);
    }
    if(!existing.valid()){
      if(next_id_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Scene object ID exhausted");
      const PersistentId id{id_namespace,next_id_};
      scene::Object object{id,{},{scene::transform_component({})},{}};
      if(requested.light)object.components.push_back(light_component(*requested.light));
      if(requested.output)object.components.push_back(output_component(*requested.output));
      tx.commands={scene::Command::create(std::move(object))};
      auto applied=world_->apply(tx);if(!applied)return applied;++next_id_;return {};
    }
    const auto put=[&](const scene::Component& component){scene::Command c;c.kind=scene::CommandKind::PutComponent;c.target=existing;c.expected_handle=handle;c.component=component;tx.commands.push_back(std::move(c));};
    const auto remove=[&](scene::TypeId type){scene::Command c;c.kind=scene::CommandKind::RemoveComponent;c.target=existing;c.expected_handle=handle;c.type=type;tx.commands.push_back(std::move(c));};
    if(requested.light)put(light_component(*requested.light));else if(has_light)remove(light_type);
    if(requested.output)put(output_component(*requested.output));else if(has_output)remove(output_type);
    return world_->apply(tx);
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Lighting settings edit allocation failed");}
}
std::uint64_t Document::generation(bool play) const { return world(play).stats().generation; }
Result<void> Document::reset() {
  if(play_) return frozen();
  auto fresh=create(); if(!fresh) return fresh.error();
  auto doc=fresh.value()->world_->document(); if(!doc) return doc.error();
  if(next_id_==std::numeric_limits<std::uint64_t>::max()) return fail(ErrorCode::Overflow,"box ID exhausted");
  doc.value().objects.front().id={id_namespace,next_id_};
  auto loaded=world_->load(doc.value()); if(loaded) ++next_id_; return loaded;
}
scene::LocalOrigin Document::origin(bool play) const {
  auto view=world(play).read(); if(!view) return {}; return view.value().origin();
}
Result<PersistentId> Document::add_box() {
  try {
  if(play_) return frozen().error();
  if(next_id_==std::numeric_limits<std::uint64_t>::max()) return fail(ErrorCode::Overflow,"box ID exhausted");
  PersistentId id{id_namespace,next_id_++};
  scene::LocalTransform transform; transform.translation={0,.5,0};
  scene::Object object{id,{}, {scene::transform_component(transform),
    {box_type,1,{{1,scene::Vec3{1,1,1}},{2,scene::Vec3{.28,.52,.78}}}}}, {}};
  auto result=world_->apply({{scene::Command::create(std::move(object))}});
  if(!result) return result.error(); return id;
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"box creation allocation failed"); }
}
Result<PersistentId> Document::add_mesh(const MeshReference& ref){
  try{
    if(play_)return frozen().error();
    if(!ref.valid())return fail(ErrorCode::InvalidInput,"Mesh requires an asset ID and revision");
    if(next_id_==std::numeric_limits<std::uint64_t>::max())return fail(ErrorCode::Overflow,"Scene object ID exhausted");
    const PersistentId id{id_namespace,next_id_};
    scene::Object object{id,{}, {scene::transform_component({}),
      {mesh_type,1,{{1,ref.asset},{2,half(ref.revision,0)},{3,half(ref.revision,16)},{4,scene::Vec3{.28,.52,.78}}}}}, {}};
    auto applied=world_->apply({{scene::Command::create(std::move(object))}});if(!applied)return applied.error();
    ++next_id_;return id;
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Mesh object creation allocation failed");}
}
Result<void> Document::assign_material(PersistentId id,std::optional<MaterialReference> ref){
  try{
    if(play_)return frozen();
    if(ref&&!ref->valid())return fail(ErrorCode::InvalidInput,"Material requires an asset ID and full revision");
    scene::ObjectHandle handle;
    {
      auto view=world_->read();if(!view)return view.error();
      auto found=view.value().find(id);if(!found)return found.error();handle=found.value();
      auto object=view.value().object(handle);if(!object)return object.error();
      if(std::none_of(object.value().components.begin(),object.value().components.end(),[](const auto& c){return c.type==box_type||c.type==mesh_type;}))
        return fail(ErrorCode::InvalidInput,"Material requires a renderable object");
      auto current=object_material(object.value());if(!current)return current.error();
      if(current.value()==ref)return {};
    }
    scene::Command cmd;cmd.target=id;cmd.expected_handle=handle;
    if(ref){cmd.kind=scene::CommandKind::PutComponent;
      cmd.component={material_type,1,{{1,ref->asset},{2,half(ref->revision,0)},{3,half(ref->revision,16)}}};}
    else{cmd.kind=scene::CommandKind::RemoveComponent;cmd.type=material_type;}
    return world_->apply({{std::move(cmd)}});
  }catch(const std::bad_alloc&){return fail(ErrorCode::Exhausted,"Material assignment allocation failed");}
}
Result<void> Document::edit(const ObjectRow& row,std::uint64_t expected) {
  try {
  if(play_) return frozen();
  if(expected!=generation()) return fail(ErrorCode::StaleHandle,"Scene changed during this edit");
  auto valid=scene::validate(row.local); if(!valid) return valid;
  scene::Transaction tx;
  tx.commands={scene::Command::set(row.id,1,1,row.local.translation,row.handle),
    scene::Command::set(row.id,1,2,row.local.rotation,row.handle),
    scene::Command::set(row.id,1,3,row.local.scale,row.handle)};
  if(row.mesh){
    if(!row.mesh->valid())return fail(ErrorCode::InvalidInput,"Invalid Mesh reference edit");
    tx.commands.push_back(scene::Command::set(row.id,mesh_type,1,row.mesh->asset,row.handle));
    tx.commands.push_back(scene::Command::set(row.id,mesh_type,2,half(row.mesh->revision,0),row.handle));
    tx.commands.push_back(scene::Command::set(row.id,mesh_type,3,half(row.mesh->revision,16),row.handle));
    tx.commands.push_back(scene::Command::set(row.id,mesh_type,4,row.color,row.handle));
  }else if(row.visible) {
    tx.commands.push_back(scene::Command::set(row.id,box_type,1,row.dimensions,row.handle));
    tx.commands.push_back(scene::Command::set(row.id,box_type,2,row.color,row.handle));
  }
  return world_->apply(tx);
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"box edit allocation failed"); }
}
Result<void> Document::remove(PersistentId id,scene::ObjectHandle handle,std::uint64_t expected) {
  if(play_) return frozen();
  if(expected!=generation()) return fail(ErrorCode::StaleHandle,"Scene changed during this edit");
  try { return world_->apply({{scene::Command::destroy(id,handle)}}); }
  catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"box removal allocation failed"); }
}
Result<void> Document::undo() { return play_ ? frozen() : world_->undo(); }
Result<void> Document::redo() { return play_ ? frozen() : world_->redo(); }
Result<void> Document::clear_history() { return play_ ? frozen() : world_->clear_history(); }
PersistenceStamp Document::persistence_stamp() const { return {identity_,generation(),play_epoch_}; }
bool Document::current(const PersistenceStamp& stamp) const {
  return stamp.document==identity_&&stamp.generation==generation()&&stamp.play_epoch==play_epoch_;
}
Result<scene::Document> Document::snapshot() const { return world_->document(); }
Result<void> Document::write(platform::Files& files,std::string_view path,
                              const scene::Document& doc,const scene::Schemas& schemas,bool cooked) {
  auto refs=mesh_references(doc);if(!refs)return refs.error();
  auto lighting=lighting_settings(doc);if(!lighting)return lighting.error();
  if(cooked){
    auto bytes=scene::cook(doc,schemas,{},codec_identity);if(!bytes)return bytes.error();
    return files.replace(path,bytes.value().bytes,platform::Durability::AtomicVisibility);
  }
  auto text=scene::encode_json(doc,schemas,{});if(!text)return text.error();
  return files.replace(path,std::as_bytes(std::span(text.value())),platform::Durability::AtomicVisibility);
}
Result<void> Document::save(platform::Files& files,std::string_view path) const {
  auto doc=snapshot();if(!doc)return doc.error();return write(files,path,doc.value(),*schemas_,false);
}
Result<void> Document::cook(platform::Files& files,std::string_view path) const {
  auto doc=world_->document(); if(!doc) return doc.error();
  return write(files,path,doc.value(),*schemas_,true);
}
Result<void> Document::load(platform::Files& files,std::string_view path) {
  if(play_) return frozen();
  const auto stamp=persistence_stamp();
  auto doc=decode(files,path,*schemas_);if(!doc)return doc.error();
  return install(doc.value(),stamp);
}
Result<scene::Document> Document::decode(platform::Files& files,std::string_view path,const scene::Schemas& schemas) {
  auto bytes=files.read(path,64u*1024u*1024u); if(!bytes) return bytes.error();
  return decode(bytes.value(),schemas);
}
Result<scene::Document> Document::decode(std::span<const std::byte> bytes,const scene::Schemas& schemas) {
  if(bytes.size()>64u*1024u*1024u)return fail(ErrorCode::Exhausted,"Scene input byte limit");
  const bool binary=bytes.size()>=8 && std::memcmp(bytes.data(),"RACESCN1",8)==0;
  auto decoded=binary ? scene::decode_cooked(bytes,schemas,{},codec_identity)
    : scene::decode_json({reinterpret_cast<const char*>(bytes.data()),bytes.size()},schemas,{});
  // Older cooked identities keep their legacy semantics (no lighting settings); cooked
  // descriptors of this identity are incompatible with older binaries by construction.
  if(binary&&!decoded&&decoded.error().code!=ErrorCode::Exhausted){
    auto legacy=scene::Schemas::create({box_schema(),mesh_schema(),material_schema()});if(!legacy)return legacy.error();
    auto old=scene::decode_cooked(bytes,*legacy.value(),{},surface_codec_identity);
    if(old)decoded=std::move(old);
    else if(old.error().code==ErrorCode::Exhausted)return old.error();
  }
  if(binary&&!decoded&&decoded.error().code!=ErrorCode::Exhausted){
    auto legacy=scene::Schemas::create({box_schema(),mesh_schema()});if(!legacy)return legacy.error();
    auto old=scene::decode_cooked(bytes,*legacy.value(),{},"race-desktop-mesh-schema-1");
    if(old)decoded=std::move(old);
    else if(old.error().code==ErrorCode::Exhausted)return old.error();
  }
  if(binary&&!decoded&&decoded.error().code!=ErrorCode::Exhausted){
    auto legacy=scene::Schemas::create({box_schema()});if(!legacy)return legacy.error();
    auto old=scene::decode_cooked(bytes,*legacy.value(),{},"race-desktop-box-schema-1");
    if(old)decoded=std::move(old);
    else if(old.error().code==ErrorCode::Exhausted)return old.error();
  }
  if(!decoded)return decoded.error();
  auto refs=mesh_references(decoded.value());if(!refs)return refs.error();
  auto lighting=lighting_settings(decoded.value());if(!lighting)return lighting.error();return decoded;
}
Result<void> Document::install(const scene::Document& doc,const PersistenceStamp& stamp) {
  if(play_)return frozen();
  if(!current(stamp))return fail(ErrorCode::StaleHandle,"Scene or Play session changed while loading; result discarded");
  auto refs=mesh_references(doc);if(!refs)return refs.error();
  auto lighting=lighting_settings(doc);if(!lighting)return lighting.error();
  std::uint64_t next=next_id_;
  for(const auto& o:doc.objects) if(o.id.high==id_namespace) {
    if(o.id.low==std::numeric_limits<std::uint64_t>::max()) return fail(ErrorCode::Overflow,"box ID exhausted");
    next=std::max(next,o.id.low+1);
  }
  auto loaded=world_->load(doc); if(!loaded) return loaded;
  next_id_=next; return {};
}
Result<void> Document::start_play() {
  if(play_) return fail(ErrorCode::Busy,"Play is already running");
  if(play_epoch_>std::numeric_limits<std::uint64_t>::max()-2)
    return fail(ErrorCode::Overflow,"Play session identity exhausted");
  auto doc=world_->document(); if(!doc) return doc.error();
  auto next=scene::World::create(memory_,schemas_); if(!next) return next.error();
  auto loaded=next.value()->load(doc.value()); if(!loaded) return loaded;
  auto initial=objects(); if(!initial) return initial.error();
  play_target_={}; play_rotation_={};
  for(const auto& row:initial.value()) if(row.visible) { play_target_=row.id;play_rotation_=row.local.rotation;break; }
  { std::lock_guard lock(extraction_mutex_);play_=std::move(next).value(); }
  ++play_epoch_;play_time_=0; return {};
}
Result<void> Document::stop_play() {
  if(!play_) return {};
  auto stopped=play_->stop(std::chrono::milliseconds{1000}); if(!stopped) return stopped;
  { std::lock_guard lock(extraction_mutex_);play_.reset(); }
  ++play_epoch_;
  return {};
}
Result<void> Document::tick_play(double seconds) {
  if(!play_) return fail(ErrorCode::InvalidInput,"Play is not running");
  if(!std::isfinite(seconds)||seconds<=0||seconds>.25) return fail(ErrorCode::InvalidInput,"invalid play step");
  // Lifecycle acceptance sample: no authoring object is modified by simulation.
  // This sample rotates the first visible primitive; it is not a physics solver.
  const double next_time=std::remainder(play_time_+seconds,4*3.14159265358979323846);
  scene::ObjectHandle target;
  {
    auto view=play_->read(); if(!view) return view.error();
    auto found=view.value().find(play_target_);
    if(!found){
      if(found.error().code!=ErrorCode::NotFound) return found.error();
      play_time_=next_time;return {};
    }
    target=found.value();
  }
  // Release the admitted read before publishing; the expected live handle is
  // still validated by the transaction, without extracting unrelated objects.
  auto rotation=scene::axis_angle({0,1,0},next_time*.5); if(!rotation) return rotation.error();
  auto a=play_rotation_,b=rotation.value();
  scene::Quat combined{a.w*b.x+a.x*b.w+a.y*b.z-a.z*b.y,a.w*b.y-a.x*b.z+a.y*b.w+a.z*b.x,
    a.w*b.z+a.x*b.y-a.y*b.x+a.z*b.w,a.w*b.w-a.x*b.x-a.y*b.y-a.z*b.z};
  try {
    auto applied=play_->apply({{scene::Command::set(play_target_,1,2,combined,target)},scene::EditMode::Simulation});
    if(applied) play_time_=next_time; return applied;
  } catch(const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"play command allocation failed"); }
}
}
