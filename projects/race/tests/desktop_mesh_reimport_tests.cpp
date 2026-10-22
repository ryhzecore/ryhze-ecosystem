#include "desktop_mesh_reimport.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace {
namespace d=race::desktop;
namespace p=race::platform::process;
constexpr std::string_view source_a="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view source_uv="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
constexpr std::string_view source_b="v 0 0 0\nv 3 0 0\nv 0 2 0\nf 1 2 3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}

struct Files final:FakeFiles {
  bool corrupt_read{},oversize_read{};
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    auto result=FakeFiles::read(leaf,limit);if(!result)return result.error();
    std::lock_guard lock(mutex);
    if(corrupt_read&&!result.value().empty())result.value().back()^=std::byte{1};
    if(oversize_read)result.value().reserve(limit+1);return result;
  }
  void put(std::string_view leaf,std::span<const std::byte> value) {
    std::lock_guard lock(mutex);data[std::string(leaf)]={value.begin(),value.end()};
  }
  std::vector<std::byte> get(std::string_view leaf) {
    std::lock_guard lock(mutex);return data.at(std::string(leaf));
  }
  void append(std::string_view leaf,std::byte value) {
    std::lock_guard lock(mutex);data.at(std::string(leaf)).push_back(value);
  }
  std::size_t write_count(){std::lock_guard lock(mutex);return writes;}
  void reset_faults(){std::lock_guard lock(mutex);corrupt_read=oversize_read=throw_read=false;fail_write=SIZE_MAX;}
};

// All shared child state is synchronized. Service calls and destruction must
// still occur on the worker which constructed the Service.
struct ProcessState {
  std::mutex mutex;
  std::condition_variable changed;
  std::thread::id owner;
  p::Request request{};
  p::Snapshot snapshot{};
  std::weak_ptr<Files> stage;
  std::shared_ptr<Gate> destruction_gate;
  bool hold{},started{},stopped{},destroyed{},fail_start{},fail_stop{};
  int product_failure{},factory_failure{},root_failure{};
  std::atomic<std::size_t> factory_calls{};
  void complete(bool streams=true) {
    std::lock_guard lock(mutex);snapshot.phase=p::Phase::Completed;
    snapshot.exit_code=0;snapshot.stdout_finished=snapshot.stderr_finished=streams;changed.notify_all();
  }
  void wait_started() {
    std::unique_lock lock(mutex);CHECK(changed.wait_for(lock,10s,[&]{return started;}));
  }
  bool was_stopped(){std::lock_guard lock(mutex);return stopped;}
  bool was_destroyed(){std::lock_guard lock(mutex);return destroyed;}
  std::shared_ptr<Files> stage_owner(){std::lock_guard lock(mutex);return stage.lock();}
  void reset_faults() {
    std::lock_guard lock(mutex);hold=fail_start=fail_stop=false;
    product_failure=factory_failure=root_failure=0;destruction_gate.reset();
  }
};
class FakeProcess final:public p::Service {
  std::shared_ptr<ProcessState> state_;
  const std::thread::id owner_{std::this_thread::get_id()};
  void owner() const noexcept {CHECK(std::this_thread::get_id()==owner_);}
public:
  FakeProcess(std::shared_ptr<ProcessState> state,std::shared_ptr<Files> stage,std::uint64_t identity)
      :state_(std::move(state)) {
    std::lock_guard lock(state_->mutex);CHECK(identity&&identity!=state_->request.service);
    state_->owner=owner_;state_->request={identity,1};state_->snapshot=p::Snapshot{state_->request};
    state_->stage=std::move(stage);state_->started=state_->stopped=state_->destroyed=false;
  }
  ~FakeProcess() override {
    owner();std::shared_ptr<Gate> gate;
    {std::lock_guard lock(state_->mutex);CHECK(!state_->stage.expired());gate=state_->destruction_gate;}
    if(gate)gate->arrive();
    std::lock_guard lock(state_->mutex);CHECK(!state_->stage.expired());
    state_->snapshot.phase=p::Phase::Completed;
    state_->snapshot.stdout_finished=state_->snapshot.stderr_finished=true;
    state_->destroyed=true;state_->changed.notify_all();
  }
  Result<p::Request> start(const p::Launch& launch) override {
    owner();CHECK(launch.executable=="C:/trusted-mesh-worker.exe"&&launch.working_directory=="C:/fresh-stage");
    CHECK((launch.arguments.size()==2||launch.arguments.size()==3)&&launch.arguments[0]==launch.working_directory);
    if(launch.arguments.size()==3)CHECK(launch.arguments[2]=="geometry-uv");
    auto id=assets::parse_id(launch.arguments[1]);CHECK(id);
    int failure{};bool held{};std::shared_ptr<Files> stage;
    {std::lock_guard lock(state_->mutex);
      if(state_->fail_start)return fail(ErrorCode::PlatformFailure,"fixture launch failure");
      failure=state_->product_failure;held=state_->hold;stage=state_->stage.lock();}
    CHECK(stage);auto input=stage->get("input.obj");
    if(failure!=4) {
      auto imported=mesh_pipeline::import_obj(failure==1?aid(99):id.value(),failure==3?bytes(source_b):std::span<const std::byte>(input));CHECK(imported);
      if(failure==2)imported.value().back()^=std::byte{1};stage->put("result.rasset",imported.value());
    }
    std::lock_guard lock(state_->mutex);state_->started=true;
    if(!held){state_->snapshot.phase=p::Phase::Completed;state_->snapshot.exit_code=0;
      state_->snapshot.stdout_finished=state_->snapshot.stderr_finished=true;}
    if(failure==5)state_->snapshot.exit_code=7;
    if(failure==6)state_->snapshot.failure=fail(ErrorCode::Exhausted,"fixture stream overflow");
    if(failure==7)++state_->snapshot.request.serial;
    if(failure==8)state_->snapshot.exit_code.reset();
    state_->changed.notify_all();return state_->request;
  }
  Result<p::Snapshot> poll(p::Request request) noexcept override {
    owner();std::lock_guard lock(state_->mutex);CHECK(request==state_->request);return state_->snapshot;
  }
  Result<p::Snapshot> wait(p::Request request,std::uint32_t milliseconds) noexcept override {
    owner();std::unique_lock lock(state_->mutex);CHECK(request==state_->request);
    if(state_->snapshot.phase!=p::Phase::Completed&&milliseconds)
      state_->changed.wait_for(lock,std::chrono::milliseconds(std::min(milliseconds,2U)),[&]{return state_->snapshot.phase==p::Phase::Completed;});
    if(state_->snapshot.phase!=p::Phase::Completed)return fail(ErrorCode::Timeout,"fixture process still live");
    return state_->snapshot;
  }
  Result<void> request_stop(p::Request request) noexcept override {
    owner();std::lock_guard lock(state_->mutex);CHECK(request==state_->request);state_->stopped=true;
    if(state_->fail_stop)return fail(ErrorCode::PlatformFailure,"fixture stop failure");
    state_->snapshot.phase=p::Phase::Completed;state_->snapshot.exit_code=1;
    state_->snapshot.stdout_finished=state_->snapshot.stderr_finished=true;state_->changed.notify_all();return {};
  }
  Result<std::span<const std::byte>> stdout_prefix(p::Request) const noexcept override {owner();return std::span<const std::byte>{};}
  Result<std::span<const std::byte>> stderr_prefix(p::Request) const noexcept override {owner();return std::span<const std::byte>{};}
  Result<p::Completion> take(p::Request) noexcept override {owner();return fail(ErrorCode::Unsupported,"fixture take unused");}
};

struct Fixture {
  std::shared_ptr<Files> recipes=std::make_shared<Files>(),sources=std::make_shared<Files>(),
      products=std::make_shared<Files>(),checkpoints=std::make_shared<Files>();
  std::shared_ptr<ProcessState> child=std::make_shared<ProcessState>();
  std::shared_ptr<const d::ProjectBrowserView> view;
  std::shared_ptr<const d::MeshReimportCheckpoint> checkpoint;
  explicit Fixture(mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry) {
    std::array<mesh_project::Reference,2> references;
    for(std::size_t n=0;n<references.size();++n){
      const auto leaf="recipe-"+std::to_string(n)+".json";
      auto recipe=mesh_recipe::encode({aid(n+1),"source.obj",n==0?profile:mesh_pipeline::Profile::Geometry});CHECK(recipe);
      recipes->put(leaf,bytes(recipe.value()));references[n]={aid(n+1),leaf,digest(bytes(recipe.value()))};
    }
    auto index=mesh_project::encode(references);CHECK(index);recipes->put("project.raceproject",std::span<const std::byte>(index.value()));
    sources->put("source.obj",bytes(profile==mesh_pipeline::Profile::GeometryUv?source_uv:source_a));view=clone();recipes->reset_reads();
  }
  d::MeshReimportRequest request() const {return {"sources","products","checkpoints","staging",9};}
  std::shared_ptr<const d::ProjectBrowserView> clone() {
    auto loaded=content_project::load(*recipes,"project.raceproject");CHECK(loaded);
    return std::make_shared<d::ProjectBrowserView>(d::ProjectBrowserRequest{"recipes","project.raceproject",9},std::move(loaded).value());
  }
  void typed_project() {
    std::vector<content_project::Reference> refs;
    for(const auto& entry:view->project.entries())refs.push_back(entry.reference);
    for(unsigned n=0;n<2;++n){
      const auto leaf=n?"material.json":"texture.json";
      const auto profile=n?surface_pipeline::Profile::Material:surface_pipeline::Profile::BmpSrgbMips;
      auto raw=surface_recipe::encode({aid(n+3),n?"material-source.json":"texture.bmp",profile});CHECK(raw);
      recipes->put(leaf,bytes(raw.value()));
      refs.push_back({aid(n+3),n?content_project::Kind::Material:content_project::Kind::Texture,leaf,digest(bytes(raw.value()))});
    }
    auto index=content_project::encode_binary(refs);CHECK(index);const auto& exact=index.value();
    recipes->put("project.raceproject",std::span<const std::byte>(exact));view=clone();recipes->reset_reads();
  }
  void repin_index() {
    std::vector<content_project::Reference> refs;
    for(const auto& row:view->project.entries())refs.push_back(row.reference);
    recipes->put("alias.json",recipes->get(refs.back().recipe));refs.back().recipe="alias.json";
    auto changed=content_project::encode_binary(refs);CHECK(changed);recipes->put("project.raceproject",changed.value());
  }
  void seed_checkpoint() {
    auto imported=mesh_pipeline::import_obj(aid(1),bytes(source_b));CHECK(imported);
    auto cooked=mesh_pipeline::cook(imported.value());CHECK(cooked);
    const mesh_checkpoint::Definition definition{aid(1),view->project.revision(),view->project.find(aid(1)).value()->reference.revision,
        digest(bytes(source_b)),digest(cooked.value())};
    auto encoded=mesh_checkpoint::encode(definition);CHECK(encoded);
    const auto revision=digest(bytes(encoded.value()));const auto leaf=hex(revision)+".json";
    products->put(hex(definition.cooked_revision)+".rasset",cooked.value());checkpoints->put(leaf,bytes(encoded.value()));
    checkpoint=std::make_shared<d::MeshReimportCheckpoint>(view,request(),mesh_checkpoint::Published{definition,revision,leaf});
  }
  d::MeshReimportTask::RootFactory roots() const {
    return [recipes=recipes,sources=sources,products=products,checkpoints=checkpoints,child=child,
        main=std::this_thread::get_id()](std::string_view name)->Result<std::shared_ptr<platform::RootedFiles>> {
      CHECK(std::this_thread::get_id()!=main);int failure{};
      {std::lock_guard lock(child->mutex);failure=child->root_failure;}
      if(failure==1)throw std::bad_alloc();if(failure==2)throw std::runtime_error("fixture root failure");
      if(name=="recipes")return std::shared_ptr<platform::RootedFiles>(recipes);
      if(name=="sources")return std::shared_ptr<platform::RootedFiles>(sources);
      if(name=="products")return std::shared_ptr<platform::RootedFiles>(products);
      if(name=="checkpoints")return std::shared_ptr<platform::RootedFiles>(checkpoints);
      return fail(ErrorCode::NotFound,"fixture root missing");
    };
  }
  d::MeshReimportTask::WorkerFactory workers() const {
    return [child=child,main=std::this_thread::get_id()](std::uint64_t identity,const d::MeshReimportRequest& names)->Result<d::MeshReimportWorker> {
      CHECK(std::this_thread::get_id()!=main&&names.staging_root=="staging");++child->factory_calls;
      int failure{};{std::lock_guard lock(child->mutex);failure=child->factory_failure;}
      if(failure==1)throw std::bad_alloc();if(failure==2)throw std::runtime_error("fixture worker failure");
      auto stage=std::make_shared<Files>();d::MeshReimportWorker worker;
      worker.stage=stage;worker.executable="C:/trusted-mesh-worker.exe";worker.directory="C:/fresh-stage";
      if(failure!=3)worker.process=std::make_unique<FakeProcess>(child,stage,identity);
      if(failure==4)worker.directory.reserve(8192);return worker;
    };
  }
  void reset_faults(){recipes->reset_faults();sources->reset_faults();products->reset_faults();checkpoints->reset_faults();child->reset_faults();}
};
d::MeshReimportCompletion finish(d::MeshReimportTask& task,std::shared_ptr<const d::ProjectBrowserView> view,
    AssetId selected,std::uint64_t epoch,std::shared_ptr<const d::MeshReimportCheckpoint>& checkpoint) {
  const auto deadline=std::chrono::steady_clock::now()+10s;
  while(std::chrono::steady_clock::now()<deadline){
    auto polled=task.poll(view,selected,epoch,checkpoint);CHECK(polled);
    if(polled.value())return std::move(*polled.value());std::this_thread::sleep_for(1ms);
  }
  std::fprintf(stderr,"editor reimport completion timed out, busy=%d\n",int(task.busy()));CHECK(false);return {};
}
auto finish(d::MeshReimportTask& task,Fixture& f){return finish(task,f.view,aid(1),9,f.checkpoint);}

void success_and_exact_reads() {
  Fixture f;d::MeshReimportTask task(f.roots(),f.workers());CHECK(task.progress().value().phase==d::MeshReimportPhase::Idle);
  auto admitted=task.start(f.view,aid(1),f.request());CHECK(admitted&&task.busy());auto done=finish(task,f);
  CHECK(done.result&&done.changed&&!task.busy()&&done.request==admitted.value()&&done.epoch==9&&done.selected==aid(1));
  CHECK(done.source==digest(bytes(source_a))&&done.source_bytes==source_a.size());
  CHECK(done.product_bytes>0&&done.retained_bytes<=d::maximum_checkpoint_retained_bytes);
  CHECK(done.admission_ms>=0&&done.revalidate_ms>=0&&done.source_ms>=0&&done.import_ms>=0&&done.cook_publish_ms>=0&&done.capture_ms>=0&&done.publication_ms>=0);
  CHECK(f.recipes->read_count()==3&&f.sources->read_count()==1&&f.products->read_count()==3&&f.checkpoints->read_count()==1);
  CHECK(f.products->write_count()==2&&f.checkpoints->write_count()==1&&f.child->factory_calls==1&&f.child->was_destroyed());
  CHECK(f.checkpoint&&f.checkpoint->project==f.view&&f.checkpoint->roots.epoch==9&&f.checkpoint->published.definition.source==done.source);
  auto loaded=mesh_checkpoint::load(*f.checkpoints,f.checkpoint->published.leaf,f.view->project,*f.products);CHECK(loaded);
  auto expected=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(1),bytes(source_a)).value());CHECK(expected);
  CHECK(std::equal(loaded.value().product().begin(),loaded.value().product().end(),expected.value().begin(),expected.value().end()));
}
void typed_project_reimport() {
  for(auto profile:{mesh_pipeline::Profile::Geometry,mesh_pipeline::Profile::GeometryUv}){
    Fixture f(profile);f.typed_project();const auto index=f.recipes->get("project.raceproject");
    d::MeshReimportTask task(f.roots(),f.workers());
    for(auto nonmesh:{aid(3),aid(4)}){
      CHECK_ERROR(task.start(f.view,nonmesh,f.request()),ErrorCode::InvalidDependency);
      CHECK(!task.busy()&&f.recipes->read_count()==0&&f.sources->read_count()==0&&f.child->factory_calls==0);
    }
    CHECK(task.start(f.view,aid(1),f.request()));const auto done=finish(task,f);
    CHECK(done.result&&done.changed&&f.view->project.schema()==2&&f.recipes->read_count()==5&&f.sources->read_count()==1);
    CHECK(f.checkpoint->published.definition.index_revision==digest(index));
    CHECK(f.checkpoint->published.definition.recipe_revision==f.view->project.find(aid(1)).value()->reference.revision);
    auto loaded=mesh_checkpoint::load(*f.checkpoints,f.checkpoint->published.leaf,f.view->project,*f.products);CHECK(loaded);
    auto product=assets::mesh::validate(loaded.value().product());CHECK(product&&product.value().has_uv()==(profile==mesh_pipeline::Profile::GeometryUv));
  }
  // Revalidation admits the complete typed closure before reading mesh source or
  // creating a child. Mutating an unselected surface cannot be silently ignored.
  for(auto leaf:{"texture.json","material.json","project.raceproject"}){
    Fixture f;f.typed_project();f.seed_checkpoint();const auto prior=f.checkpoint;
    const auto old_products=f.products->data,old_checkpoints=f.checkpoints->data;
    if(std::string_view{leaf}=="project.raceproject")f.repin_index();else f.recipes->append(leaf,std::byte{' '});d::MeshReimportTask task(f.roots(),f.workers());
    CHECK(task.start(f.view,aid(1),f.request()));const auto done=finish(task,f);
    CHECK_ERROR(done.result,ErrorCode::InvalidDependency);CHECK(!done.changed&&f.checkpoint==prior);
    CHECK(f.sources->read_count()==0&&f.child->factory_calls==0&&f.products->data==old_products&&f.checkpoints->data==old_checkpoints);
  }
}
void raw_pins_and_source_copy() {
  for(unsigned mode=0;mode<3;++mode){
    Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;const auto prior_raw=f.checkpoints->get(prior->published.leaf);
    if(mode==0)f.repin_index();
    else f.recipes->append(mode==1?"recipe-0.json":"recipe-1.json",std::byte{' '});
    d::MeshReimportTask task(f.roots(),f.workers());CHECK(task.start(f.view,aid(1),f.request()));auto done=finish(task,f);
    CHECK_ERROR(done.result,ErrorCode::InvalidDependency);CHECK(!done.changed&&f.checkpoint==prior);
    CHECK(f.sources->read_count()==0&&f.child->factory_calls==0&&f.checkpoints->get(prior->published.leaf)==prior_raw);
  }
  Fixture f;f.child->hold=true;d::MeshReimportTask task(f.roots(),f.workers());CHECK(task.start(f.view,aid(1),f.request()));f.child->wait_started();
  auto stage=f.child->stage_owner();CHECK(stage&&stage->get("input.obj")==std::vector<std::byte>(bytes(source_a).begin(),bytes(source_a).end()));
  CHECK(stage->read_count()==3); // Fresh input/result probes and input readback.
  f.sources->put("source.obj",bytes(source_b));f.child->complete();auto done=finish(task,f);
  CHECK(done.result&&done.source==digest(bytes(source_a))&&f.sources->read_count()==1&&stage->read_count()==4);
  CHECK(f.checkpoint->published.definition.source==digest(bytes(source_a)));
}
void stream_boundary_stale_and_cancel() {
  for(unsigned mode=0;mode<4;++mode){
    Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;f.child->hold=true;d::MeshReimportTask task(f.roots(),f.workers());
    CHECK(task.start(f.view,aid(1),f.request()));f.child->wait_started();
    CHECK_ERROR(task.start(f.view,aid(1),f.request()),ErrorCode::Busy);
    auto current=f.view;auto selected=aid(1);std::uint64_t epoch=9;
    if(mode==0)current=f.clone();if(mode==1)selected=aid(2);if(mode==2)++epoch;
    if(mode==3){current=f.clone();CHECK(task.cancel());CHECK(task.cancel());current=f.view;selected=aid(1);epoch=9;}
    if(mode!=3){f.child->complete(false);auto pending=task.poll(current,selected,epoch,f.checkpoint);CHECK(pending&&!pending.value()&&task.busy()&&f.checkpoint==prior);f.child->complete();}
    auto done=finish(task,current,selected,epoch,f.checkpoint);CHECK_ERROR(done.result,mode==3?ErrorCode::Cancelled:ErrorCode::StaleHandle);
    CHECK(!done.changed&&f.checkpoint==prior&&!task.busy()&&f.child->was_destroyed());
    f.child->reset_faults();auto second=task.start(f.view,aid(1),f.request());CHECK(second&&second.value()==done.request+1);CHECK(finish(task,f).result);
  }
  Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;f.child->hold=true;
  auto gate=std::make_shared<Gate>();f.child->destruction_gate=gate;d::MeshReimportTask task(f.roots(),f.workers());
  CHECK(task.start(f.view,aid(1),f.request()));f.child->wait_started();CHECK(task.cancel());gate->wait();
  CHECK(f.child->was_stopped()&&!f.child->was_destroyed());auto pending=task.poll(f.view,aid(1),9,f.checkpoint);
  CHECK(pending&&!pending.value()&&task.busy()&&f.checkpoint==prior&&task.progress().value().cancellation_requested);
  gate->release();auto done=finish(task,f);CHECK_ERROR(done.result,ErrorCode::Cancelled);CHECK(f.checkpoint==prior);
}
void blocked_io_and_shutdown() {
  for(bool shutdown:{false,true}){
    Fixture f;auto gate=std::make_shared<Gate>();f.sources->gate=gate;f.sources->gated="source.obj";
    auto task=std::make_unique<d::MeshReimportTask>(f.roots(),f.workers());CHECK(task->start(f.view,aid(1),f.request()));gate->wait();
    CHECK(task->progress().value().phase==d::MeshReimportPhase::ReadingSource);CHECK(task->cancel());
    auto pending=task->poll(f.view,aid(1),9,f.checkpoint);CHECK(pending&&!pending.value()&&task->busy());
    if(!shutdown){gate->release();auto done=finish(*task,f);CHECK_ERROR(done.result,ErrorCode::Cancelled);CHECK(f.child->factory_calls==0);continue;}
    std::weak_ptr<Files> recipes=f.recipes,sources=f.sources,products=f.products,checkpoints=f.checkpoints;
    std::weak_ptr<const d::ProjectBrowserView> view=f.view;f.recipes.reset();f.sources.reset();f.products.reset();f.checkpoints.reset();f.view.reset();
    std::atomic<bool> destroyed{};
    std::thread release([&]{std::this_thread::sleep_for(5ms);CHECK(!destroyed.load()&&!recipes.expired()&&!sources.expired()&&!view.expired());gate->release();});
    task.reset();destroyed=true;release.join();CHECK(recipes.expired()&&sources.expired()&&products.expired()&&checkpoints.expired()&&view.expired());
  }
  Fixture f;f.child->hold=true;auto gate=std::make_shared<Gate>();f.child->destruction_gate=gate;
  auto task=std::make_unique<d::MeshReimportTask>(f.roots(),f.workers());CHECK(task->start(f.view,aid(1),f.request()));f.child->wait_started();
  std::atomic<bool> destroyed{};std::thread release([&]{gate->wait();CHECK(!destroyed.load()&&f.child->stage_owner());gate->release();});
  task.reset();destroyed=true;release.join();CHECK(f.child->was_stopped()&&f.child->was_destroyed());
}
void owner_and_bounds() {
  Fixture f;d::MeshReimportTask task(f.roots(),f.workers());
  std::thread foreign([&]{CHECK_ERROR(task.start(f.view,aid(1),f.request()),ErrorCode::WrongThread);
    CHECK_ERROR(task.cancel(),ErrorCode::WrongThread);CHECK_ERROR(task.progress(),ErrorCode::WrongThread);
    CHECK_ERROR(task.poll(f.view,aid(1),9,f.checkpoint),ErrorCode::WrongThread);});foreign.join();
  CHECK(!task.busy());CHECK_ERROR(task.cancel(),ErrorCode::NotFound);
  CHECK_ERROR(task.start({},aid(1),f.request()),ErrorCode::InvalidInput);CHECK_ERROR(task.start(f.view,{},f.request()),ErrorCode::InvalidInput);
  CHECK_ERROR(task.start(f.view,aid(99),f.request()),ErrorCode::NotFound);
  for(unsigned root=0;root<4;++root)for(unsigned mode=0;mode<4;++mode){
    auto names=f.request();const std::array<std::string*,4> paths{&names.source_root,&names.product_root,&names.checkpoint_root,&names.staging_root};
    if(mode==0)paths[root]->clear();if(mode==1)paths[root]->push_back('\0');
    if(mode==2)paths[root]->assign(4097,'a');if(mode==3)paths[root]->reserve(8192);
    CHECK_ERROR(task.start(f.view,aid(1),std::move(names)),mode==3?ErrorCode::Exhausted:ErrorCode::InvalidInput);CHECK(!task.busy());
  }
  auto names=f.request();names.epoch=0;CHECK_ERROR(task.start(f.view,aid(1),std::move(names)),ErrorCode::InvalidInput);
  CHECK(f.recipes->read_count()==0&&f.child->factory_calls==0);
  for(unsigned mode=0;mode<3;++mode){
    Fixture bounded;if(mode==0)bounded.sources->put("source.obj",{});
    if(mode==1){std::vector<std::byte> oversized(mesh_pipeline::Limits{}.source_bytes+1);bounded.sources->put("source.obj",oversized);}
    if(mode==2)bounded.sources->oversize_read=true;
    d::MeshReimportTask load(bounded.roots(),bounded.workers());CHECK(load.start(bounded.view,aid(1),bounded.request()));auto done=finish(load,bounded);
    CHECK_ERROR(done.result,mode==0?ErrorCode::InvalidInput:ErrorCode::Exhausted);CHECK(!done.changed&&bounded.child->factory_calls==0);
  }
}
void failures_and_recovery() {
  for(unsigned mode=0;mode<19;++mode){
    Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;const auto old=f.checkpoints->get(prior->published.leaf);
    if(mode==0)f.child->factory_failure=1;if(mode==1)f.child->factory_failure=2;if(mode==2)f.child->factory_failure=3;
    if(mode==3)f.child->fail_start=true;if(mode>=4&&mode<=11)f.child->product_failure=int(mode-3);
    if(mode==12)f.sources->throw_read=true;if(mode==13)f.products->fail_write=0;
    if(mode==14)f.checkpoints->fail_write=0;if(mode==15)f.checkpoints->corrupt_read=true;
    if(mode==16)f.child->factory_failure=4;if(mode==17)f.child->root_failure=1;if(mode==18)f.child->root_failure=2;
    d::MeshReimportTask task(f.roots(),f.workers());CHECK(task.start(f.view,aid(1),f.request()));auto failed=finish(task,f);
    CHECK(!failed.result&&!failed.changed&&!task.busy()&&f.checkpoint==prior&&f.checkpoints->get(prior->published.leaf)==old);
    f.reset_faults();CHECK(task.start(f.view,aid(1),f.request()));auto recovered=finish(task,f);
    CHECK(recovered.result&&recovered.changed&&f.checkpoint!=prior&&f.child->was_destroyed());
  }
  Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;f.child->hold=true;f.child->fail_stop=true;
  d::MeshReimportTask task(f.roots(),f.workers());CHECK(task.start(f.view,aid(1),f.request()));f.child->wait_started();CHECK(task.cancel());
  auto done=finish(task,f);CHECK_ERROR(done.result,ErrorCode::Cancelled);CHECK(!done.changed&&f.checkpoint==prior&&f.child->was_destroyed());
}
void uv_profile_reimport() {
  Fixture f(mesh_pipeline::Profile::GeometryUv);d::MeshReimportTask task(f.roots(),f.workers());
  CHECK(task.start(f.view,aid(1),f.request()));auto first=finish(task,f);
  CHECK(first.result&&first.changed&&f.sources->read_count()==1);
  auto previous=f.checkpoint;const auto first_revision=previous->published.definition.cooked_revision;
  auto first_bytes=f.products->get(hex(first_revision)+".rasset");auto first_view=assets::mesh::validate(first_bytes);CHECK(first_view&&first_view.value().has_uv());
  auto expected=mesh_pipeline::profile_import_stamp(mesh_pipeline::Profile::GeometryUv,digest(bytes(source_uv)));CHECK(expected);
  CHECK(first_view.value().stamp()==mesh_pipeline::cook_stamp(expected.value()));
  auto mapped=std::string(source_uv);const auto position=mapped.find("vt 1 0");CHECK(position!=std::string::npos);mapped.replace(position,6,"vt 0 0");
  f.sources->put("source.obj",bytes(mapped));CHECK(task.start(f.view,aid(1),f.request()));auto second=finish(task,f);
  CHECK(second.result&&second.changed&&f.checkpoint->published.definition.cooked_revision!=first_revision);
  CHECK(f.sources->read_count()==2&&f.products->get(hex(first_revision)+".rasset")==first_bytes);
  previous=f.checkpoint;
  // A valid geometry-only worker output cannot cross the pinned UV profile.
  f.sources->put("source.obj",bytes(source_a));CHECK(task.start(f.view,aid(1),f.request()));auto wrong=finish(task,f);
  CHECK(!wrong.result&&!wrong.changed&&f.checkpoint==previous&&f.child->was_destroyed());
  f.sources->put("source.obj",bytes(source_uv));f.child->hold=true;CHECK(task.start(f.view,aid(1),f.request()));f.child->wait_started();
  CHECK(task.cancel());auto cancelled=finish(task,f);CHECK_ERROR(cancelled.result,ErrorCode::Cancelled);
  CHECK(!cancelled.changed&&f.checkpoint==previous&&f.child->was_destroyed());
}
void admission_allocations() {
  std::size_t failures{};
  for(std::size_t step=0;step<64;++step){
    Fixture f;f.seed_checkpoint();auto prior=f.checkpoint;d::MeshReimportTask task(f.roots(),f.workers());auto names=f.request();
    d::profile::testing::fail_allocation_after(step);auto admitted=task.start(f.view,aid(1),std::move(names));
    d::profile::testing::fail_next_allocation_attempts(0);
    if(admitted){CHECK(finish(task,f).result);break;}
    CHECK_ERROR(admitted,ErrorCode::Exhausted);CHECK(!task.busy()&&f.checkpoint==prior);
    CHECK(task.start(f.view,aid(1),f.request()));CHECK(finish(task,f).result);++failures;
  }
  CHECK(failures>0&&failures<64);std::printf("editor reimport admission allocation failure steps=%zu\n",failures);
}
} // namespace
int main(){
  success_and_exact_reads();typed_project_reimport();raw_pins_and_source_copy();stream_boundary_stale_and_cancel();blocked_io_and_shutdown();
  owner_and_bounds();failures_and_recovery();uv_profile_reimport();admission_allocations();
  std::puts("editor reimport ownership, raw pins, cancellation, bounds and checkpoint selection passed");
}
