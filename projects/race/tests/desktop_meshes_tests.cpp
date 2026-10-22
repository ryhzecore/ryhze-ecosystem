#include "desktop_meshes.hpp"
#include "desktop_persistence.hpp"
#include "mesh_pipeline.hpp"
#include "assets_support.hpp"
#include <algorithm>
#include <string>

namespace {
namespace d=race::desktop;
namespace m=race::assets::mesh;
std::vector<std::byte> product(std::string_view source="v 2 0 0\nv 4 0 0\nv 2 2 0\nf 1 2 3"){
  auto imported=mesh_pipeline::import_obj({91,7},std::as_bytes(std::span(source)));CHECK(imported);
  auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);return std::move(cooked).value();
}
std::vector<std::byte> encoded_product(std::uint32_t revision,std::size_t vertex_count=3,
    std::size_t index_count=3,AssetId id={91,7}){
  static const auto base_stamp=[](){const auto bytes=product();return m::validate(bytes).value().stamp();}();
  auto stamp=base_stamp;const auto label=std::to_string(revision);stamp.source=assets::digest(std::as_bytes(std::span(label)));
  const std::array<m::Vertex,3> triangle{{{{2,0,0},{0,0,1}},{{4,0,0},{0,0,1}},{{2,2,0},{0,0,1}}}};
  std::vector<m::Vertex> vertices;vertices.reserve(vertex_count);
  for(std::size_t i=0;i<vertex_count;++i)vertices.push_back(triangle[i%3]);
  std::vector<std::uint32_t> indices(index_count);
  for(std::size_t i=0;i<index_count;++i)indices[i]=static_cast<std::uint32_t>(i%3);
  auto encoded=m::encode(id,stamp,vertices,indices);CHECK(encoded);return std::move(encoded).value();
}
d::MeshReference reference(const std::vector<std::byte>& bytes){
  auto view=m::validate(bytes);CHECK(view);return {view.value().id(),view.value().revision()};
}
d::MeshReference put(FakeFiles& files,const std::vector<std::byte>& bytes){
  auto view=m::validate(bytes);CHECK(view);const auto leaf=assets::hex(view.value().revision())+".rasset";
  files.data[leaf]=bytes;return {view.value().id(),view.value().revision()};
}
std::unique_ptr<d::Document> document(){auto doc=d::Document::create();CHECK(doc);return std::move(doc).value();}
void scene_contracts(){
  auto doc=document();d::MeshReference ref{{11,22},{}};
  for(std::size_t i=0;i<ref.revision.size();++i)ref.revision[i]=std::byte(i+1);
  auto added=doc->add_mesh(ref);CHECK(added);auto rows=doc->objects();CHECK(rows);
  auto row=rows.value().back();CHECK(row.mesh==ref&&row.visible&&row.dimensions==scene::Vec3{1,1,1});
  row.local.translation={5,6,7};row.color={.1,.2,.3};CHECK(doc->edit(row,doc->generation()));
  CHECK(doc->undo());CHECK(doc->objects().value().back().local.translation==scene::Vec3{});
  CHECK(doc->redo());CHECK(doc->objects().value().back().mesh==ref);
  const auto generation=doc->generation();row.mesh->revision={};CHECK(!doc->edit(row,generation));CHECK(doc->generation()==generation);
  CHECK(!doc->add_mesh({}));CHECK(doc->generation()==generation);
  CHECK_ERROR(doc->edit(rows.value().back(),generation-1),ErrorCode::StaleHandle);
  auto copy=doc->objects().value();copy.back().mesh->revision[0]=std::byte{};CHECK(doc->objects().value().back().mesh==ref);
  CHECK(doc->start_play());CHECK(doc->objects(true).value().back().mesh==ref);CHECK(!doc->add_mesh(ref));CHECK(doc->stop_play());
  FakeFiles files;CHECK(doc->save(files,"scene.json"));CHECK(doc->cook(files,"scene.rscene"));
  auto loaded=document();CHECK(loaded->load(files,"scene.json"));CHECK(loaded->objects().value().back().mesh==ref);
  CHECK(loaded->load(files,"scene.rscene"));CHECK(loaded->objects().value().back().mesh==ref);
  CHECK(loaded->cook(files,"repeat.rscene"));CHECK(files.data["repeat.rscene"]==files.data["scene.rscene"]);
  auto snapshot=doc->snapshot().value();auto bad=snapshot;
  bad.objects.back().components.push_back(snapshot.objects.front().components.back());
  const auto old_file=files.data["scene.rscene"];
  CHECK(!doc->install(bad,doc->persistence_stamp()));CHECK(doc->generation()==generation);
  CHECK(!d::Document::write(files,"scene.rscene",bad,*doc->schemas(),true));CHECK(files.data["scene.rscene"]==old_file);
  bad=snapshot;bad.objects.resize(scene::Limits{}.objects+1);
  CHECK_ERROR(doc->install(bad,doc->persistence_stamp()),ErrorCode::Exhausted);CHECK(doc->generation()==generation);
  CHECK_ERROR(d::Document::write(files,"scene.rscene",bad,*doc->schemas(),true),ErrorCode::Exhausted);CHECK(files.data["scene.rscene"]==old_file);
  bad=snapshot;bad.objects.back().components.resize(scene::max_types+1);
  CHECK_ERROR(d::mesh_references(bad),ErrorCode::Exhausted);
  bad=snapshot;bad.objects.back().components.back().fields.resize(scene::max_fields+1);
  CHECK_ERROR(d::mesh_references(bad),ErrorCode::Exhausted);
  bad=snapshot;for(auto& c:bad.objects.back().components)if(c.type==d::mesh_type)c.fields.erase(c.fields.begin());
  CHECK(!doc->install(bad,doc->persistence_stamp()));
  for(int half=0;half<2;++half){
    auto one=ref;std::fill(one.revision.begin()+half*16,one.revision.begin()+(half+1)*16,std::byte{});
    CHECK(doc->add_mesh(one));CHECK(doc->objects().value().back().mesh==one);
  }
  CHECK(doc->reset());CHECK(!doc->objects().value().front().mesh);
  auto legacy=scene::Schemas::create({{d::box_type,1,"Box",{{1,"dimensions",scene::Vec3{1,1,1},.01,1000,true},{2,"color",scene::Vec3{.28,.52,.78},0,1,true}}}});CHECK(legacy);
  auto old_doc=doc->snapshot().value();auto old=scene::cook(old_doc,*legacy.value(),{},"race-desktop-box-schema-1");CHECK(old);
  files.data["legacy.rscene"]=old.value().bytes;CHECK(loaded->load(files,"legacy.rscene"));CHECK(loaded->objects().value().size()==1);
  auto wrong=scene::cook(old_doc,*legacy.value(),{},"wrong-desktop-identity");CHECK(wrong);files.data["wrong.rscene"]=wrong.value().bytes;
  CHECK(!loaded->load(files,"wrong.rscene"));
  auto wrong_descriptor=scene::cook(old_doc,*doc->schemas(),{},"race-desktop-box-schema-1");CHECK(wrong_descriptor);
  files.data["wrong.rscene"]=wrong_descriptor.value().bytes;CHECK(!loaded->load(files,"wrong.rscene"));
}
void resolver_contracts(){
  auto files=std::make_shared<FakeFiles>();auto a=put(*files,product());
  auto b=put(*files,product("v 2 0 0\nv 5 0 0\nv 2 2 0\nf 1 2 3"));
  CHECK(a.asset==b.asset&&a.revision!=b.revision);
  std::array refs{a,b,a};auto set=d::MeshSet::prepare(files,refs);CHECK(set&&set.value()->entries().size()==2);
  CHECK(set.value()->find(a).value()->bounds.maximum[0]==4&&set.value()->find(b).value()->bounds.maximum[0]==5);
  auto doc=document();CHECK(doc->add_mesh(a));CHECK(doc->add_mesh(b));
  auto frame=d::scene_frame(*doc,set.value().get(),{});CHECK(frame&&frame.value().draws.size()==3);
  CHECK(!frame.value().draws[0].geometry&&frame.value().draws[1].geometry&&frame.value().draws[2].geometry);
  CHECK(frame.value().draws[1].geometry!=frame.value().draws[2].geometry);
  CHECK(!d::scene_frame(*doc,nullptr,{}));
  auto bounds=d::local_bounds(doc->objects().value()[1],set.value().get());CHECK(bounds&&bounds.value().minimum[0]==2);
  const auto reads=files->reads;CHECK(d::MeshSet::prepare(files,refs,set.value()));CHECK(files->reads==reads);
  CHECK(d::mesh_file_reference(*files,assets::hex(a.revision)+".rasset").value()==a);
  CHECK(!d::mesh_file_reference(*files,"../escape.rasset"));
  auto missing=a;missing.revision[0]^=std::byte{1};CHECK(!d::MeshSet::prepare(files,std::span(&missing,1)));
  auto wrong_id=a;wrong_id.asset.low++;CHECK(!d::MeshSet::prepare(files,std::span(&wrong_id,1)));
  auto other=std::make_shared<FakeFiles>();CHECK(!d::MeshSet::prepare(other,refs,set.value()));
  const auto leaf=assets::hex(a.revision)+".rasset";files->data[leaf]=product("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3");
  CHECK(!d::MeshSet::prepare(files,std::span(&a,1)));CHECK(set.value()->find(a).value()->bounds.maximum[0]==4);
  files->data[leaf].back()^=std::byte{1};CHECK(!d::MeshSet::prepare(files,std::span(&a,1)));
  auto imported=mesh_pipeline::import_obj(a.asset,std::as_bytes(std::span(std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3"))));CHECK(imported);
  auto imp=put(*files,imported.value());CHECK(!d::MeshSet::prepare(files,std::span(&imp,1)));
  CHECK(!d::MeshSet::prepare(nullptr,refs));CHECK(d::MeshSet::prepare(nullptr,{}));
}
void instance_capacity(){
  auto small=product();auto view=m::validate(small);CHECK(view);
  std::vector<m::Vertex> vertices;for(std::size_t i=0;i<view.value().vertex_count();++i)vertices.push_back(view.value().vertex(i).value());
  std::vector<std::uint32_t> indices(render::viewport_mesh_frame_indices);
  for(std::size_t i=0;i<indices.size();++i)indices[i]=static_cast<std::uint32_t>(i%3);
  auto bytes=m::encode(view.value().id(),view.value().stamp(),vertices,indices);CHECK(bytes);
  auto files=std::make_shared<FakeFiles>();auto ref=put(*files,bytes.value());std::array refs{ref,ref};
  auto set=d::MeshSet::prepare(files,refs);CHECK(set&&set.value()->entries().size()==1);
  CHECK(set.value()->validate_instances(std::span(&ref,1)));
  CHECK_ERROR(set.value()->validate_instances(refs),ErrorCode::Exhausted);
}
void extension_contracts(){
  auto files=std::make_shared<FakeFiles>();auto a_bytes=product();const auto a=put(*files,a_bytes);
  auto b_bytes=product("v 2 0 0\nv 5 0 0\nv 2 2 0\nf 1 2 3");const auto b=reference(b_bytes);
  const auto other=put(*files,encoded_product(1,3,3,{91,8}));
  std::weak_ptr<const render::ViewportGeometry> old_packet;render::DrawPrimitive retained_draw;
  std::shared_ptr<const d::MeshSet> compact;
  {
    const std::array refs{a,other};auto prepared=d::MeshSet::prepare(files,refs);CHECK(prepared);
    auto prior=std::move(prepared).value();const auto reads=files->reads;files->data.clear();files->throw_read=true;
    auto same=d::MeshSet::extend(prior,a_bytes);CHECK(same&&same.value()==prior);
    auto updated=d::MeshSet::extend(prior,b_bytes);CHECK(updated);auto extended=std::move(updated).value();
    CHECK(extended!=prior&&extended->files()==files&&extended->entries().size()==3);
    CHECK(prior->entries().size()==2&&!prior->find(b));
    CHECK(extended->find(a).value()->geometry==prior->find(a).value()->geometry);
    CHECK(extended->find(other).value()->geometry==prior->find(other).value()->geometry);
    CHECK(extended->find(b).value()->bounds.maximum[0]==5);
    CHECK(extended->find(b).value()->geometry!=extended->find(a).value()->geometry);
    CHECK(d::MeshSet::extend(extended,b_bytes).value()==extended);
    auto bad=a_bytes;bad.back()^=std::byte{1};CHECK(!d::MeshSet::extend(extended,bad));
    CHECK(!d::MeshSet::extend(extended,{}));CHECK_ERROR(d::MeshSet::extend(nullptr,b_bytes),ErrorCode::InvalidInput);
    auto empty=d::MeshSet::prepare(nullptr,{});CHECK(empty);
    CHECK_ERROR(d::MeshSet::extend(empty.value(),b_bytes),ErrorCode::InvalidInput);
    const auto foreign=encoded_product(2,3,3,{91,9});
    CHECK_ERROR(d::MeshSet::extend(extended,foreign),ErrorCode::InvalidInput);
    auto imported=mesh_pipeline::import_obj(a.asset,std::as_bytes(std::span(std::string_view("v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3"))));CHECK(imported);
    CHECK_ERROR(d::MeshSet::extend(extended,imported.value()),ErrorCode::InvalidInput);
    // Resign an unsupported target: rejection must be semantic, not just checksum failure.
    bad=b_bytes;bad[28]=std::byte{2};
    const auto checksum=assets::digest(std::span<const std::byte>(bad).first(bad.size()-m::checksum_bytes));
    std::copy(checksum.begin(),checksum.end(),bad.end()-m::checksum_bytes);
    CHECK_ERROR(d::MeshSet::extend(extended,bad),ErrorCode::IncompatibleVersion);
    CHECK(extended->entries().size()==3&&prior->entries().size()==2);
    // Supplied bytes are temporary; the new render packet owns its copied data.
    std::fill(b_bytes.begin(),b_bytes.end(),std::byte{});
    CHECK(extended->find(b).value()->geometry->vertices()[1].position[0]==5);
    d::ObjectRow row;row.mesh=a;auto draw=d::mesh_draw(row,extended.get());CHECK(draw);
    retained_draw=std::move(draw).value();old_packet=retained_draw.geometry;
    auto compacted=d::MeshSet::prepare(files,std::span(&b,1),extended);CHECK(compacted);compact=std::move(compacted).value();
    CHECK(compact->entries().size()==1&&!compact->find(a)&&!compact->find(other));
    CHECK(compact->find(b).value()->geometry==extended->find(b).value()->geometry);
    CHECK(files->reads==reads&&files->writes==0);
  }
  CHECK(!old_packet.expired()&&retained_draw.geometry->vertices()[1].position[0]==4);
  CHECK(retained_draw.geometry->indices().size()==3&&compact->find(b).value()->bounds.maximum[0]==5);
  retained_draw.geometry.reset();CHECK(old_packet.expired());
}
void extension_revision_capacity(){
  auto files=std::make_shared<FakeFiles>();auto bytes=encoded_product(0);const auto ref=put(*files,bytes);
  auto prepared=d::MeshSet::prepare(files,std::span(&ref,1));CHECK(prepared);auto set=std::move(prepared).value();
  const auto first=set->find(ref).value()->geometry;const auto reads=files->reads;files->data.clear();files->throw_read=true;
  for(std::uint32_t i=1;i<128;++i){
    bytes=encoded_product(i);auto next=d::MeshSet::extend(set,bytes);CHECK(next);set=std::move(next).value();
    CHECK(set->entries().size()==i+1&&set->find(ref).value()->geometry==first);
  }
  CHECK(d::MeshSet::extend(set,bytes).value()==set);
  CHECK_ERROR(d::MeshSet::extend(set,encoded_product(128)),ErrorCode::Exhausted);
  CHECK(set->entries().size()==128&&set->find(ref).value()->geometry==first&&files->reads==reads);
}
void extension_payload_capacity(){
  // Derive the boundary from the owned Render packet, independently of the
  // native wire stride. UVs enlarge GeometryVertex even for legacy mesh products.
  constexpr std::size_t budget=64*1024*1024;
  constexpr auto vertex_bytes=sizeof(render::GeometryVertex),index_bytes=sizeof(std::uint32_t);
  constexpr auto triangle_bytes=3*index_bytes;
  constexpr auto maximum_packet=render::viewport_mesh_max_vertices*vertex_bytes+render::viewport_mesh_max_indices*index_bytes;
  constexpr auto full_packets=(budget-3*vertex_bytes-triangle_bytes)/maximum_packet;
  constexpr auto remaining=budget-full_packets*maximum_packet;
  std::size_t tail_vertices=std::min(render::viewport_mesh_max_vertices,(remaining-triangle_bytes)/vertex_bytes);
  while(tail_vertices>=3&&(remaining-tail_vertices*vertex_bytes)%triangle_bytes) --tail_vertices;
  CHECK(tail_vertices>=3);
  const auto tail_indices=(remaining-tail_vertices*vertex_bytes)/index_bytes;
  CHECK(tail_indices>=6&&tail_indices+3<=render::viewport_mesh_max_indices&&tail_indices%3==0);
  CHECK(full_packets>0&&full_packets+2<128);
  CHECK(full_packets*maximum_packet+tail_vertices*vertex_bytes+tail_indices*index_bytes==budget);
  const auto payload=[](const d::MeshSet& set){
    std::size_t bytes{};
    for(const auto& entry:set.entries())bytes+=entry.geometry->vertices().size()*sizeof(render::GeometryVertex)+entry.geometry->indices().size()*sizeof(std::uint32_t);
    return bytes;
  };
  auto files=std::make_shared<FakeFiles>();
  auto bytes=encoded_product(0,render::viewport_mesh_max_vertices,render::viewport_mesh_max_indices);
  const auto ref=put(*files,bytes);auto prepared=d::MeshSet::prepare(files,std::span(&ref,1));CHECK(prepared);
  auto set=std::move(prepared).value();const auto reads=files->reads;files->data.clear();files->throw_read=true;
  for(std::uint32_t i=1;i<full_packets;++i){
    bytes=encoded_product(i,render::viewport_mesh_max_vertices,render::viewport_mesh_max_indices);
    auto next=d::MeshSet::extend(set,bytes);CHECK(next);set=std::move(next).value();
  }
  const auto revision=static_cast<std::uint32_t>(full_packets);
  {
    auto below=d::MeshSet::extend(set,encoded_product(revision,tail_vertices,tail_indices-3));CHECK(below);
    CHECK(payload(*below.value())==budget-triangle_bytes&&below.value()->entries().size()==full_packets+1);
  }
  CHECK_ERROR(d::MeshSet::extend(set,encoded_product(revision,tail_vertices,tail_indices+3)),ErrorCode::Exhausted);
  CHECK(payload(*set)==full_packets*maximum_packet&&set->entries().size()==full_packets&&set->find(ref));
  bytes=encoded_product(revision,tail_vertices,tail_indices);
  auto boundary=d::MeshSet::extend(set,bytes);CHECK(boundary);set=std::move(boundary).value();
  CHECK(payload(*set)==budget&&set->entries().size()==full_packets+1);
  CHECK(d::MeshSet::extend(set,bytes).value()==set);
  CHECK_ERROR(d::MeshSet::extend(set,encoded_product(revision+1)),ErrorCode::Exhausted);
  CHECK(payload(*set)==budget&&set->entries().size()==full_packets+1&&set->find(ref)&&files->reads==reads);
  // Compaction permits extension while the original immutable set is retained.
  const auto keep=reference(bytes);auto compact=d::MeshSet::prepare(files,std::span(&keep,1),set);CHECK(compact);
  auto accepted=d::MeshSet::extend(compact.value(),encoded_product(revision+1));CHECK(accepted&&accepted.value()->entries().size()==2);
  CHECK(files->reads==reads);
}
void lighting_frame_contracts(){
  auto doc=document();auto frame=d::scene_frame(*doc,nullptr,{});
  CHECK(frame&&frame.value().lighting_profile==render::LightingProfile::LegacyViewportV1&&frame.value().physical==render::PhysicalLighting{});
  d::LightingSettings s;s.light=d::DirectionalLightSettings{{0,0,2},1234.5,true};s.output=d::RenderOutputSettings{};
  s.output->environment_cd_m2=7;s.output->exposure_stops=.5;s.output->reference_luminance_cd_m2=90;s.output->background_cd_m2={1,2,3};
  CHECK(doc->set_lighting(s,doc->generation()));
  frame=d::scene_frame(*doc,nullptr,{});CHECK(frame&&frame.value().lighting_profile==render::LightingProfile::PhysicalDiffuseV1);
  const auto& p=frame.value().physical;
  CHECK(p.direction_to_light==std::array<double,3>{0,0,2}&&p.illuminance_lux==1234.5&&p.environment_cd_m2==7&&p.exposure_stops==.5&&
      p.reference_luminance_cd_m2==90&&p.background_cd_m2==std::array<double,3>{1,2,3}&&p.target==render::OutputTarget::DisplaySdr);
  CHECK(frame.value().draws.size()==1); // The settings object is never drawn.
  // A disabled light leaves the environment only; a legacy profile id restores legacy frames with inactive physical fields.
  s.light->enabled=false;CHECK(doc->set_lighting(s,doc->generation()));frame=d::scene_frame(*doc,nullptr,{});
  CHECK(frame&&frame.value().lighting_profile==render::LightingProfile::PhysicalDiffuseV1&&frame.value().physical.illuminance_lux==0);
  s.output->profile=render::LightingProfile::LegacyViewportV1;CHECK(doc->set_lighting(s,doc->generation()));frame=d::scene_frame(*doc,nullptr,{});
  CHECK(frame&&frame.value().lighting_profile==render::LightingProfile::LegacyViewportV1&&frame.value().physical==render::PhysicalLighting{});
  // An authored HDR presentation request rejects at frame extraction instead of being downgraded.
  s.output->profile=render::LightingProfile::PhysicalDiffuseV1;s.output->target=render::OutputTarget::DisplayScrgb;CHECK(doc->set_lighting(s,doc->generation()));
  CHECK_ERROR(d::scene_frame(*doc,nullptr,{}),ErrorCode::Unsupported);
  // Play frames extract the same settings from the isolated World.
  s.output->target=render::OutputTarget::DisplaySdr;CHECK(doc->set_lighting(s,doc->generation()));CHECK(doc->start_play());
  auto play=d::scene_frame(*doc,nullptr,{},true,3);CHECK(play&&play.value().lighting_profile==render::LightingProfile::PhysicalDiffuseV1&&play.value().simulation_tick==3);
  CHECK(doc->stop_play());
}
void worker_dependency_rejection(){
  auto files=std::make_unique<FakeFiles>();auto doc=document();CHECK(doc->save(*files,"scene.json"));
  d::PersistenceTask task(std::move(files));const auto generation=doc->generation();
  const auto owner=std::this_thread::get_id();std::atomic<bool> worker{};
  CHECK(task.start(*doc,d::PersistenceKind::Load,"scene.json",[&](const scene::Document&){worker=std::this_thread::get_id()!=owner;return Result<void>{fail(ErrorCode::NotFound,"missing mesh dependency")};}));
  for(int i=0;i<2000;++i){auto done=task.poll(*doc);CHECK(done);if(done.value()){CHECK_ERROR(done.value()->result,ErrorCode::NotFound);CHECK(worker&&doc->generation()==generation);return;}std::this_thread::sleep_for(1ms);}
  CHECK(false);
}
}
int main(){scene_contracts();resolver_contracts();instance_capacity();extension_contracts();extension_revision_capacity();extension_payload_capacity();lighting_frame_contracts();worker_dependency_rejection();std::puts("desktop mesh Scene, legacy compatibility, pinned resolution, immutable extension and worker rejection passed");}
