#include "desktop_project_browser.hpp"
#include "../apps/mesh_project.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace {
namespace d=race::desktop;
namespace p=race::mesh_project;
const mesh_recipe::Snapshot& mesh(const content_project::Recipe& value){return std::get<mesh_recipe::Snapshot>(value);}
const char* phase="startup";std::size_t fault_step{};
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
void put(FakeFiles& files,std::string leaf,std::span<const std::byte> data){files.data[std::move(leaf)]={data.begin(),data.end()};}
void put(FakeFiles& files,std::string leaf,std::string_view text){
  const auto raw=bytes(text);files.data[std::move(leaf)]={raw.begin(),raw.end()};
}
struct BrowserFixture {
  std::shared_ptr<FakeFiles> files=std::make_shared<FakeFiles>();
  std::vector<p::Reference> references;
  std::string index_leaf="project.raceproject";
  explicit BrowserFixture(std::size_t count=2,bool maximum=false,bool mixed=false){
    for(std::size_t n=0;n<count;++n){
      const auto leaf="recipe-"+std::to_string(n)+".json";
      auto raw=mesh_recipe::encode({aid(n+1),"source-"+std::to_string(n)+".obj",mixed&&n%2?mesh_pipeline::Profile::GeometryUv:mesh_pipeline::Profile::Geometry});CHECK(raw);
      if(maximum)raw.value().resize(mesh_recipe::maximum_bytes,' ');
      else raw.value()+=" \n"; // Pin raw provenance, independently of canonical form.
      put(*files,leaf,raw.value());references.push_back({aid(n+1),leaf,digest(bytes(raw.value()))});
    }
    auto index=p::encode(references);CHECK(index);
    put(*files,"project.raceproject",index.value());
  }
  d::ProjectBrowserRequest request(std::uint64_t epoch=1) const{return {"recipes",index_leaf,epoch};}
  d::ProjectBrowserTask::RootFactory factory() const {
    return [files=files](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>>{
      if(name!="recipes")return fail(ErrorCode::NotFound,"fixture root missing");
      return std::shared_ptr<platform::RootedFiles>(files);
    };
  }
  std::shared_ptr<Gate> block(std::string leaf="recipe-1.json"){
    auto gate=std::make_shared<Gate>();files->gate=gate;files->gated=std::move(leaf);return gate;
  }
};
d::ProjectBrowserCompletion finish(d::ProjectBrowserTask& task,
    std::shared_ptr<const d::ProjectBrowserView>& visible,std::uint64_t epoch=1){
  const auto deadline=std::chrono::steady_clock::now()+10s;
  while(std::chrono::steady_clock::now()<deadline){
    auto polled=task.poll(epoch,visible);CHECK(polled);
    if(polled.value())return std::move(*polled.value());
    std::this_thread::sleep_for(1ms);
  }
  std::fprintf(stderr,"browser completion timeout phase=%s step=%zu busy=%d\n",phase,fault_step,int(task.busy()));
  CHECK(false);return {};
}
std::shared_ptr<const d::ProjectBrowserView> initial(BrowserFixture& fixture){
  d::ProjectBrowserTask task(fixture.factory());std::shared_ptr<const d::ProjectBrowserView> view;
  CHECK(task.start(fixture.request()));CHECK(finish(task,view).result);CHECK(view);fixture.files->reset_reads();return view;
}
void metrics(const d::ProjectBrowserCompletion& value){
  CHECK(std::isfinite(value.admission_ms)&&value.admission_ms>=0);
  CHECK(std::isfinite(value.read_ms)&&value.read_ms>=0);
  CHECK(std::isfinite(value.publication_ms)&&value.publication_ms>=0);
}
void complete_snapshots_and_exact_reads(){
  for(std::size_t count:{std::size_t{0},std::size_t{2},p::maximum_entries}){
    BrowserFixture f(count,count==p::maximum_entries);d::ProjectBrowserTask task(f.factory());
    std::shared_ptr<const d::ProjectBrowserView> visible;auto names=f.request();
    const auto id=task.start(names);CHECK(id);names.recipe_root="overwritten";names.project_leaf="lost.json";names.epoch=9;
    const auto done=finish(task,visible);CHECK(done.result&&done.changed&&done.request==id.value()&&done.epoch==1);metrics(done);
    CHECK(!task.busy()&&visible&&visible->source.recipe_root=="recipes"&&visible->source.project_leaf=="project.raceproject"&&visible->source.epoch==1);
    CHECK(done.entries==count&&visible->project.entries().size()==count&&f.files->read_count()==count+1&&f.files->writes==0);
    CHECK(visible->project.schema()==2&&visible->project.format()==content_project::Format::Binary&&visible->project.revision()==digest(f.files->data["project.raceproject"]));
    CHECK(std::equal(visible->project.bytes().begin(),visible->project.bytes().end(),f.files->data["project.raceproject"].begin(),f.files->data["project.raceproject"].end()));
    const auto retained=visible->project.retained_bytes()+sizeof(d::ProjectBrowserView)-sizeof(content_project::Snapshot)+
        visible->source.recipe_root.capacity()+1+visible->source.project_leaf.capacity()+1;
    CHECK(done.retained_bytes==retained&&retained<=p::maximum_retained_bytes);
    for(std::size_t n=0;n<count;++n){
      const auto& row=visible->project.entries()[n];CHECK(row.reference.id==aid(n+1)&&mesh(row.recipe).definition.id==aid(n+1));
      CHECK(row.reference.recipe==f.references[n].recipe&&row.reference.revision==f.references[n].revision&&mesh(row.recipe).revision==f.references[n].revision);
      auto canonical=mesh_recipe::encode(mesh(row.recipe).definition);CHECK(canonical);CHECK(mesh(row.recipe).revision!=digest(bytes(canonical.value())));
      if(count==p::maximum_entries)CHECK(mesh(row.recipe).bytes.size()==mesh_recipe::maximum_bytes&&mesh(row.recipe).bytes.capacity()<=mesh_recipe::maximum_bytes);
    }
    if(count==p::maximum_entries)CHECK(visible->project.bytes().size()==content_project::binary_header_bytes+count*content_project::binary_entry_bytes+content_project::binary_checksum_bytes&&visible->project.storage().index_capacity<=p::maximum_bytes);
    auto empty=task.poll(1,visible);CHECK(empty&&!empty.value());CHECK_ERROR(task.cancel(),ErrorCode::NotFound);
  }
}
void mixed_profiles_preserve_raw_pins(){
  BrowserFixture f(2,false,true);auto visible=initial(f),prior=visible;
  CHECK(mesh(visible->project.entries().front().recipe).definition.profile==mesh_pipeline::Profile::Geometry);
  const auto& uv=visible->project.entries().back();CHECK(mesh(uv.recipe).definition.profile==mesh_pipeline::Profile::GeometryUv);
  CHECK(mesh(uv.recipe).bytes==f.files->data[uv.reference.recipe]&&mesh(uv.recipe).revision==digest(mesh(uv.recipe).bytes));
  auto changed=mesh(uv.recipe).definition;changed.profile=mesh_pipeline::Profile::Geometry;
  auto encoded=mesh_recipe::encode(changed);CHECK(encoded);put(*f.files,uv.reference.recipe,encoded.value());
  d::ProjectBrowserTask task(f.factory());CHECK(task.start(f.request()));auto done=finish(task,visible);
  CHECK_ERROR(done.result,ErrorCode::InvalidDependency);CHECK(!done.changed&&visible==prior);
  CHECK(mesh(visible->project.entries().back().recipe).definition.profile==mesh_pipeline::Profile::GeometryUv);
  // A profile change becomes visible only through a newly pinned complete index.
  f.references.back().revision=digest(bytes(encoded.value()));auto index=p::encode(f.references);CHECK(index);put(*f.files,"project.raceproject",index.value());
  CHECK(task.start(f.request()));done=finish(task,visible);CHECK(done.result&&done.changed&&visible!=prior);
  CHECK(mesh(visible->project.entries().back().recipe).definition.profile==mesh_pipeline::Profile::Geometry);
  CHECK(mesh(prior->project.entries().back().recipe).definition.profile==mesh_pipeline::Profile::GeometryUv);
}

void typed_index(BrowserFixture& f,std::span<const content_project::Reference> references){
  auto index=content_project::encode_binary(references);CHECK(index);f.files->data[f.index_leaf]=std::move(index).value();
}
std::vector<content_project::Reference> typed_fixture(BrowserFixture& f,std::size_t count=3,bool maximum=false){
  std::vector<content_project::Reference> references;
  for(std::size_t n=0;n<count;++n){
    const auto leaf="typed-"+std::to_string(n)+".json";
    const auto kind=n%3==0?content_project::Kind::Mesh:n%3==1?content_project::Kind::Texture:content_project::Kind::Material;
    const std::optional<surface::Reference> pin=kind==content_project::Kind::Material?
        std::optional<surface::Reference>{{aid(900),digest(bytes("exact cooked texture"))}}:std::nullopt;
    auto encoded=kind==content_project::Kind::Mesh?mesh_recipe::encode({aid(n+1),"mesh.obj",mesh_pipeline::Profile::GeometryUv}):
        surface_recipe::encode({aid(n+1),kind==content_project::Kind::Texture?"texture.bmp":"material.json",
            kind==content_project::Kind::Texture?surface_recipe::Profile::BmpSrgbMips:surface_recipe::Profile::MaterialMips,pin});
    CHECK(encoded);if(maximum)encoded.value().resize(surface_recipe::maximum_bytes,' ');else encoded.value()+=" \n";
    put(*f.files,leaf,encoded.value());references.push_back({aid(n+1),kind,leaf,digest(bytes(encoded.value()))});
  }
  typed_index(f,references);return references;
}
void typed_family_closure_and_lifetime(){
  for(const auto count:{std::size_t{0},std::size_t{3},content_project::maximum_entries}){
    BrowserFixture f(0);const auto refs=typed_fixture(f,count,count==content_project::maximum_entries);
    const auto raw_index=f.files->data[f.index_leaf];d::ProjectBrowserTask task(f.factory());
    std::shared_ptr<const d::ProjectBrowserView> visible;CHECK(task.start(f.request()));auto done=finish(task,visible);
    CHECK(done.result&&done.changed&&done.entries==count&&f.files->read_count()==count+1&&f.files->writes==0);
    CHECK(visible->source.project_leaf==f.index_leaf&&visible->project.schema()==2&&visible->project.revision()==digest(raw_index));
    CHECK(visible->project.format()==content_project::Format::Binary);
    CHECK(std::equal(raw_index.begin(),raw_index.end(),visible->project.bytes().begin(),visible->project.bytes().end()));
    const auto retained=visible->project.retained_bytes()+sizeof(d::ProjectBrowserView)-sizeof(content_project::Snapshot)+
        visible->source.recipe_root.capacity()+1+visible->source.project_leaf.capacity()+1;
    CHECK(done.retained_bytes==retained&&retained<=content_project::maximum_retained_bytes);
    for(const auto& ref:refs){
      const auto row=visible->project.find(ref.id);CHECK(row&&row.value()->reference.kind==ref.kind&&row.value()->reference.revision==ref.revision);
      std::visit([&](const auto& value){CHECK(value.definition.id==ref.id&&value.bytes==f.files->data[ref.recipe]&&value.revision==ref.revision);},row.value()->recipe);
      CHECK((ref.kind==content_project::Kind::Mesh)==std::holds_alternative<mesh_recipe::Snapshot>(row.value()->recipe));
    }
    auto prior=visible;f.files->data.clear();CHECK(task.start(f.request()));done=finish(task,visible);
    CHECK_ERROR(done.result,ErrorCode::NotFound);CHECK(!done.changed&&visible==prior);
    CHECK((!count||visible->project.find(aid(count)))&&visible->project.revision()==digest(raw_index));
  }
}
void typed_failures_and_stale_publication(){
  for(unsigned mode=0;mode<5;++mode){
    BrowserFixture f(0);auto refs=typed_fixture(f,3,false);auto visible=initial(f),prior=visible;
    if(mode==0)f.files->data[refs.back().recipe].push_back(std::byte{' '});
    if(mode==1)f.files->data.erase(refs.back().recipe);
    if(mode==2){refs.back().kind=content_project::Kind::Texture;typed_index(f,refs);}
    if(mode==3)f.files->data[refs.back().recipe].resize(surface_recipe::maximum_bytes+1,std::byte{' '});
    if(mode==4){refs.back().revision=refs.front().revision;typed_index(f,refs);}
    d::ProjectBrowserTask task(f.factory());CHECK(task.start(f.request()));const auto done=finish(task,visible);
    CHECK(!done.result&&!done.changed&&visible==prior&&f.files->writes==0);
    CHECK(visible->project.entries().size()==3&&visible->project.find(aid(3)));
  }
  for(unsigned mode=0;mode<2;++mode){
    BrowserFixture f(0);typed_fixture(f,3,false);auto visible=initial(f),prior=visible;
    auto gate=f.block("typed-2.json");d::ProjectBrowserTask task(f.factory());CHECK(task.start(f.request()));gate->wait();
    CHECK(task.busy()&&visible==prior);if(mode)CHECK(task.cancel());
    gate->release();const auto done=finish(task,visible,mode?1:2);
    CHECK_ERROR(done.result,mode?ErrorCode::Cancelled:ErrorCode::StaleHandle);CHECK(!done.changed&&visible==prior&&!task.busy());
    CHECK(task.start(f.request()));const auto retry=finish(task,visible);
    CHECK(retry.result&&retry.changed&&visible!=prior&&visible->project.format()==prior->project.format());
  }
  struct ReservedSurface final:FakeFiles {
    Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{
      auto value=FakeFiles::read(leaf,limit);if(value&&leaf=="typed-2.json")value.value().reserve(limit+1);return value;
    }
  };
  BrowserFixture f(0);typed_fixture(f);auto visible=initial(f),prior=visible;
  auto files=std::make_shared<ReservedSurface>();files->data=f.files->data;
  d::ProjectBrowserTask task([files](std::string_view)->Result<std::shared_ptr<platform::RootedFiles>>{return std::shared_ptr<platform::RootedFiles>(files);});
  CHECK(task.start(f.request()));const auto done=finish(task,visible);
  CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&visible==prior&&files->read_count()==4);
}

void prior_view_and_refresh_failures(){
  BrowserFixture f;auto visible=initial(f),prior=visible;const auto hash=prior->project.revision();
  const std::vector<std::byte> raw(prior->project.bytes().begin(),prior->project.bytes().end());
  const auto leaf=f.references.back().recipe;const auto recipe=f.files->data[leaf];d::ProjectBrowserTask task(f.factory());
  for(unsigned mode=0;mode<4;++mode){
    f.files->data[leaf]=recipe;
    if(mode==0)f.files->data[leaf].push_back(std::byte{' '});
    if(mode==1)f.files->data[leaf]={std::byte{'x'}};
    if(mode==2)f.files->data.erase(leaf);
    if(mode==3)f.files->data[leaf].resize(mesh_recipe::maximum_bytes+1,std::byte{' '});
    f.files->reset_reads();CHECK(task.start(f.request()));const auto done=finish(task,visible);CHECK(!done.result&&!done.changed);metrics(done);
    CHECK(visible==prior&&visible->project.revision()==hash&&std::equal(raw.begin(),raw.end(),visible->project.bytes().begin(),visible->project.bytes().end()));
    CHECK(f.files->read_count()==3&&f.files->writes==0);
  }
  f.files->data[leaf]=recipe;f.files->reset_reads();CHECK(task.start(f.request()));CHECK(finish(task,visible).result);
  CHECK(visible!=prior&&visible->project.revision()==hash&&prior->project.revision()==hash&&f.files->read_count()==3);
}
void index_and_capacity_rejection(){
  BrowserFixture f;auto visible=initial(f),prior=visible;const auto saved=f.files->data["project.raceproject"];
  // Binary structural checks reject corruption before any recipe I/O;
  // legacy JSON is rejected even with the canonical binary extension.
  std::array<std::vector<std::byte>,5> malformed{saved,saved,saved,saved,saved};
  malformed[0]={std::byte{'{'},std::byte{'}'}};
  malformed[1].back()^=std::byte{1};
  malformed[2][20]=std::byte{33};
  malformed[3].resize(p::maximum_bytes+1);
  malformed[4][8]=std::byte{2};
  const std::array codes{ErrorCode::IncompatibleVersion,ErrorCode::InvalidInput,ErrorCode::Exhausted,ErrorCode::Exhausted,ErrorCode::IncompatibleVersion};
  d::ProjectBrowserTask task(f.factory());
  for(std::size_t n=0;n<malformed.size();++n){
    put(*f.files,"project.raceproject",malformed[n]);f.files->reset_reads();CHECK(task.start(f.request()));auto done=finish(task,visible);
    CHECK_ERROR(done.result,codes[n]);CHECK(!done.changed&&visible==prior&&f.files->read_count()==1);
  }
  f.files->data["project.raceproject"]=saved;
  struct Reserved final:FakeFiles {
    std::string oversized;
    Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{
      auto value=FakeFiles::read(leaf,limit);if(value&&leaf==oversized)value.value().reserve(limit+1);return value;
    }
  };
  for(const auto& leaf:{std::string("project.raceproject"),f.references.back().recipe}){
    auto files=std::make_shared<Reserved>();files->data=f.files->data;files->oversized=leaf;
    d::ProjectBrowserTask reserved([files](std::string_view)->Result<std::shared_ptr<platform::RootedFiles>>{return std::shared_ptr<platform::RootedFiles>(files);});
    CHECK(reserved.start(f.request()));auto done=finish(reserved,visible);CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&visible==prior);
    CHECK(files->read_count()==(leaf=="project.raceproject"?1:3));
  }
}
void staleness_and_sticky_cancel(){
  for(unsigned mode=0;mode<3;++mode){
    BrowserFixture f;auto visible=initial(f),prior=visible;auto gate=f.block();d::ProjectBrowserTask task(f.factory());
    const auto first=task.start(f.request());CHECK(first);gate->wait();CHECK_ERROR(task.start(f.request(2)),ErrorCode::Busy);
    auto pending=task.poll(1,visible);CHECK(pending&&!pending.value()&&task.busy()&&visible==prior);
    std::uint64_t epoch=1;
    if(mode==0)epoch=2;
    if(mode==1){CHECK(task.cancel());CHECK(task.cancel());}
    if(mode==2){epoch=2;CHECK(task.cancel());epoch=1;} // Explicit cancellation protects project ABA.
    gate->release();auto done=finish(task,visible,epoch);
    CHECK_ERROR(done.result,mode==0?ErrorCode::StaleHandle:ErrorCode::Cancelled);CHECK(!done.changed&&visible==prior&&!task.busy());
    const auto second=task.start(f.request(epoch));CHECK(second&&second.value()==first.value()+1);
    auto fresh=finish(task,visible,epoch);CHECK(fresh.result&&fresh.changed&&visible!=prior&&visible->source.epoch==epoch);
  }
}
void owner_admission_and_factory_errors(){
  BrowserFixture f;auto visible=initial(f),prior=visible;d::ProjectBrowserTask task(f.factory());
  std::thread foreign([&]{
    CHECK_ERROR(task.start(f.request()),ErrorCode::WrongThread);CHECK_ERROR(task.cancel(),ErrorCode::WrongThread);CHECK_ERROR(task.poll(1,visible),ErrorCode::WrongThread);
  });foreign.join();CHECK(!task.busy()&&visible==prior&&f.files->read_count()==0);
  for(unsigned mode=0;mode<9;++mode){
    auto request=f.request();
    if(mode==0)request.recipe_root.clear();if(mode==1)request.recipe_root.assign(4097,'a');if(mode==2)request.recipe_root.push_back('\0');
    if(mode==3)request.project_leaf="../project.raceproject";if(mode==4)request.project_leaf="PROJECT.JSON";
    if(mode==5)request.project_leaf="project.obj";if(mode==6)request.project_leaf="con.json";
    if(mode==7)request.project_leaf=std::string(97,'a')+".json";if(mode==8)request.epoch=0;
    CHECK_ERROR(task.start(std::move(request)),ErrorCode::InvalidInput);CHECK(!task.busy()&&visible==prior&&f.files->read_count()==0);
  }
  for(unsigned mode=0;mode<2;++mode){auto request=f.request();if(mode==0)request.recipe_root.reserve(8192);else request.project_leaf.reserve(1024);
    CHECK_ERROR(task.start(std::move(request)),ErrorCode::Exhausted);CHECK(!task.busy());}
  for(unsigned mode=0;mode<3;++mode){
    d::ProjectBrowserTask broken([mode](std::string_view)->Result<std::shared_ptr<platform::RootedFiles>>{
      if(mode==0)throw std::runtime_error("browser fixture");if(mode==1)throw std::bad_alloc();return std::shared_ptr<platform::RootedFiles>{};
    });
    CHECK(broken.start(f.request()));auto done=finish(broken,visible);
    CHECK_ERROR(done.result,mode==0?ErrorCode::PlatformFailure:mode==1?ErrorCode::Exhausted:ErrorCode::InvalidInput);CHECK(!done.changed&&visible==prior&&!broken.busy());
  }
}
void blocked_shutdown_retains_owners(){
  BrowserFixture f;auto gate=f.block();std::weak_ptr<FakeFiles> owner=f.files;
  auto task=std::make_unique<d::ProjectBrowserTask>([owner](std::string_view)->Result<std::shared_ptr<platform::RootedFiles>>{
    return std::shared_ptr<platform::RootedFiles>(owner.lock());
  });
  auto request=f.request();CHECK(task->start(std::move(request)));gate->wait();f.files.reset();CHECK(!owner.expired());
  CHECK(task->cancel());std::shared_ptr<const d::ProjectBrowserView> visible;auto pending=task->poll(1,visible);CHECK(pending&&!pending.value()&&task->busy());
  std::atomic<bool> destroyed{};std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load()&&!owner.expired());gate->release();});
  task.reset();destroyed=true;release.join();CHECK(owner.expired());
}
struct ScopedRoot final:platform::RootedFiles {
  std::shared_ptr<platform::RootedFiles> inner;
  std::shared_ptr<std::atomic<bool>> closed;
  explicit ScopedRoot(std::shared_ptr<platform::RootedFiles> value,std::shared_ptr<std::atomic<bool>> signal={})
      :inner(std::move(value)),closed(std::move(signal)){}
  ~ScopedRoot() override{d::profile::testing::fail_next_allocation_attempts(0);if(closed)closed->store(true,std::memory_order_release);}
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override{return inner->read(leaf,limit);}
  Result<void> publish_immutable(std::string_view leaf,std::span<const std::byte> value) override{return inner->publish_immutable(leaf,value);}
  Result<void> replace(std::string_view leaf,std::span<const std::byte> value,platform::Durability mode) override{return inner->replace(leaf,value,mode);}
};
void allocation_failure_recovery_and_publication(){
  std::size_t admission_failures{},worker_failures{};
  for(std::size_t step=0;step<64;++step){
    phase="allocation-admission";fault_step=step;BrowserFixture f;auto visible=initial(f),prior=visible;d::ProjectBrowserTask task(f.factory());auto request=f.request();
    d::profile::testing::fail_allocation_after(step);auto admitted=task.start(std::move(request));d::profile::testing::fail_next_allocation_attempts(0);
    if(admitted){CHECK(finish(task,visible).result);break;}
    CHECK_ERROR(admitted,ErrorCode::Exhausted);CHECK(!task.busy()&&visible==prior&&f.files->read_count()==0);
    CHECK(task.start(f.request()));CHECK(finish(task,visible).result);++admission_failures;
  }
  CHECK(admission_failures>0&&admission_failures<64);
  for(std::size_t step=0;step<128;++step){
    phase="allocation-worker";fault_step=step;BrowserFixture f;auto visible=initial(f),prior=visible;auto base=f.factory();auto armed=std::make_shared<std::atomic<bool>>(true);
    d::ProjectBrowserTask task([base,step,armed](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>>{
      auto root=base(name);if(!root)return root.error();auto scoped=std::make_shared<ScopedRoot>(std::move(root).value());
      if(armed->load())if(armed->exchange(false))d::profile::testing::fail_allocation_after(step);return std::shared_ptr<platform::RootedFiles>(std::move(scoped));
    });
    CHECK(task.start(f.request()));auto done=finish(task,visible);
    if(done.result){CHECK(done.changed);break;}
    CHECK_ERROR(done.result,ErrorCode::Exhausted);CHECK(!done.changed&&visible==prior&&!task.busy());++worker_failures;
    armed->store(false);CHECK(task.start(f.request()));auto retry=finish(task,visible);CHECK(retry.result&&retry.changed&&visible!=prior);
  }
  CHECK(worker_failures>4&&worker_failures<128);
  BrowserFixture f;auto visible=initial(f),prior=visible;auto closed=std::make_shared<std::atomic<bool>>();auto base=f.factory();
  d::ProjectBrowserTask task([base,closed](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>>{
    auto root=base(name);if(!root)return root.error();return std::shared_ptr<platform::RootedFiles>(std::make_shared<ScopedRoot>(std::move(root).value(),closed));
  });
  CHECK(task.start(f.request()));const auto deadline=std::chrono::steady_clock::now()+10s;
  while(!closed->load(std::memory_order_acquire)){CHECK(std::chrono::steady_clock::now()<deadline);std::this_thread::sleep_for(1ms);}
  std::uint64_t allocations{};
  {d::profile::AllocationScope scope;const auto before=d::profile::allocation_snapshot();d::profile::testing::fail_next_allocation_attempts(1);
    auto done=finish(task,visible);d::profile::testing::fail_next_allocation_attempts(0);const auto after=d::profile::allocation_snapshot();
    CHECK(done.result&&done.changed&&visible!=prior&&!after.overflowed);allocations=after.requests-before.requests;CHECK(allocations==0);}
  CHECK(f.files->read_count()==3);std::printf("browser allocation failures admission=%zu worker=%zu publication=%llu\n",admission_failures,worker_failures,static_cast<unsigned long long>(allocations));
}
} // namespace
int main(){
  phase="snapshots";complete_snapshots_and_exact_reads();phase="mixed profiles";mixed_profiles_preserve_raw_pins();phase="typed closure";typed_family_closure_and_lifetime();phase="typed failures";typed_failures_and_stale_publication();phase="refresh";prior_view_and_refresh_failures();phase="bounds";index_and_capacity_rejection();
  phase="stale";staleness_and_sticky_cancel();phase="owner";owner_admission_and_factory_errors();phase="shutdown";blocked_shutdown_retains_owners();
  phase="allocation";allocation_failure_recovery_and_publication();std::puts("project browser closure, publication, cancellation and ownership passed");
}
