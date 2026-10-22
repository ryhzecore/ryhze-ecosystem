#include "../apps/desktop_scene.hpp"
#include "test.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <thread>

using namespace race;
using desktop::Document;
using desktop::ObjectRow;
namespace {
struct MemoryFiles final : platform::Files {
  std::map<std::string, std::vector<std::byte>, std::less<>> data;
  bool fail_read{}, fail_write{};
  Result<std::vector<std::byte>> read(std::string_view path, std::size_t limit) override {
    if (fail_read) return fail(ErrorCode::IoFailure, "injected read failure");
    auto it=data.find(path);
    if (it==data.end()) return fail(ErrorCode::NotFound, "missing test file");
    if (it->second.size()>limit) return fail(ErrorCode::Exhausted, "read budget exceeded");
    return it->second;
  }
  Result<void> replace(std::string_view path, std::span<const std::byte> bytes,
                       platform::Durability durability) override {
    CHECK(durability==platform::Durability::AtomicVisibility);
    if (fail_write) return fail(ErrorCode::IoFailure, "injected replace failure");
    std::vector<std::byte> staged(bytes.begin(),bytes.end());
    data[std::string(path)].swap(staged);
    return {};
  }
};
std::unique_ptr<Document> document() {
  auto d=Document::create(); CHECK(d); return std::move(d).value();
}
std::vector<ObjectRow> rows(const Document& d, bool play=false) {
  auto value=d.objects(play); CHECK(value); return std::move(value).value();
}
ObjectRow find(const Document& d, PersistentId id, bool play=false) {
  auto value=rows(d,play);
  auto it=std::find_if(value.begin(),value.end(),[&](const auto& row){return row.id==id;});
  CHECK(it!=value.end()); return *it;
}
void same_values(const ObjectRow& a, const ObjectRow& b) {
  CHECK(a.id==b.id); CHECK(a.local==b.local); CHECK(a.world==b.world);
  CHECK(a.dimensions==b.dimensions); CHECK(a.color==b.color); CHECK(a.visible==b.visible);
  CHECK(a.parent==b.parent); CHECK(a.mesh==b.mesh);
}
void same_rows(const std::vector<ObjectRow>& a, const std::vector<ObjectRow>& b) {
  CHECK(a.size()==b.size());
  for (const auto& row:a) {
    auto it=std::find_if(b.begin(),b.end(),[&](const auto& other){return row.id==other.id;});
    CHECK(it!=b.end()); same_values(row,*it);
  }
}
void persistence_parity_and_io_failure() {
  auto source=document(); auto second=source->add_box(); CHECK(second);
  auto row=find(*source,second.value());
  row.local.translation={2.25,3.5,-4.75}; row.local.scale={2,3,4};
  row.dimensions={.25,2.5,7}; row.color={.125,.5,.875};
  CHECK(source->edit(row,source->generation()));
  MemoryFiles files;
  CHECK(source->save(files,"authoring.scene.json")); CHECK(source->cook(files,"runtime.rscene"));
  const auto original_json=files.data.at("authoring.scene.json");
  const auto original_binary=files.data.at("runtime.rscene");
  CHECK(!original_json.empty() && original_json.front()==std::byte{'{'});
  CHECK(original_binary.size()>8 && std::memcmp(original_binary.data(),"RACESCN1",8)==0);
  auto from_json=document(), from_binary=document();
  CHECK(from_json->load(files,"authoring.scene.json")); CHECK(from_binary->load(files,"runtime.rscene"));
  same_rows(rows(*source),rows(*from_json)); same_rows(rows(*source),rows(*from_binary));
  CHECK(from_json->save(files,"roundtrip.json"));
  CHECK(files.data.at("roundtrip.json")==original_json);
  CHECK(from_binary->cook(files,"roundtrip.bin"));
  CHECK(files.data.at("roundtrip.bin")==original_binary);
  files.fail_write=true;
  CHECK_ERROR(source->save(files,"authoring.scene.json"),ErrorCode::IoFailure);
  CHECK_ERROR(source->cook(files,"runtime.rscene"),ErrorCode::IoFailure);
  CHECK(files.data.at("authoring.scene.json")==original_json);
  CHECK(files.data.at("runtime.rscene")==original_binary);
}
void corrupt_load_preserves_state_and_history() {
  auto d=document(); auto before=rows(*d);
  auto changed=before.front(); changed.dimensions={4,5,6}; changed.local.translation={7,8,9};
  CHECK(d->edit(changed,d->generation()));
  const auto committed=rows(*d); const auto generation=d->generation();
  MemoryFiles files; CHECK(d->save(files,"json")); CHECK(d->cook(files,"binary"));
  files.data["bad-json"]=files.data.at("json"); files.data["bad-json"].resize(12);
  files.data["bad-binary"]=files.data.at("binary");
  files.data["bad-binary"].back()^=std::byte{1};
  for (const auto* path:{"bad-json","bad-binary"}) {
    CHECK(!d->load(files,path)); CHECK(d->generation()==generation);
    same_rows(rows(*d),committed); CHECK(rows(*d).front().handle==committed.front().handle);
  }
  files.fail_read=true;
  CHECK_ERROR(d->load(files,"json"),ErrorCode::IoFailure);
  CHECK(d->generation()==generation); same_rows(rows(*d),committed);
  CHECK(d->undo()); same_rows(rows(*d),before);
  CHECK(d->redo()); same_rows(rows(*d),committed);
}
void dimensions_scale_and_atomic_history() {
  auto d=document(); const auto initial=rows(*d).front();
  auto edited=initial; edited.dimensions={2,4,8}; edited.local.scale={3,5,7};
  edited.local.translation={1,2,3}; edited.color={.2,.4,.6};
  CHECK(d->edit(edited,d->generation()));
  const auto committed=rows(*d).front();
  CHECK(committed.dimensions==scene::Vec3{2,4,8});
  CHECK(committed.local.scale==scene::Vec3{3,5,7});
  auto affine=scene::affine(edited.local); CHECK(affine); CHECK(committed.world==affine.value());
  CHECK(d->undo()); same_values(rows(*d).front(),initial);
  CHECK(d->redo()); same_values(rows(*d).front(),committed);
  auto invalid=rows(*d).front(); invalid.local.translation={100,200,300};
  invalid.local.scale={11,12,13}; invalid.dimensions={0,1,1};
  const auto generation=d->generation();
  CHECK(!d->edit(invalid,generation)); CHECK(d->generation()==generation);
  same_values(rows(*d).front(),committed);
  // Failed dimensions validation must not publish earlier transform commands or
  // consume a history entry. One undo reverts the complete preceding edit.
  CHECK(d->undo()); same_values(rows(*d).front(),initial);
}
void stale_generation_and_handle() {
  auto d=document(); auto stale=rows(*d).front(); const auto old_generation=d->generation();
  auto edited=stale; edited.color={.9,.2,.1}; CHECK(d->edit(edited,old_generation));
  const auto committed=rows(*d);
  CHECK_ERROR(d->edit(stale,old_generation),ErrorCode::StaleHandle);
  CHECK_ERROR(d->remove(stale.id,stale.handle,old_generation),ErrorCode::StaleHandle);
  same_rows(rows(*d),committed);
  auto current=rows(*d).front(); CHECK(d->remove(current.id,current.handle,d->generation()));
  CHECK(rows(*d).empty()); CHECK(d->undo());
  auto restored=rows(*d).front(); CHECK(restored.id==current.id); CHECK(restored.handle!=current.handle);
  const auto generation=d->generation();
  CHECK_ERROR(d->edit(current,generation),ErrorCode::StaleHandle);
  CHECK_ERROR(d->remove(current.id,current.handle,generation),ErrorCode::StaleHandle);
  CHECK(d->generation()==generation); same_values(rows(*d).front(),restored);
}
void play_isolation_and_edit_rejection() {
  auto d=document(); const auto before=rows(*d); const auto generation=d->generation();
  MemoryFiles files; CHECK(d->save(files,"source"));
  CHECK_ERROR(d->tick_play(.01),ErrorCode::InvalidInput);
  CHECK(d->start_play()); CHECK(d->playing());
  same_rows(before,rows(*d,true)); CHECK(rows(*d,true).front().handle!=before.front().handle);
  CHECK_ERROR(d->start_play(),ErrorCode::Busy);
  CHECK_ERROR(d->add_box(),ErrorCode::Busy);
  CHECK_ERROR(d->edit(before.front(),generation),ErrorCode::Busy);
  CHECK_ERROR(d->remove(before.front().id,before.front().handle,generation),ErrorCode::Busy);
  CHECK_ERROR(d->undo(),ErrorCode::Busy); CHECK_ERROR(d->redo(),ErrorCode::Busy);
  CHECK_ERROR(d->load(files,"source"),ErrorCode::Busy);
  const auto play_generation=d->generation(true);
  for (double seconds:{0.0,-.1,.251,std::numeric_limits<double>::infinity(),std::numeric_limits<double>::quiet_NaN()})
    CHECK_ERROR(d->tick_play(seconds),ErrorCode::InvalidInput);
  CHECK(d->generation(true)==play_generation);
  CHECK(d->tick_play(.2)); CHECK(rows(*d,true).front().local.rotation!=before.front().local.rotation);
  CHECK(d->generation()==generation); same_rows(rows(*d),before);
  CHECK(d->save(files,"during-play")); CHECK(files.data.at("during-play")==files.data.at("source"));
  CHECK(d->stop_play()); CHECK(!d->playing()); same_rows(rows(*d),before);
  CHECK(d->stop_play()); CHECK(d->start_play()); same_rows(rows(*d,true),before); CHECK(d->stop_play());
  CHECK(d->add_box());
}
void next_id_after_reload() {
  auto source=document(); PersistentId greatest{};
  for (int i=0;i<8;++i) {auto id=source->add_box(); CHECK(id); greatest=id.value();}
  MemoryFiles files; CHECK(source->save(files,"json")); CHECK(source->cook(files,"binary"));
  for (const auto* path:{"json","binary"}) {
    auto d=document(); CHECK(d->load(files,path));
    auto added=d->add_box(); CHECK(added); CHECK(added.value().high==greatest.high);
    CHECK(added.value().low==greatest.low+1);
    // Loading an older file must not recycle IDs consumed by this document.
    CHECK(d->load(files,path)); auto next=d->add_box(); CHECK(next);
    CHECK(next.value().low==added.value().low+1);
    const auto row=find(*d,next.value()); CHECK(d->remove(row.id,row.handle,d->generation()));
    auto after_delete=d->add_box(); CHECK(after_delete); CHECK(after_delete.value().low==next.value().low+1);
  }
}
void new_scene_uses_fresh_identity_without_file_io() {
  auto d=document(); const auto defaults=rows(*d).front();
  auto removed_id=d->add_box(); CHECK(removed_id);
  auto removed=find(*d,removed_id.value());
  CHECK(d->remove(removed.id,removed.handle,d->generation()));
  auto edited=rows(*d).front(); edited.dimensions={3,4,5}; edited.local.translation={8,9,10};
  CHECK(d->edit(edited,d->generation()));
  const auto before=rows(*d); const auto generation=d->generation();
  CHECK(d->start_play()); CHECK_ERROR(d->reset(),ErrorCode::Busy);
  same_rows(rows(*d),before); CHECK(d->generation()==generation); CHECK(d->stop_play());
  CHECK(d->reset()); const auto reset_rows=rows(*d); CHECK(reset_rows.size()==1);
  const auto& fresh=reset_rows.front();
  CHECK(fresh.id.high==removed.id.high); CHECK(fresh.id.low>removed.id.low);
  CHECK(fresh.handle!=defaults.handle); CHECK(fresh.local==defaults.local);
  CHECK(fresh.dimensions==defaults.dimensions); CHECK(fresh.color==defaults.color); CHECK(fresh.visible);
  CHECK(!d->undo()); CHECK(!d->redo());
  auto stale=fresh; stale.handle=defaults.handle;
  CHECK_ERROR(d->edit(stale,d->generation()),ErrorCode::StaleHandle);
  auto added=d->add_box(); CHECK(added); CHECK(added.value().low==fresh.id.low+1);
  CHECK(d->reset()); CHECK(rows(*d).front().id.low==added.value().low+1);
}
void play_composes_authored_rotation() {
  auto d=document(); auto authored=rows(*d).front();
  auto initial_rotation=scene::axis_angle({1,0,0},.7); CHECK(initial_rotation);
  authored.local.rotation=initial_rotation.value();
  CHECK(d->edit(authored,d->generation())); const auto original=rows(*d).front();
  CHECK(d->start_play()); CHECK(d->tick_play(.05)); CHECK(d->tick_play(.15));
  // Independent matrix composition verifies the sample adds 0.1 radians of
  // local Y rotation to the authored X rotation instead of replacing it.
  scene::LocalTransform delta;
  auto delta_rotation=scene::axis_angle({0,1,0},.1); CHECK(delta_rotation);
  delta.rotation=delta_rotation.value(); auto delta_matrix=scene::affine(delta); CHECK(delta_matrix);
  auto expected=scene::compose(original.world,delta_matrix.value()); CHECK(expected);
  CHECK(scene::near(rows(*d,true).front().world,expected.value()));
  same_values(rows(*d).front(),original);
  CHECK(d->stop_play()); CHECK(d->start_play()); same_values(rows(*d,true).front(),original);
  CHECK(d->stop_play());
}
void empty_scene_play_has_no_target() {
  auto d=document();const auto initial=rows(*d).front();
  CHECK(d->remove(initial.id,initial.handle,d->generation()));CHECK(rows(*d).empty());
  const auto authoring_generation=d->generation();
  for(int session=0;session<2;++session){
    CHECK(d->start_play());CHECK(rows(*d,true).empty());
    const auto play_generation=d->generation(true);
    for(double seconds:{.01,.05,.25})CHECK(d->tick_play(seconds));
    CHECK(d->generation(true)==play_generation);CHECK(rows(*d,true).empty());
    CHECK(d->generation()==authoring_generation);CHECK(rows(*d).empty());
    CHECK(d->stop_play());CHECK(rows(*d,true).empty());
  }
  auto added=d->add_box();CHECK(added);CHECK(d->start_play());
  const auto before=rows(*d,true);CHECK(d->tick_play(.1));
  const auto after=rows(*d,true);CHECK(after.front().id==added.value());
  CHECK(after.front().local.rotation!=before.front().local.rotation);
  CHECK(d->stop_play());
}
void explicit_history_clear_preserves_scene() {
  auto d=document();
  for(int i=0;i<32;++i) {
    auto row=rows(*d).front();row.local.translation.x=double(i+1);
    CHECK(d->edit(row,d->generation()));
  }
  const auto full=rows(*d);const auto generation=d->generation();
  auto rejected=full.front();rejected.local.translation.x=99;
  CHECK_ERROR(d->edit(rejected,generation),ErrorCode::Exhausted);
  same_rows(rows(*d),full);CHECK(d->generation()==generation);
  CHECK(d->start_play());CHECK_ERROR(d->clear_history(),ErrorCode::Busy);CHECK(d->stop_play());
  CHECK(d->undo());const auto retained=rows(*d);
  MemoryFiles files;CHECK(d->save(files,"before"));
  CHECK(d->clear_history());same_rows(rows(*d),retained);
  CHECK(!d->undo());CHECK(!d->redo());
  CHECK(d->save(files,"after"));CHECK(files.data.at("before")==files.data.at("after"));
  auto edited=rows(*d).front();edited.local.translation.x=123;
  CHECK(d->edit(edited,d->generation()));CHECK(d->undo());same_rows(rows(*d),retained);
}
void extraction_returns_independent_owned_rows() {
  auto d=document();const auto initial=rows(*d);const auto generation=d->generation();
  auto changed_copy=rows(*d);same_rows(changed_copy,initial);
  changed_copy.front().local.translation={90,80,70};changed_copy.front().world.m[3]=999;
  changed_copy.front().dimensions={9,8,7};changed_copy.front().handle={};
  same_rows(rows(*d),initial);CHECK(rows(*d).front().handle==initial.front().handle);
  CHECK(d->generation()==generation);
  auto edited=initial.front();edited.local.translation={1.25,2.5,3.75};
  CHECK(d->edit(edited,generation));const auto committed=rows(*d);
  CHECK(committed.front().local.translation==edited.local.translation);
  auto expected_world=scene::affine(edited.local);CHECK(expected_world);
  CHECK(committed.front().world==expected_world.value());
  // Retained outputs remain owned values across edits, undo/redo and reset.
  CHECK(initial.front().local.translation==scene::Vec3{0,.5,0});
  CHECK(changed_copy.front().local.translation==scene::Vec3{90,80,70});
  CHECK(d->undo());same_rows(rows(*d),initial);
  CHECK(d->redo());same_rows(rows(*d),committed);
  CHECK(d->reset());CHECK(rows(*d).front().id!=initial.front().id);
  CHECK(committed.front().local.translation==edited.local.translation);
}
void extraction_separates_play_world_lifetimes() {
  auto d=document();auto edited=rows(*d).front();edited.color={.1,.2,.3};
  CHECK(d->edit(edited,d->generation()));
  const auto authoring=rows(*d);CHECK(d->start_play());
  const auto first=rows(*d,true);const auto first_generation=d->generation(true);
  same_rows(first,authoring);CHECK(first.front().handle!=authoring.front().handle);
  CHECK(d->stop_play());same_rows(rows(*d,true),authoring);
  CHECK(rows(*d,true).front().handle==authoring.front().handle);
  CHECK(d->start_play());CHECK(d->generation(true)==first_generation);
  const auto recreated=rows(*d,true);same_rows(recreated,authoring);
  CHECK(recreated.front().handle!=first.front().handle);
  // Equal numeric generations in different Worlds must never share rows.
  CHECK(d->generation(true)<d->generation());
  while(d->generation(true)<d->generation())CHECK(d->tick_play(.05));
  CHECK(d->generation(true)==d->generation());const auto moving=rows(*d,true);
  CHECK(moving.front().local.rotation!=authoring.front().local.rotation);
  same_rows(rows(*d),authoring);same_rows(rows(*d,true),moving);
  CHECK(first.front().local.rotation==authoring.front().local.rotation);
  CHECK(d->stop_play());CHECK(d->start_play());same_rows(rows(*d,true),authoring);
  CHECK(d->stop_play());
}
void origin_reload_invalidates_extraction() {
  auto d=document();const auto original=rows(*d);
  const auto origin=d->origin();MemoryFiles files;CHECK(d->save(files,"original"));
  auto edited=original.front();edited.dimensions={2,3,4};CHECK(d->edit(edited,d->generation()));
  const auto before=rows(*d);const auto generation=d->generation();
  const auto& saved=files.data.at("original");
  std::string json(reinterpret_cast<const char*>(saved.data()),saved.size());
  const auto key=json.find("\"origin\"");CHECK(key!=std::string::npos);
  const auto start=json.find('[',key),end=json.find(']',start);
  CHECK(start!=std::string::npos&&end!=std::string::npos);
  json.replace(start,end-start+1,"[1000000000000.25, -20, 30]");
  CHECK(files.replace("rebased",{reinterpret_cast<const std::byte*>(json.data()),json.size()},platform::Durability::AtomicVisibility));
  CHECK(d->load(files,"rebased"));CHECK(d->generation()>generation);
  CHECK(d->origin().position==scene::Vec3{1000000000000.25,-20,30});
  CHECK(d->origin().revision>origin.revision);
  const auto after=rows(*d);same_rows(after,original);
  CHECK(before.front().dimensions==scene::Vec3{2,3,4});
  files.fail_read=true;CHECK_ERROR(d->load(files,"original"),ErrorCode::IoFailure);
  same_rows(rows(*d),after);CHECK(rows(*d).front().handle==after.front().handle);
  CHECK_ERROR(d->edit(before.front(),generation),ErrorCode::StaleHandle);
}
void concurrent_extraction_owns_each_result() {
  auto d=document();const auto expected=rows(*d);
  auto edited=expected.front();edited.color={.1,.2,.3};CHECK(d->edit(edited,d->generation()));
  // Leave this generation uncached so simultaneous readers also exercise the
  // synchronized first publication, then independently mutate cache-hit copies.
  std::array<std::thread,4> readers;
  for(auto& reader:readers)reader=std::thread([&]{
    for(int i=0;i<64;++i){
      auto copy=rows(*d);CHECK(copy.size()==1);CHECK(copy.front().color==edited.color);
      CHECK(copy.front().local==edited.local);CHECK(copy.front().handle==edited.handle);
      copy.front().color={.9,.8,.7};copy.clear();
    }
  });
  for(auto& reader:readers)reader.join();
  CHECK(rows(*d).front().color==edited.color);CHECK(expected.front().color!=edited.color);
}
const ObjectRow& copied_row(std::span<const ObjectRow> values,PersistentId id){
  const auto it=std::find_if(values.begin(),values.end(),[&](const auto& row){return row.id==id;});
  CHECK(it!=values.end());return *it;
}
void parent_preview_matches_scene_without_publication(){
  auto d=document();for(int i=0;i<3;++i)CHECK(d->add_box());
  auto fixture=d->snapshot();CHECK(fixture);auto& objects=fixture.value().objects;
  CHECK(objects.size()==4);const auto root=objects[0].id,child=objects[1].id,grandchild=objects[2].id,other=objects[3].id;
  std::array<scene::LocalTransform,3> transforms;
  transforms[0].translation={1e9,-2e9,3e9};transforms[0].scale={-2,3,.5};
  auto root_rotation=scene::axis_angle({0,1,0},.6);CHECK(root_rotation);transforms[0].rotation=root_rotation.value();
  transforms[1].translation={2,3,4};transforms[1].scale={1,2,3};
  auto child_rotation=scene::axis_angle({1,0,0},.4);CHECK(child_rotation);transforms[1].rotation=child_rotation.value();
  transforms[2].translation={-1,.25,2};
  for(std::size_t i=0;i<3;++i)for(auto& component:objects[i].components)if(component.type==scene::transform_type)
    component=scene::transform_component(transforms[i],{{},i==0?PersistentId{}:objects[i-1].id});
  fixture.value().origin.position={-4e9,2e9,7e9};
  CHECK(d->install(fixture.value(),d->persistence_stamp()));
  const auto authored=rows(*d);CHECK(copied_row(authored,child).parent==root);CHECK(copied_row(authored,grandchild).parent==child);
  // Parent identities survive both codecs and extraction cache hits.
  MemoryFiles files;CHECK(d->save(files,"hierarchy.json"));CHECK(d->cook(files,"hierarchy.rscene"));
  for(const auto* path:{"hierarchy.json","hierarchy.rscene"}){auto loaded=document();CHECK(loaded->load(files,path));same_rows(rows(*loaded),authored);}
  auto history=find(*d,other);history.color={.8,.1,.2};CHECK(d->edit(history,d->generation()));
  const auto before=rows(*d);
  for(const auto selected:{root,child})for(int mode=0;mode<3;++mode){
    auto preview=copied_row(before,selected);
    if(mode==0)preview.local.translation={4,5,6};
    else if(mode==1){auto rotation=scene::axis_angle({0,0,1},.8);CHECK(rotation);preview.local.rotation=rotation.value();}
    else preview.local.scale={.75,-1.25,2.5};
    auto reversed=before;std::reverse(reversed.begin(),reversed.end());
    const auto generation=d->generation();
    auto composed=desktop::preview_objects(reversed,preview);CHECK(composed);
    CHECK(composed.value().size()==reversed.size());for(std::size_t i=0;i<reversed.size();++i)CHECK(composed.value()[i].id==reversed[i].id);
    CHECK(d->generation()==generation);
    same_rows(rows(*d),before);same_values(copied_row(composed.value(),other),copied_row(before,other));
    if(selected==child)same_values(copied_row(composed.value(),root),copied_row(before,root));
    CHECK(copied_row(composed.value(),grandchild).world!=copied_row(before,grandchild).world);
    // Scene's committed hierarchy is the independent oracle for all descendants.
    CHECK(d->edit(preview,d->generation()));same_rows(rows(*d),composed.value());
    CHECK(d->undo());same_rows(rows(*d),before);
  }
  // Discarded previews do not consume the old history entry or the redo entry.
  auto preview=copied_row(before,root);preview.local.translation.x+=.25;
  auto composed=desktop::preview_objects(before,preview);CHECK(composed);
  CHECK(d->undo());same_rows(rows(*d),authored);const auto undone=rows(*d);const auto undone_generation=d->generation();
  preview=copied_row(undone,child);preview.local.translation.y+=2;
  CHECK(desktop::preview_objects(undone,preview));CHECK(d->generation()==undone_generation);
  CHECK(d->redo());same_rows(rows(*d),before);
  // Only the transform belongs to a preview; copied mesh/appearance stays owned.
  preview=copied_row(before,root);preview.color={1,0,0};preview.dimensions={8,9,10};preview.visible=false;
  composed=desktop::preview_objects(before,preview);CHECK(composed);same_rows(composed.value(),before);
  auto owned=before;const auto* storage=owned.data();
  composed=desktop::preview_objects(std::move(owned),copied_row(before,root));CHECK(composed);
  CHECK(composed.value().data()==storage);same_rows(composed.value(),before);
}
void lighting_settings_contracts(){
  auto d=document();
  auto initial=d->lighting();CHECK(initial&&!initial.value().light&&!initial.value().output);
  auto extraction=d->extract();CHECK(extraction&&extraction.value().rows.size()==1&&!extraction.value().lighting.output&&!extraction.value().rows.front().lighting);
  desktop::LightingSettings settings;settings.light=desktop::DirectionalLightSettings{{.3,.9,.2},5000,true};
  settings.output=desktop::RenderOutputSettings{};settings.output->environment_cd_m2=12.5;settings.output->exposure_stops=-1;
  settings.output->reference_luminance_cd_m2=80;settings.output->background_cd_m2={1,2,3};
  CHECK_ERROR(d->set_lighting(settings,d->generation()-1),ErrorCode::StaleHandle);
  CHECK(d->set_lighting(settings,d->generation()));
  auto lit=d->lighting();CHECK(lit&&lit.value().light==settings.light&&lit.value().output==settings.output);
  CHECK(lit.value().light_object.valid()&&lit.value().light_object==lit.value().output_object);
  auto current=rows(*d);CHECK(current.size()==2);
  const auto settings_row=std::find_if(current.begin(),current.end(),[](const auto& row){return row.lighting;});
  CHECK(settings_row!=current.end()&&!settings_row->visible&&settings_row->id==lit.value().light_object);
  // One undo reverts the whole settings edit; redo restores it exactly.
  CHECK(d->undo());CHECK(!d->lighting().value().output&&rows(*d).size()==1);
  CHECK(d->redo());CHECK(d->lighting().value()==lit.value());
  // Updates reuse the one settings object; removing both components destroys it; an empty request is a no-op.
  settings.light->illuminance_lux=750;settings.output->exposure_stops=2;CHECK(d->set_lighting(settings,d->generation()));
  CHECK(d->lighting().value().light->illuminance_lux==750&&d->lighting().value().output->exposure_stops==2&&rows(*d).size()==2);
  desktop::LightingSettings only_output;only_output.output=settings.output;CHECK(d->set_lighting(only_output,d->generation()));
  CHECK(!d->lighting().value().light&&d->lighting().value().output==settings.output&&rows(*d).size()==2);
  const auto before_clear=d->generation();CHECK(d->set_lighting({},d->generation()));CHECK(rows(*d).size()==1&&!d->lighting().value().output);
  CHECK(d->set_lighting({},d->generation()));CHECK(d->generation()==before_clear+1);
  CHECK(d->undo());CHECK(d->lighting().value().output==settings.output&&!d->lighting().value().light);
  // JSON and cooked round trips preserve exact values through the new codec identity.
  CHECK(d->set_lighting(settings,d->generation()));const auto expected=d->lighting().value();
  MemoryFiles files;CHECK(d->save(files,"lit.scene.json"));CHECK(d->cook(files,"lit.rscene"));
  CHECK(std::memcmp(files.data.at("lit.rscene").data(),"RACESCN1",8)==0);
  for(const auto* path:{"lit.scene.json","lit.rscene"}){
    auto loaded=document();CHECK(loaded->load(files,path));CHECK(loaded->lighting().value()==expected);same_rows(rows(*loaded),rows(*d));
    CHECK(loaded->cook(files,"again.rscene"));CHECK(files.data.at("again.rscene")==files.data.at("lit.rscene"));
  }
  // Out-of-range or degenerate values are rejected before publication; the Scene is untouched.
  const auto generation=d->generation();
  auto bad=settings;bad.light->illuminance_lux=render::physical_illuminance_max_lux*2;CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.light->direction={0,0,0};CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.light->direction={std::numeric_limits<double>::quiet_NaN(),1,0};CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.output->reference_luminance_cd_m2=0;CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.output->peak_nits=100;bad.output->paper_white_nits=200;CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.output->background_cd_m2={-1,0,0};CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.output->exposure_stops=-21;CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::InvalidInput);
  bad=settings;bad.output->profile=static_cast<render::LightingProfile>(7);CHECK_ERROR(d->set_lighting(bad,generation),ErrorCode::Unsupported);
  CHECK(d->generation()==generation&&d->lighting().value()==expected);
  // A second settings component elsewhere is rejected on validation, install and write.
  auto snapshot=d->snapshot();CHECK(snapshot);auto duplicate=snapshot.value();
  const scene::Component* output_component=nullptr;
  for(const auto& object:duplicate.objects)for(const auto& c:object.components)if(c.type==desktop::output_type)output_component=&c;
  CHECK(output_component);const auto copied=*output_component;
  for(auto& object:duplicate.objects)if(object.id!=expected.output_object){object.components.push_back(copied);break;}
  CHECK_ERROR(desktop::lighting_settings(duplicate),ErrorCode::InvalidInput);
  CHECK(!d->install(duplicate,d->persistence_stamp()));CHECK(d->generation()==generation);
  const auto cooked=files.data.at("lit.rscene");
  CHECK(!desktop::Document::write(files,"lit.rscene",duplicate,*d->schemas(),true));CHECK(files.data.at("lit.rscene")==cooked);
  // Cooked files of the previous identity keep legacy semantics; this identity's descriptors reject under the old identity.
  auto fresh=document();auto legacy_doc=fresh->snapshot();CHECK(legacy_doc);
  auto legacy=scene::Schemas::create({{desktop::box_type,1,"Box",{{1,"dimensions",scene::Vec3{1,1,1},.01,1000,true},{2,"color",scene::Vec3{.28,.52,.78},0,1,true}}},
    {desktop::mesh_type,1,"Mesh",{{1,"asset",PersistentId{}},{2,"revisionLow",PersistentId{}},{3,"revisionHigh",PersistentId{}},{4,"color",scene::Vec3{.28,.52,.78},0,1,true}}},
    {desktop::material_type,1,"Material",{{1,"asset",PersistentId{}},{2,"revisionLow",PersistentId{}},{3,"revisionHigh",PersistentId{}}}}});CHECK(legacy);
  auto old=scene::cook(legacy_doc.value(),*legacy.value(),{},"race-desktop-surface-schema-1");CHECK(old);files.data["surface.rscene"]=old.value().bytes;
  auto loaded=document();CHECK(loaded->load(files,"surface.rscene"));CHECK(!loaded->lighting().value().output&&rows(*loaded).size()==1);
  auto wrong=scene::cook(legacy_doc.value(),*d->schemas(),{},"race-desktop-surface-schema-1");CHECK(wrong);files.data["wrong.rscene"]=wrong.value().bytes;
  CHECK(!loaded->load(files,"wrong.rscene"));
  // Play extraction reads the isolated World; authoring edits stay frozen during Play.
  CHECK(d->start_play());CHECK(d->lighting(true).value()==expected);CHECK_ERROR(d->set_lighting(settings,d->generation()),ErrorCode::Busy);CHECK(d->stop_play());
}
void preview_rejects_invalid_graphs_without_changing_inputs(){
  auto d=document();CHECK(d->add_box());const auto initial=rows(*d);auto preview=initial.front();
  auto malformed=initial;malformed[0].parent=malformed[1].id;malformed[1].parent=malformed[0].id;
  preview=malformed[0];CHECK_ERROR(desktop::preview_objects(malformed,preview),ErrorCode::Cycle);
  malformed=initial;malformed[0].parent={99,99};preview=malformed[0];
  CHECK_ERROR(desktop::preview_objects(malformed,preview),ErrorCode::InvalidDependency);
  malformed=initial;malformed[1].id=malformed[0].id;
  CHECK_ERROR(desktop::preview_objects(malformed,malformed[0]),ErrorCode::Duplicate);
  preview=initial[0];preview.handle={};CHECK_ERROR(desktop::preview_objects(initial,preview),ErrorCode::StaleHandle);
  preview=initial[0];preview.parent=initial[1].id;CHECK_ERROR(desktop::preview_objects(initial,preview),ErrorCode::StaleHandle);
  preview=initial[0];preview.local.translation.x=std::numeric_limits<double>::quiet_NaN();
  CHECK_ERROR(desktop::preview_objects(initial,preview),ErrorCode::InvalidInput);
  malformed=initial;malformed[1].parent=malformed[0].id;malformed[0].world.m[0]=1e308;
  preview=malformed[1];preview.local.scale.x=2;
  CHECK_ERROR(desktop::preview_objects(malformed,preview),ErrorCode::Overflow);
  // No inverse is needed: valid zero and very nonuniform parent scales compose.
  for(const auto scale:{scene::Vec3{0,2,3},scene::Vec3{1e-6,1e6,-1}}){
    malformed=initial;malformed[1].parent=malformed[0].id;malformed[0].local.scale=scale;
    auto parent=scene::affine(malformed[0].local);CHECK(parent);malformed[0].world=parent.value();
    preview=malformed[1];preview.local.translation={3,4,5};auto composed=desktop::preview_objects(malformed,preview);CHECK(composed);
    auto local=scene::affine(preview.local);CHECK(local);auto expected=scene::compose(parent.value(),local.value());CHECK(expected);
    CHECK(composed.value()[1].world==expected.value());
  }
  std::vector<ObjectRow> chain(scene::Limits{}.hierarchy_depth+1);
  for(std::size_t i=0;i<chain.size();++i){chain[i].id={77,i+1};if(i)chain[i].parent=chain[i-1].id;}
  CHECK_ERROR(desktop::preview_objects(chain,chain.front()),ErrorCode::Exhausted);
  chain.resize(scene::Limits{}.objects+1);CHECK_ERROR(desktop::preview_objects(chain,chain.front()),ErrorCode::Exhausted);
  same_rows(rows(*d),initial);
}
}
int main() {
  persistence_parity_and_io_failure(); corrupt_load_preserves_state_and_history();
  dimensions_scale_and_atomic_history(); stale_generation_and_handle();
  play_isolation_and_edit_rejection(); next_id_after_reload();
  new_scene_uses_fresh_identity_without_file_io(); play_composes_authored_rotation();
  empty_scene_play_has_no_target();
  explicit_history_clear_preserves_scene();
  extraction_returns_independent_owned_rows();extraction_separates_play_world_lifetimes();
  origin_reload_invalidates_extraction();concurrent_extraction_owns_each_result();
  parent_preview_matches_scene_without_publication();preview_rejects_invalid_graphs_without_changing_inputs();
  lighting_settings_contracts();
  std::puts("desktop scene: persistence/history, stale edits, Play/fresh IDs, owned extraction cache, generation/lifetime invalidation and concurrent readers passed");
}
