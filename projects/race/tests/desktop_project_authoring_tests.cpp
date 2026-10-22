#include "desktop_project_authoring.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <array>
#include <cmath>

namespace {
namespace d=race::desktop;
const mesh_recipe::Snapshot& mesh(const content_project::Recipe& value){return std::get<mesh_recipe::Snapshot>(value);}
const std::vector<std::byte>& raw(const content_project::Recipe& value){
  return std::visit([](const auto& snapshot)->const std::vector<std::byte>&{return snapshot.bytes;},value);
}
constexpr std::string_view obj="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
struct AuthorFiles final:FakeFiles {
  std::string corrupt_leaf,capacity_leaf,race_leaf;
  int race{};
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    auto read=FakeFiles::read(leaf,limit);if(!read)return read.error();
    std::lock_guard lock(mutex);
    if(leaf==corrupt_leaf&&!read.value().empty())read.value().back()^=std::byte{1};
    if(leaf==capacity_leaf)read.value().reserve(limit+1);return read;
  }
  Result<void> publish_immutable(std::string_view leaf,std::span<const std::byte> value) override {
    {std::lock_guard lock(mutex);if(leaf==race_leaf&&race){
      data[std::string(leaf)]={value.begin(),value.end()};
      if(race==2)data[std::string(leaf)].push_back(std::byte{'x'});race=0;
    }}
    return FakeFiles::publish_immutable(leaf,value);
  }
  void put(std::string_view leaf,std::span<const std::byte> value){std::lock_guard lock(mutex);data[std::string(leaf)]={value.begin(),value.end()};}
  std::vector<std::byte> get(std::string_view leaf){std::lock_guard lock(mutex);return data.at(std::string(leaf));}
  bool exists(std::string_view leaf){std::lock_guard lock(mutex);return data.contains(std::string(leaf));}
  void reset_faults(){std::lock_guard lock(mutex);corrupt_leaf.clear();capacity_leaf.clear();throw_read=false;fail_write=SIZE_MAX;}
};
struct AuthorFixture {
  std::shared_ptr<AuthorFiles> recipes=std::make_shared<AuthorFiles>(),sources=std::make_shared<AuthorFiles>(),
      products=std::make_shared<AuthorFiles>(),checkpoints=std::make_shared<AuthorFiles>();
  std::shared_ptr<const d::ProjectBrowserView> visible;
  std::string index_leaf="original.raceproject";
  explicit AuthorFixture(std::size_t count=2){
    std::vector<content_project::Reference> references;
    for(std::size_t n=0;n<count;++n){
      const auto leaf="old-"+std::to_string(n)+".json";auto encoded=mesh_recipe::encode({aid(n+1),"old.obj"});CHECK(encoded);
      encoded.value()+=" \n";recipes->put(leaf,bytes(encoded.value()));references.push_back({aid(n+1),content_project::Kind::Mesh,leaf,digest(bytes(encoded.value()))});
    }
    auto encoded=content_project::encode_binary(references);CHECK(encoded);recipes->put(index_leaf,encoded.value());
    auto snapshot=content_project::load(*recipes,index_leaf);CHECK(snapshot);
    CHECK(snapshot.value().schema()==2&&snapshot.value().format()==content_project::Format::Binary);
    visible=std::make_shared<d::ProjectBrowserView>(d::ProjectBrowserRequest{"recipes",index_leaf,1},std::move(snapshot).value());
    recipes->reset_reads();sources->put("new.obj",bytes(obj));
  }
  d::ProjectAuthoringRequest request(d::ProjectAuthoringKind kind=d::ProjectAuthoringKind::Add) const {
    return {kind,"recipes","sources","products","checkpoints","next.raceproject",kind==d::ProjectAuthoringKind::Add?"new.obj":"",kind==d::ProjectAuthoringKind::Add?aid(100):AssetId{},2};
  }
  d::ProjectAuthoringTask::RootFactory roots() const {
    return [recipes=recipes,sources=sources,products=products,checkpoints=checkpoints,owner=std::this_thread::get_id()]
        (std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>> {
      CHECK(std::this_thread::get_id()!=owner);
      if(name=="recipes")return std::shared_ptr<platform::RootedFiles>(recipes);
      if(name=="sources")return std::shared_ptr<platform::RootedFiles>(sources);
      if(name=="products")return std::shared_ptr<platform::RootedFiles>(products);
      if(name=="checkpoints")return std::shared_ptr<platform::RootedFiles>(checkpoints);
      return fail(ErrorCode::NotFound,"authoring fixture root missing");
    };
  }
  std::string recipe_leaf(mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry) const {auto encoded=mesh_recipe::encode({aid(100),"new.obj",profile});CHECK(encoded);return hex(digest(bytes(encoded.value())))+".json";}
};
d::ProjectAuthoringCompletion finish(d::ProjectAuthoringTask& task,std::shared_ptr<const d::ProjectBrowserView>& visible,std::uint64_t epoch=2){
  const auto deadline=std::chrono::steady_clock::now()+10s;
  for(;;){auto result=task.poll(epoch,visible);CHECK(result);if(result.value())return std::move(*result.value());
    CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}
}
void success_and_exact_reads(){
  for(bool empty:{false,true}){
    AuthorFixture f(2);auto prior=f.visible;const auto old=f.recipes->get(f.index_leaf);d::ProjectAuthoringTask task(f.roots());
    CHECK(task.progress().value().phase==d::ProjectAuthoringPhase::Idle);
    auto request=f.request(empty?d::ProjectAuthoringKind::Create:d::ProjectAuthoringKind::Add);const auto kind=request.kind;
    auto admitted=task.start(f.visible,std::move(request));CHECK(admitted);auto done=finish(task,f.visible);
    CHECK(done.result&&done.changed&&!task.busy()&&done.request==admitted.value()&&done.kind==kind&&done.epoch==2);
    CHECK(f.visible!=prior&&f.visible->source.project_leaf=="next.raceproject"&&f.visible->source.epoch==2);
    CHECK(f.recipes->get(f.index_leaf)==old&&prior->project.bytes().size()==old.size());
    CHECK(f.visible->project.schema()==2&&f.visible->project.format()==content_project::Format::Binary);
    const auto published=f.recipes->get("next.raceproject");
    CHECK(f.visible->project.revision()==digest(published)&&std::equal(published.begin(),published.end(),f.visible->project.bytes().begin(),f.visible->project.bytes().end()));
    if(empty){auto expected=content_project::encode_binary({});CHECK(expected&&published==expected.value());}
    CHECK(done.entries==(empty?0:3)&&done.retained_bytes<=content_project::maximum_retained_bytes);
    CHECK(f.sources->read_count()==(empty?0:1)&&f.products->read_count()==0&&f.checkpoints->read_count()==0);
    CHECK(f.recipes->read_count()==(empty?2:9));
    CHECK(f.recipes->writes==(empty?1:2)&&f.sources->writes==0&&f.products->writes==0&&f.checkpoints->writes==0);
    CHECK(std::isfinite(done.admission_ms)&&std::isfinite(done.readback_ms)&&done.publication_ms>=0);
    if(!empty){
      CHECK(done.source==digest(bytes(obj))&&done.source_bytes==obj.size()&&done.recipe_leaf==f.recipe_leaf());
      auto found=f.visible->project.find(aid(100));CHECK(found&&found.value()->reference.recipe==done.recipe_leaf);
      auto expected=mesh_recipe::encode({aid(100),"new.obj"});CHECK(expected);
      CHECK(raw(found.value()->recipe)==std::vector<std::byte>(bytes(expected.value()).begin(),bytes(expected.value()).end()));
      for(const auto& entry:prior->project.entries()){auto copy=f.visible->project.find(entry.reference.id);CHECK(copy&&raw(copy.value()->recipe)==raw(entry.recipe));}
    }
  }
  AuthorFixture f(31);d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,f.request()));CHECK(finish(task,f.visible).result);
  CHECK(f.visible->project.entries().size()==32);auto next=f.request();next.new_index_leaf="full.raceproject";next.asset=aid(101);
  CHECK_ERROR(task.start(f.visible,std::move(next)),ErrorCode::Exhausted);
}
void explicit_profiles_and_mixed_closure(){
  constexpr std::string_view uv="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
  for(const auto profile:{mesh_pipeline::Profile::Geometry,mesh_pipeline::Profile::GeometryUv})for(const auto source:{obj,uv}){
    AuthorFixture f;const auto prior=f.visible;f.sources->put("new.obj",bytes(source));
    auto request=f.request();CHECK(request.profile==mesh_pipeline::Profile::Geometry);request.profile=profile;
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,request));const auto done=finish(task,f.visible);
    CHECK(done.result&&done.changed&&done.names.profile==profile&&done.source==digest(bytes(source))&&done.source_bytes==source.size());
    const auto found=f.visible->project.find(aid(100));CHECK(found&&mesh(found.value()->recipe).definition.profile==profile);
    auto expected=mesh_recipe::encode({aid(100),"new.obj",profile});CHECK(expected);
    CHECK(raw(found.value()->recipe)==std::vector<std::byte>(bytes(expected.value()).begin(),bytes(expected.value()).end()));
    CHECK(done.recipe_leaf==f.recipe_leaf(profile));
    CHECK(f.recipe_leaf(mesh_pipeline::Profile::Geometry)!=f.recipe_leaf(mesh_pipeline::Profile::GeometryUv));
    for(const auto& entry:prior->project.entries()){
      auto copied=f.visible->project.find(entry.reference.id);CHECK(copied&&raw(copied.value()->recipe)==raw(entry.recipe));
      CHECK(mesh(copied.value()->recipe).definition.profile==mesh_pipeline::Profile::Geometry);
    }
    CHECK(f.sources->read_count()==1&&f.products->writes==0&&f.checkpoints->writes==0);
    // Authoring never infers or parses a profile from the source. Reimport owns
    // format validation; both explicit profiles can name either source here.
  }
  AuthorFixture f;d::ProjectAuthoringTask task(f.roots());
  for(const auto invalid:{0,3,255}){auto request=f.request();request.profile=static_cast<mesh_pipeline::Profile>(invalid);
    CHECK_ERROR(task.start(f.visible,request),ErrorCode::InvalidInput);CHECK(!task.busy());}
  auto create=f.request(d::ProjectAuthoringKind::Create);create.profile=mesh_pipeline::Profile::GeometryUv;
  CHECK_ERROR(task.start(f.visible,create),ErrorCode::InvalidInput);
  CHECK(f.recipes->read_count()==0&&f.sources->read_count()==0&&f.recipes->writes==0);
}

d::ProjectAuthoringRequest surface_request(AuthorFixture& f,surface_recipe::Profile profile){
  auto request=f.request();request.surface_profile=profile;const auto info=surface_pipeline::profile_info(profile);CHECK(info);
  request.source_leaf="new"+std::string(info.value().source_extension);
  if(info.value().material)request.texture=surface::Reference{aid(500),digest(bytes("exact cooked texture"))};
  // This is recipe authoring, not import/cook. Source format parsing belongs to
  // the explicit build boundary; the worker records these exact bounded bytes.
  f.sources->put(request.source_leaf,bytes("bounded authoring source\n"));return request;
}
std::string surface_leaf(const d::ProjectAuthoringRequest& request){
  CHECK(request.surface_profile);auto encoded=surface_recipe::encode({request.asset,request.source_leaf,*request.surface_profile,request.texture});CHECK(encoded);
  return hex(digest(bytes(encoded.value())))+".json";
}
void preserved_entries(const d::ProjectBrowserView& prior,const d::ProjectBrowserView& current){
  for(const auto& entry:prior.project.entries()){
    auto copied=current.project.find(entry.reference.id);CHECK(copied);
    CHECK(copied.value()->reference.kind==entry.reference.kind&&copied.value()->reference.recipe==entry.reference.recipe&&
        copied.value()->reference.revision==entry.reference.revision&&copied.value()->recipe.index()==entry.recipe.index()&&raw(copied.value()->recipe)==raw(entry.recipe));
  }
}
void surface_profiles_and_typed_add_preserve_closure(){
  AuthorFixture f;const auto original=f.visible;d::ProjectAuthoringTask task(f.roots());
  for(unsigned n=1;n<=11;++n){
    auto request=surface_request(f,static_cast<surface_recipe::Profile>(n));request.asset=aid(100+n);request.new_index_leaf="surface-"+std::to_string(n)+".raceproject";
    const auto prior=f.visible;const auto before=f.recipes->get(prior->source.project_leaf);auto encoded=surface_recipe::encode({request.asset,request.source_leaf,*request.surface_profile,request.texture});CHECK(encoded);
    CHECK(task.start(f.visible,request));const auto done=finish(task,f.visible);
    CHECK(done.result&&done.changed&&done.names.surface_profile==request.surface_profile&&done.names.texture==request.texture);
    CHECK(done.source==digest(bytes("bounded authoring source\n"))&&done.source_bytes==25&&done.recipe_leaf==surface_leaf(request));
    CHECK(f.visible->project.schema()==2&&f.visible->project.format()==content_project::Format::Binary&&f.visible->project.entries().size()==n+2&&f.recipes->get(prior->source.project_leaf)==before);
    const auto found=f.visible->project.find(request.asset);CHECK(found);
    const auto info=surface_pipeline::profile_info(*request.surface_profile);CHECK(info);
    CHECK(found.value()->reference.kind==(info.value().material?content_project::Kind::Material:content_project::Kind::Texture));
    const auto& recipe=std::get<surface_recipe::Snapshot>(found.value()->recipe);
    CHECK(recipe.definition.profile==*request.surface_profile&&recipe.definition.texture==request.texture);
    CHECK(raw(found.value()->recipe)==std::vector<std::byte>(bytes(encoded.value()).begin(),bytes(encoded.value()).end()));
    preserved_entries(*prior,*f.visible);
    CHECK(done.retained_bytes<=content_project::maximum_retained_bytes&&f.products->read_count()==0&&f.products->writes==0&&f.checkpoints->writes==0);
  }
  CHECK(original->project.schema()==2&&original->project.entries().size()==2);
  // A later mesh addition retains the complete typed index instead of projecting
  // its mesh rows into a different raw project identity.
  const auto prior=f.visible;auto request=f.request();request.asset=aid(200);request.new_index_leaf="mixed-mesh.raceproject";request.profile=mesh_pipeline::Profile::GeometryUv;
  CHECK(task.start(f.visible,request));auto done=finish(task,f.visible);CHECK(done.result&&done.changed&&f.visible->project.schema()==2);
  CHECK(f.visible->project.entries().size()==14);preserved_entries(*prior,*f.visible);
  const auto mesh_row=f.visible->project.find(aid(200));CHECK(mesh_row&&mesh_row.value()->reference.kind==content_project::Kind::Mesh&&mesh(mesh_row.value()->recipe).definition.profile==mesh_pipeline::Profile::GeometryUv);
  auto unbound=surface_request(f,surface_recipe::Profile::Material);unbound.texture.reset();unbound.asset=aid(201);unbound.new_index_leaf="unbound.raceproject";
  CHECK(task.start(f.visible,unbound));done=finish(task,f.visible);CHECK(done.result&&done.changed);
  CHECK(!std::get<surface_recipe::Snapshot>(f.visible->project.find(aid(201)).value()->recipe).definition.texture);
  // Create writes the canonical binary typed index, independently of its prior view.
  auto create=f.request(d::ProjectAuthoringKind::Create);create.new_index_leaf="new-empty.raceproject";
  CHECK(task.start(f.visible,create));CHECK(finish(task,f.visible).result);auto empty=content_project::encode_binary({});CHECK(empty);
  CHECK(f.visible->project.schema()==2&&f.visible->project.format()==content_project::Format::Binary&&f.recipes->get("new-empty.raceproject")==empty.value());
}
void surface_admission_and_capacity(){
  AuthorFixture f;d::ProjectAuthoringTask task(f.roots());
  for(unsigned mode=0;mode<11;++mode){
    auto request=surface_request(f,surface_recipe::Profile::Material);
    if(mode==0)request.surface_profile=static_cast<surface_recipe::Profile>(0);
    if(mode==1)request.surface_profile=static_cast<surface_recipe::Profile>(255);
    if(mode==2)request.profile=mesh_pipeline::Profile::GeometryUv;
    if(mode==3)request.texture=surface::Reference{};
    if(mode==4){request.surface_profile=surface_recipe::Profile::MaterialMips;request.texture.reset();}
    if(mode==5){request.surface_profile=surface_recipe::Profile::PpmSrgb;request.source_leaf="new.ppm";}
    if(mode==6)request.source_leaf="new.ppm";
    if(mode==7){request.surface_profile.reset();request.source_leaf="new.obj";}
    if(mode==8){request.kind=d::ProjectAuthoringKind::Create;request.asset={};request.source_leaf.clear();}
    if(mode==9){request.surface_profile=surface_recipe::Profile::MaterialScalarMips;request.texture.reset();}
    if(mode==10){request.surface_profile=surface_recipe::Profile::MaterialScalarMips;request.texture=surface::Reference{aid(500),{}};}
    auto admitted=task.start(f.visible,request);CHECK_ERROR(admitted,mode<=1?ErrorCode::Unsupported:ErrorCode::InvalidInput);CHECK(!task.busy());
  }
  CHECK(f.recipes->read_count()==0&&f.sources->read_count()==0&&f.products->read_count()==0&&f.recipes->writes==0);
  AuthorFixture full(31);d::ProjectAuthoringTask owner(full.roots());auto request=surface_request(full,surface_recipe::Profile::PpmLinear);
  CHECK(owner.start(full.visible,request));CHECK(finish(owner,full.visible).result);CHECK(full.visible->project.entries().size()==32&&full.visible->project.schema()==2);
  request.asset=aid(101);request.new_index_leaf="overflow.raceproject";CHECK_ERROR(owner.start(full.visible,request),ErrorCode::Exhausted);
  for(const auto profile:{surface_recipe::Profile::BmpSrgb,surface_recipe::Profile::Material,surface_recipe::Profile::BmpScalarMips})for(unsigned mode=0;mode<3;++mode){
    AuthorFixture sample;const auto prior=sample.visible;auto names=surface_request(sample,profile);
    const auto limit=profile==surface_recipe::Profile::Material?surface_pipeline::material_source_bytes:surface_pipeline::source_bytes;
    if(mode==0)sample.sources->put(names.source_leaf,{});
    if(mode==1)sample.sources->capacity_leaf=names.source_leaf;
    if(mode==2){std::vector<std::byte> oversized(limit+1);sample.sources->put(names.source_leaf,oversized);}
    d::ProjectAuthoringTask bounded(sample.roots());CHECK(bounded.start(sample.visible,names));auto done=finish(bounded,sample.visible);
    CHECK_ERROR(done.result,mode==0?ErrorCode::InvalidInput:ErrorCode::Exhausted);CHECK(!done.changed&&sample.visible==prior&&sample.recipes->writes==0);
  }
}
void surface_failure_stale_and_cancel(){
  for(const auto profile:{surface_recipe::Profile::BmpSrgbMips,surface_recipe::Profile::MaterialScalarMips})for(unsigned mode=0;mode<6;++mode){
    AuthorFixture f;auto prior=f.visible;const auto old=f.recipes->get("original.raceproject");auto request=surface_request(f,profile);
    if(mode==0)f.recipes->corrupt_leaf=surface_leaf(request);
    if(mode==1)f.recipes->corrupt_leaf="next.raceproject";
    if(mode==2)f.recipes->fail_write=0;
    if(mode==3)f.recipes->fail_write=1;
    if(mode==4){auto changed=f.recipes->get("old-1.json");changed.push_back(std::byte{' '});f.recipes->put("old-1.json",changed);}
    if(mode==5)f.sources->throw_read=true;
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,request));const auto done=finish(task,f.visible);
    CHECK(!done.result&&!done.changed&&f.visible==prior&&!task.busy()&&f.recipes->get("original.raceproject")==old);
    CHECK(f.products->writes==0&&f.checkpoints->writes==0);
    if(mode==4)CHECK(f.sources->read_count()==0&&f.recipes->writes==0);
  }
  for(const auto profile:{surface_recipe::Profile::BmpSrgbMips,surface_recipe::Profile::MaterialMips,surface_recipe::Profile::PpmScalarMips,surface_recipe::Profile::BmpScalarMips})for(unsigned mode=0;mode<4;++mode){
    AuthorFixture f;auto prior=f.visible;auto request=surface_request(f,profile);const auto leaf=surface_leaf(request);
    auto gate=std::make_shared<Gate>();f.recipes->gate=gate;f.recipes->gated=leaf;
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,request));gate->wait();
    CHECK(task.busy()&&f.recipes->exists(leaf)&&!f.recipes->exists("next.raceproject"));
    if(mode==0||mode==3){CHECK(task.cancel());CHECK(task.progress().value().cancellation_requested);}
    if(mode==2)f.visible.reset();
    if(mode==3){f.visible.reset();f.visible=prior;} // Pointer ABA still cancelled.
    auto pending=task.poll(mode==1?3:2,f.visible);CHECK(pending&&!pending.value()&&task.busy());
    gate->release();const auto done=finish(task,f.visible,mode==1?3:2);
    CHECK_ERROR(done.result,mode==0||mode==3?ErrorCode::Cancelled:ErrorCode::StaleHandle);CHECK(!done.changed&&!task.busy());
    CHECK(mode==2?!f.visible:f.visible==prior);
    if(mode==0||mode==3)CHECK(!f.recipes->exists("next.raceproject"));
  }
}

void collision_and_reuse(){
  for(unsigned mode=0;mode<5;++mode){
    AuthorFixture f(2);auto prior=f.visible;const auto old=f.recipes->get(f.index_leaf);
    auto request=f.request();auto recipe=mesh_recipe::encode({aid(100),"new.obj"});CHECK(recipe);
    if(mode==0){auto index=content_project::encode_binary({});CHECK(index);f.recipes->put("next.raceproject",index.value());}
    if(mode==1)f.recipes->put(f.recipe_leaf(),bytes(recipe.value()));
    if(mode==2)f.recipes->put(f.recipe_leaf(),bytes("foreign recipe"));
    if(mode>=3){f.recipes->race_leaf="next.raceproject";f.recipes->race=mode==3?1:2;}
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,std::move(request)));auto done=finish(task,f.visible);
    if(mode==1||mode==3){CHECK(done.result&&done.changed&&f.visible!=prior&&f.visible->project.format()==content_project::Format::Binary);}
    else{CHECK_ERROR(done.result,ErrorCode::Duplicate);CHECK(!done.changed&&f.visible==prior);}
    CHECK(f.recipes->get(f.index_leaf)==old);
    if(mode==0)CHECK(f.recipes->read_count()==1&&f.sources->read_count()==0&&f.recipes->writes==0);
  }
  AuthorFixture f;d::ProjectAuthoringTask task(f.roots());auto request=f.request();request.new_index_leaf=f.recipe_leaf();
  CHECK_ERROR(task.start(f.visible,std::move(request)),ErrorCode::InvalidInput);CHECK(!task.busy()&&f.recipes->read_count()==0&&f.recipes->writes==0);
}
void raw_pins_and_readback_failures(){
  for(unsigned mode=0;mode<10;++mode){
    AuthorFixture f(2);auto prior=f.visible;const auto old=f.recipes->get(f.index_leaf);
    if(mode==0){
        std::vector<content_project::Reference> refs;
        for(const auto& entry:prior->project.entries())refs.push_back(entry.reference);
        refs.pop_back();auto changed=content_project::encode_binary(refs);CHECK(changed);f.recipes->put(f.index_leaf,changed.value());
    }
    if(mode==1){auto raw=f.recipes->get("old-1.json");raw.push_back(std::byte{' '});f.recipes->put("old-1.json",raw);}
    if(mode==2)f.sources->put("new.obj",{});
    if(mode==3)f.sources->capacity_leaf="new.obj";
    if(mode==4)f.recipes->fail_write=0;
    if(mode==5)f.recipes->corrupt_leaf=f.recipe_leaf();
    if(mode==6)f.recipes->fail_write=1;
    if(mode==7)f.recipes->corrupt_leaf="next.raceproject";
    if(mode==8)f.sources->throw_read=true;
    if(mode==9){std::vector<std::byte> oversized(mesh_pipeline::Limits{}.source_bytes+1);f.sources->put("new.obj",oversized);}
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,f.request()));auto done=finish(task,f.visible);
    CHECK(!done.result&&!done.changed&&f.visible==prior&&!task.busy());
    CHECK(std::equal(prior->project.bytes().begin(),prior->project.bytes().end(),old.begin(),old.end()));
    if(mode<=1)CHECK_ERROR(done.result,ErrorCode::InvalidDependency);
    if(mode<=1)CHECK(f.sources->read_count()==0&&f.recipes->writes==0);
  }
}
void cancellation_stale_and_shutdown(){
  for(const auto profile:{mesh_pipeline::Profile::Geometry,mesh_pipeline::Profile::GeometryUv})for(unsigned mode=0;mode<4;++mode){
    AuthorFixture f(2);auto prior=f.visible;const auto old=f.recipes->get(f.index_leaf);auto gate=std::make_shared<Gate>();f.recipes->gate=gate;f.recipes->gated=f.recipe_leaf(profile);
    auto request=f.request();request.profile=profile;
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,request));gate->wait();
    CHECK(task.busy()&&f.recipes->exists(f.recipe_leaf(profile))&&!f.recipes->exists("next.raceproject"));
    CHECK_ERROR(task.start(f.visible,f.request()),ErrorCode::Busy);
    if(mode==0||mode==3){CHECK(task.cancel());CHECK(task.cancel());}
    if(mode==2)f.visible.reset(); // Pointer staleness, independently of epoch.
    auto pending=task.poll(mode==1?3:2,f.visible);CHECK(pending&&!pending.value()&&task.busy());
    gate->release();auto done=finish(task,f.visible,mode==1?3:2);
    CHECK_ERROR(done.result,(mode==0||mode==3)?ErrorCode::Cancelled:ErrorCode::StaleHandle);CHECK(!done.changed);
    if(mode!=2)CHECK(f.visible==prior);CHECK(f.recipes->get(f.index_leaf)==old);
    if(mode==0||mode==3)CHECK(!f.recipes->exists("next.raceproject"));
    if(mode==3){ // Explicit cancellation also rejects pointer/epoch ABA.
      f.visible=prior;auto next=f.request();next.profile=profile;next.new_index_leaf="retry.raceproject";
      CHECK(task.start(f.visible,std::move(next)));CHECK(finish(task,f.visible).result);
    }
  }
  for(bool cancel:{false,true}){
    AuthorFixture f(2);const auto prior=f.visible;const auto old=f.recipes->get(f.index_leaf);
    auto gate=std::make_shared<Gate>();f.recipes->gate=gate;f.recipes->gated="next.raceproject";
    d::ProjectAuthoringTask task(f.roots());CHECK(task.start(f.visible,f.request(d::ProjectAuthoringKind::Create)));gate->wait();
    CHECK(task.busy()&&f.visible==prior&&!f.recipes->exists("next.raceproject"));if(cancel)CHECK(task.cancel());
    gate->release();const auto done=finish(task,f.visible,cancel?2:3);
    CHECK_ERROR(done.result,cancel?ErrorCode::Cancelled:ErrorCode::StaleHandle);
    CHECK(!done.changed&&!task.busy()&&f.visible==prior&&f.recipes->get(f.index_leaf)==old);
    if(cancel)CHECK(!f.recipes->exists("next.raceproject"));
    else{auto empty=content_project::encode_binary({});CHECK(empty&&f.recipes->get("next.raceproject")==empty.value());}
  }
  AuthorFixture f;auto gate=std::make_shared<Gate>();f.sources->gate=gate;f.sources->gated="new.obj";
  auto task=std::make_unique<d::ProjectAuthoringTask>(f.roots());CHECK(task->start(f.visible,f.request()));gate->wait();
  CHECK(task->progress().value().phase==d::ProjectAuthoringPhase::ReadingSource);
  std::weak_ptr<AuthorFiles> recipes=f.recipes,sources=f.sources,products=f.products,checkpoints=f.checkpoints;
  std::weak_ptr<const d::ProjectBrowserView> view=f.visible;f.recipes.reset();f.sources.reset();f.products.reset();f.checkpoints.reset();f.visible.reset();
  std::atomic<bool> destroyed{};
  std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load()&&!recipes.expired()&&!sources.expired()&&!view.expired());gate->release();});
  task.reset();destroyed=true;release.join();CHECK(recipes.expired()&&sources.expired()&&products.expired()&&checkpoints.expired()&&view.expired());
}
void malformed_admission_and_allocations(){
  AuthorFixture f;d::ProjectAuthoringTask task(f.roots());
  std::thread foreign([&]{CHECK_ERROR(task.start(f.visible,f.request()),ErrorCode::WrongThread);CHECK_ERROR(task.cancel(),ErrorCode::WrongThread);
    CHECK_ERROR(task.progress(),ErrorCode::WrongThread);CHECK_ERROR(task.poll(2,f.visible),ErrorCode::WrongThread);});foreign.join();
  CHECK_ERROR(task.cancel(),ErrorCode::NotFound);
  for(unsigned mode=0;mode<9;++mode){auto names=f.request();
    if(mode==0)names.recipe_root.clear();if(mode==1)names.source_root.push_back('\0');if(mode==2)names.product_root.assign(4097,'x');
    if(mode==3)names.checkpoint_root.reserve(8192);if(mode==4)names.new_index_leaf="../bad.raceproject";if(mode==5)names.source_leaf="bad.OBJ";
    if(mode==6)names.new_index_leaf.reserve(1024);if(mode==7)names.epoch=0;if(mode==8)names.asset={};
    CHECK_ERROR(task.start(f.visible,std::move(names)),mode==3||mode==6?ErrorCode::Exhausted:ErrorCode::InvalidInput);CHECK(!task.busy());
  }
  for(const auto kind:{d::ProjectAuthoringKind::Create,d::ProjectAuthoringKind::Add})
    for(const auto leaf:{"new.json","new.RACEPROJECT","con.raceproject"}){
#ifndef _WIN32
      if(std::string_view{leaf}=="con.raceproject")continue;
#endif
      auto names=f.request(kind);names.new_index_leaf=leaf;
      CHECK_ERROR(task.start(f.visible,std::move(names)),ErrorCode::InvalidInput);CHECK(!task.busy());
    }
  auto duplicate=f.request();duplicate.asset=aid(1);CHECK_ERROR(task.start(f.visible,std::move(duplicate)),ErrorCode::Duplicate);
  auto replacement=f.request();replacement.new_index_leaf="original.json";CHECK_ERROR(task.start(f.visible,std::move(replacement)),ErrorCode::InvalidInput);
  AuthorFixture binary(2);d::ProjectAuthoringTask binary_task(binary.roots());
  replacement=binary.request();replacement.new_index_leaf=binary.index_leaf;
  CHECK_ERROR(binary_task.start(binary.visible,std::move(replacement)),ErrorCode::Duplicate);
  CHECK(!binary_task.busy()&&binary.recipes->read_count()==0&&binary.recipes->writes==0);
  auto moved=f.request();moved.recipe_root="elsewhere";CHECK_ERROR(task.start(f.visible,std::move(moved)),ErrorCode::InvalidDependency);
  CHECK_ERROR(task.start({},f.request()),ErrorCode::InvalidInput);CHECK(f.recipes->read_count()==0);
  std::size_t failures{};
  for(std::size_t step=0;step<64;++step){
    AuthorFixture sample;auto prior=sample.visible;d::ProjectAuthoringTask owner(sample.roots());auto names=sample.request();
    d::profile::testing::fail_allocation_after(step);auto admitted=owner.start(sample.visible,std::move(names));d::profile::testing::fail_next_allocation_attempts(0);
    if(admitted){CHECK(finish(owner,sample.visible).result);break;}
    CHECK_ERROR(admitted,ErrorCode::Exhausted);CHECK(!owner.busy()&&sample.visible==prior);
    CHECK(owner.start(sample.visible,sample.request()));CHECK(finish(owner,sample.visible).result);++failures;
  }
  CHECK(failures>0&&failures<64);
  AuthorFixture worker;auto prior=worker.visible;auto roots=worker.roots();auto armed=std::make_shared<std::atomic<bool>>(true);
  d::ProjectAuthoringTask fault([roots,armed](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>>{
    if(armed->exchange(false))throw std::bad_alloc();return roots(name);
  });
  CHECK(fault.start(worker.visible,worker.request()));CHECK_ERROR(finish(fault,worker.visible).result,ErrorCode::Exhausted);CHECK(worker.visible==prior);
  CHECK(fault.start(worker.visible,worker.request()));CHECK(finish(fault,worker.visible).result);
  std::printf("project authoring admission allocation failure steps=%zu\n",failures);
}
} // namespace
int main(){success_and_exact_reads();explicit_profiles_and_mixed_closure();surface_profiles_and_typed_add_preserve_closure();surface_admission_and_capacity();surface_failure_stale_and_cancel();collision_and_reuse();raw_pins_and_readback_failures();cancellation_stale_and_shutdown();malformed_admission_and_allocations();
  std::puts("immutable project authoring closure, bounds, collisions and ownership passed");}
