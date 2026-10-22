#include "mesh_watch.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <functional>

namespace imp=race::mesh_import;
namespace w=race::platform::directory_watch;
namespace p=race::platform::process;
namespace m=race::assets::mesh;
namespace {
constexpr std::string_view a="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view b="v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n";
auto bytes(std::string_view s){return std::as_bytes(std::span(s));}
struct WatchState {
  inline static std::uint64_t identity{};
  w::Snapshot snapshot{{++identity,1}};
  bool stop_called{},fail_stop{},hold_stop{};
  void change(){++snapshot.observation.generation;snapshot.dirty=true;}
};
struct Watcher final:w::Service {
  std::shared_ptr<WatchState> state;
  explicit Watcher(std::shared_ptr<WatchState> s):state(std::move(s)){}
  Result<w::Snapshot> poll() noexcept override{return state->snapshot;}
  Result<void> acknowledge(w::Observation o) noexcept override {
    if(o!=state->snapshot.observation)return fail(ErrorCode::StaleHandle,"fixture read race");
    state->snapshot.dirty=false;return {};
  }
  Result<void> request_stop() noexcept override {
    state->stop_called=true;state->snapshot.phase=state->hold_stop?w::Phase::Stopping:w::Phase::Stopped;
    if(state->fail_stop)return fail(ErrorCode::PlatformFailure,"fixture watcher stop failure");return {};
  }
  Result<w::Snapshot> wait_stopped(std::uint32_t) noexcept override{return poll();}
};
struct Source final:FakeFiles {
  std::function<void()> after_read;
  Result<std::vector<std::byte>> read(std::string_view n,std::size_t limit) override {
    auto result=FakeFiles::read(n,limit);if(after_read)after_read();return result;
  }
  void set(std::string_view s){data["source.obj"]={bytes(s).begin(),bytes(s).end()};}
};
struct Child {
  std::shared_ptr<FakeFiles> stage=std::make_shared<FakeFiles>();
  p::Snapshot snapshot;
  bool stopped{},destroyed{},fail_start{},fail_stop{};
  explicit Child(std::uint64_t id){snapshot.request={id,1};}
  void complete(){
    auto source=stage->data.at("input.obj");auto product=mesh_pipeline::import_obj(aid(1),source);CHECK(product);
    stage->data["result.rasset"]=std::move(product).value();
    snapshot.phase=p::Phase::Completed;snapshot.exit_code=0;snapshot.stdout_finished=snapshot.stderr_finished=true;
  }
};
struct Process final:p::Service {
  std::shared_ptr<Child> child;
  explicit Process(std::shared_ptr<Child> c):child(std::move(c)){}
  ~Process(){child->destroyed=true;}
  Result<p::Request> start(const p::Launch&) override {
    if(child->fail_start)return fail(ErrorCode::PlatformFailure,"fixture launch failure");return child->snapshot.request;
  }
  Result<p::Snapshot> poll(p::Request) noexcept override{return child->snapshot;}
  Result<p::Snapshot> wait(p::Request r,std::uint32_t) noexcept override{return poll(r);}
  Result<void> request_stop(p::Request) noexcept override {
    child->stopped=true;if(child->fail_stop)return fail(ErrorCode::PlatformFailure,"fixture child stop failure");return {};
  }
  Result<std::span<const std::byte>> stdout_prefix(p::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<std::span<const std::byte>> stderr_prefix(p::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<p::Completion> take(p::Request) noexcept override{return fail(ErrorCode::Unsupported,"unused fixture take");}
};
struct FactoryState {
  inline static std::uint64_t identity{};
  std::vector<std::shared_ptr<Child>> children;
  std::uint32_t calls{};
  bool fail_prepare{},fail_start{};
};
struct Factory final:imp::WorkerFactory {
  std::shared_ptr<FactoryState> state;
  explicit Factory(std::shared_ptr<FactoryState> s):state(std::move(s)){}
  Result<imp::PreparedWorker> prepare(std::uint32_t attempt) override {
    CHECK(attempt==++state->calls);
    for(const auto& old:state->children)CHECK(old->destroyed);
    if(state->fail_prepare)return fail(ErrorCode::IoFailure,"fixture stage failure");
    auto child=std::make_shared<Child>(++FactoryState::identity);child->fail_start=state->fail_start;state->children.push_back(child);
    return imp::PreparedWorker{child->stage,"C:/worker.exe","C:/stage",std::make_unique<Process>(child)};
  }
};
struct Setup {
  inline static std::uint64_t identity{};
  std::shared_ptr<Source> source=std::make_shared<Source>();
  std::shared_ptr<FakeFiles> output=std::make_shared<FakeFiles>();
  std::shared_ptr<WatchState> watcher=std::make_shared<WatchState>();
  std::shared_ptr<FactoryState> factory=std::make_shared<FactoryState>();
  TrackedAllocator memory=TrackedAllocator::create({64*1024*1024}).value();
  std::unique_ptr<imp::Watch> owner;
  explicit Setup(imp::WatchLimits limits={},mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){
    source->set(a);auto reimport=imp::Reimport::create(++identity,aid(1),output,memory,{},profile);CHECK(reimport);
    auto made=imp::Watch::create(source,"source.obj",std::make_unique<Watcher>(watcher),std::make_unique<Factory>(factory),std::move(reimport).value(),limits);
    CHECK(made);owner=std::move(made).value();
  }
  imp::WatchStatus poll(){auto result=owner->poll();CHECK(result);return result.value();}
  std::shared_ptr<Child> child(){return factory->children.back();}
  void change(std::string_view s=b){source->set(s);watcher->change();}
  m::ResourceHandle install(){CHECK(poll().outcome==imp::WatchOutcome::Admitted);child()->complete();CHECK(poll().outcome==imp::WatchOutcome::Published);return owner->current().value();}
};
void uv_coalescing(){
  constexpr std::string_view uv="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
  Setup f({4,3,true},mesh_pipeline::Profile::GeometryUv);f.source->set(uv);auto old=f.install();
  auto lease=f.owner->acquire(old);CHECK(lease&&lease.value()->view().has_uv());
  f.change(uv);auto same=f.poll();CHECK(same.outcome==imp::WatchOutcome::Unchanged&&same.unchanged==1&&same.attempts==1&&f.factory->calls==1);
  auto changed=std::string(uv);changed.replace(changed.find("vt 1 0"),6,"vt 0 0");f.change(changed);
  CHECK(f.poll().outcome==imp::WatchOutcome::Admitted);f.child()->complete();CHECK(f.poll().outcome==imp::WatchOutcome::Published);
  const auto latest=f.owner->current().value();CHECK(latest!=old&&lease.value()->view().stamp().source==assets::digest(bytes(uv)));
  f.change(a);CHECK(f.poll().outcome==imp::WatchOutcome::Admitted);f.child()->complete();CHECK(f.poll().outcome==imp::WatchOutcome::Rejected);
  CHECK(f.owner->current().value()==latest&&f.child()->destroyed);
}
void supersession(){
  for(bool completed:{false,true}){
    Setup f;auto old=f.install();auto lease=f.owner->acquire(old);CHECK(lease);
    f.change();CHECK(f.poll().worker_retained);auto obsolete=f.child();if(completed)obsolete->complete();
    f.change(a);auto s=f.poll();CHECK(obsolete->stopped&&s.superseded==1);
    if(!completed){
      CHECK(!obsolete->destroyed&&s.source_reads==2&&s.attempts==2);
      f.change(b);f.poll();f.change(a);f.poll();CHECK(f.factory->calls==2&&f.source->reads==2);
      obsolete->complete();s=f.poll();
    }
    CHECK(obsolete->destroyed&&s.attempts==3&&f.owner->current().value()==old);
    CHECK(f.child()->stage->data.at("input.obj")==std::vector<std::byte>(bytes(a).begin(),bytes(a).end()));
    f.child()->complete();CHECK(f.poll().outcome==imp::WatchOutcome::Published);
    CHECK(lease.value()->view().stamp().source==assets::digest(bytes(a)));
  }
}
void read_races_and_retries(){
  Setup f;f.source->after_read=[&]{f.watcher->change();};auto s=f.poll();
  CHECK(s.attempts==0&&!s.worker_retained&&f.source->reads==1);
  f.source->after_read={};CHECK(f.poll().attempts==1);f.child()->complete();f.poll();auto old=f.owner->current().value();
  f.source->data.clear();f.watcher->change();
  const auto reads=f.source->reads;
  for(unsigned n=1;n<=3;++n){s=f.poll();CHECK(s.read_failures==n&&s.outcome==imp::WatchOutcome::SourceUnavailable);}
  for(unsigned n=0;n<8;++n)f.poll();CHECK(f.source->reads==reads+3&&f.owner->current().value()==old);
  f.change();CHECK(f.poll().attempts==2);f.child()->complete();CHECK(f.poll().outcome==imp::WatchOutcome::Published);
  f.change(a);f.poll();auto completed=f.child();completed->complete();f.source->data.clear();f.watcher->change();
  s=f.poll();CHECK(completed->stopped&&completed->destroyed&&s.outcome==imp::WatchOutcome::SourceUnavailable);
  CHECK(f.owner->current().value()!=old);
}
void attempts_and_failure(){
  for(bool prepare:{false,true}){
    Setup f({2,3,false});auto old=f.install();auto held=f.owner->acquire(old);CHECK(held);
    f.factory->fail_prepare=prepare;f.factory->fail_start=!prepare;f.change();auto s=f.poll();
    CHECK(s.attempts==2&&s.outcome==imp::WatchOutcome::Rejected&&!s.worker_retained);
    for(unsigned n=0;n<5;++n)f.poll();CHECK(f.factory->calls==2&&f.owner->current().value()==old);
    f.change();s=f.poll();CHECK(s.phase==imp::WatchPhase::Stopping&&s.terminal_error->code==ErrorCode::Exhausted);
    CHECK(f.poll().phase==imp::WatchPhase::Stopped&&f.factory->calls==2);
  }
  Setup f({1,3,false});auto old=f.install();CHECK(f.poll().phase==imp::WatchPhase::Running&&f.owner->current().value()==old);
  // A bad terminal product preserves current and a new observation recovers.
  Setup recovery;old=recovery.install();recovery.change();recovery.poll();recovery.child()->complete();recovery.child()->stage->data["result.rasset"].back()^=std::byte{1};
  CHECK(recovery.poll().outcome==imp::WatchOutcome::Rejected&&recovery.owner->current().value()==old);
  recovery.change();recovery.install();
}
void stopping_and_identity(){
  Setup f;f.poll();auto child=f.child();child->fail_stop=true;f.watcher->hold_stop=f.watcher->fail_stop=true;
  CHECK_ERROR(f.owner->request_stop(),ErrorCode::PlatformFailure);CHECK(child->stopped&&f.watcher->stop_called);
  CHECK(f.poll().phase==imp::WatchPhase::Stopping&&!child->destroyed);
  child->complete();CHECK(f.poll().phase==imp::WatchPhase::Stopping&&child->destroyed&&f.output->data.empty());
  f.watcher->snapshot.phase=w::Phase::Stopped;CHECK(f.poll().phase==imp::WatchPhase::Stopped);
  for(unsigned variant=0;variant<3;++variant){
    Setup foreign;foreign.poll();foreign.child()->complete();
    if(variant==0)++foreign.watcher->snapshot.observation.watcher;
    if(variant==1)--foreign.watcher->snapshot.observation.generation;
    if(variant==2){foreign.watcher->change();foreign.watcher->snapshot.dirty=false;}
    auto s=foreign.poll();CHECK(s.terminal_error->code==ErrorCode::Invariant&&foreign.output->data.empty());
  }
  Setup threaded;std::thread wrong([&]{CHECK_ERROR(threaded.owner->poll(),ErrorCode::WrongThread);CHECK_ERROR(threaded.owner->request_stop(),ErrorCode::WrongThread);});wrong.join();
}
void coalescence(){
  Setup f({1,3,true});auto old=f.install();auto lease=f.owner->acquire(old);CHECK(lease);
  for(unsigned n=0;n<9;++n){f.change(a);auto s=f.poll();CHECK(s.outcome==imp::WatchOutcome::Unchanged&&s.unchanged==n+1&&s.attempts==1);}
  CHECK(f.owner->current().value()==old&&f.output->data.size()==2);
  f.owner.reset();CHECK(lease.value()->view().stamp().source==assets::digest(bytes(a))&&f.memory.snapshot().total.live>0);
}
void allocations(){
  std::size_t failures{};
  for(std::size_t n=0;n<80;++n){
    Setup f;auto old=f.install();f.change();
    desktop::profile::testing::fail_allocation_after(n);auto result=f.owner->poll();desktop::profile::testing::fail_next_allocation_attempts(0);
    CHECK(result);auto s=result.value();if(s.worker_retained)break;
    CHECK(f.owner->current().value()==old);CHECK(s.terminal_error||s.last_error);++failures;
  }
  CHECK(failures>5&&failures<80);
}
void admission(){
  for(bool live:{false,true}){
    auto source=std::make_shared<Source>();source->set(a);auto output=std::make_shared<FakeFiles>();
    auto memory=TrackedAllocator::create({64*1024*1024}).value();
    auto created=imp::Reimport::create(++Setup::identity,aid(1),output,memory);CHECK(created);auto owner=std::move(created).value();
    auto desired=owner->desire(assets::digest(bytes(a)));CHECK(desired);
    auto child=std::make_shared<Child>(++FactoryState::identity);
    CHECK(owner->start(desired.value(),std::make_unique<Process>(child),child->stage,"C:/worker.exe","C:/stage",bytes(a)));
    assets::Digest revision{};
    if(!live){child->complete();auto installed=owner->finish(desired.value());CHECK(installed);revision=owner->acquire(installed.value()).value()->view().revision();}
    auto watched=imp::Watch::create(source,"source.obj",std::make_unique<Watcher>(std::make_shared<WatchState>()),
      std::make_unique<Factory>(std::make_shared<FactoryState>()),std::move(owner),{1,3,true});
    if(live){CHECK(!watched&&watched.error().code==ErrorCode::Busy);CHECK(child->destroyed);}
    else{CHECK(watched);auto state=watched.value()->poll();CHECK(state&&state.value().outcome==imp::WatchOutcome::Unchanged&&state.value().current_revision==revision);}
  }
}
}
int main(){uv_coalescing();supersession();read_races_and_retries();attempts_and_failure();stopping_and_identity();coalescence();allocations();admission();std::puts("watched reimport admission passed");}
