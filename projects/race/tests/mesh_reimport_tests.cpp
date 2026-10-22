#include "mesh_reimport.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"
#include <filesystem>
#include <fstream>

namespace proc=race::platform::process;
namespace m=race::assets::mesh;
namespace imp=race::mesh_import;
namespace {
constexpr std::string_view a="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view b="v 0 0 0\nv 2 0 0\nv 0 3 0\nf 1 2 3\n";
constexpr std::string_view uv="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
auto hash(std::string_view text){return assets::digest(bytes(text));}
struct ProcessState {
  inline static std::uint64_t identity{};
  proc::Request request{++identity,1};
  proc::Snapshot snapshot{request};
  std::weak_ptr<platform::RootedFiles> stage;
  bool started{},stopped{},destroyed{},fail_start{},fail_stop{};
  mesh_pipeline::Profile profile{mesh_pipeline::Profile::Geometry};
  void complete(){snapshot.phase=proc::Phase::Completed;snapshot.exit_code=0;snapshot.stdout_finished=snapshot.stderr_finished=true;}
};
class FakeProcess final:public proc::Service {
  std::shared_ptr<ProcessState> state_;
public:
  explicit FakeProcess(std::shared_ptr<ProcessState> state):state_(std::move(state)){}
  ~FakeProcess(){CHECK(!state_->stage.expired());state_->complete();state_->destroyed=true;}
  Result<proc::Request> start(const proc::Launch& launch) override {
    CHECK(launch.arguments.size()==(state_->profile==mesh_pipeline::Profile::GeometryUv?3U:2U));
    if(state_->profile==mesh_pipeline::Profile::GeometryUv)CHECK(launch.arguments[2]=="geometry-uv");
    if(state_->fail_start)return fail(ErrorCode::PlatformFailure,"fixture launch failure");
    state_->started=true;return state_->request;
  }
  Result<proc::Snapshot> poll(proc::Request r) noexcept override {CHECK(r==state_->request);return state_->snapshot;}
  Result<proc::Snapshot> wait(proc::Request r,std::uint32_t) noexcept override {
    auto result=poll(r);if(result.value().phase!=proc::Phase::Completed)return fail(ErrorCode::Timeout,"fixture live timeout");return result;
  }
  Result<void> request_stop(proc::Request) noexcept override {
    state_->stopped=true;if(state_->fail_stop)return fail(ErrorCode::PlatformFailure,"fixture stop failure");return {};
  }
  Result<std::span<const std::byte>> stdout_prefix(proc::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<std::span<const std::byte>> stderr_prefix(proc::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<proc::Completion> take(proc::Request) noexcept override{return fail(ErrorCode::Unsupported,"unused fixture take");}
};
struct Child {
  std::shared_ptr<FakeFiles> stage=std::make_shared<FakeFiles>();
  std::shared_ptr<ProcessState> process=std::make_shared<ProcessState>();
  Child(){process->stage=stage;}
  Result<void> start(imp::Reimport& owner,imp::Desired desired,std::string_view source=a){
    process->profile=owner.profile();
    return owner.start(desired,std::make_unique<FakeProcess>(process),stage,"C:/worker.exe","C:/stage",bytes(source));
  }
  void complete(std::string_view source=a){stage->data["result.rasset"]=mesh_pipeline::import_obj(aid(1),bytes(source)).value();process->complete();}
};
struct Fixture {
  inline static std::uint64_t identity{};
  std::shared_ptr<FakeFiles> output=std::make_shared<FakeFiles>();
  TrackedAllocator memory=TrackedAllocator::create({64*1024*1024}).value();
  std::unique_ptr<imp::Reimport> owner;
  explicit Fixture(imp::ReimportLimits limits={},mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){
    auto created=imp::Reimport::create(++identity,aid(1),output,memory,limits,profile);CHECK(created);owner=std::move(created).value();
  }
  imp::Desired desire(std::string_view source=a){auto result=owner->desire(hash(source));CHECK(result);return result.value();}
  m::ResourceHandle install(std::string_view source=a){auto desired=desire(source);Child child;CHECK(child.start(*owner,desired,source));child.complete(source);auto result=owner->finish(desired);CHECK(result&&child.process->destroyed);return result.value();}
};
void profiles(){
  using P=mesh_pipeline::Profile;
  for(auto profile:{P::Geometry,P::GeometryUv}){
    const auto text=profile==P::GeometryUv?uv:a;
    const auto product=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(1),bytes(text),profile).value()).value();
    Fixture restored({},profile);CHECK(restored.owner->profile()==profile);
    auto view=m::validate(product);CHECK(view);std::array<m::Vertex,3> vertices;
    for(std::size_t i=0;i<vertices.size();++i)vertices[i]=view.value().vertex(i).value();
    const std::array<std::uint32_t,3> indices{0,1,2};
    const auto forged=m::encode(aid(1),view.value().stamp(),vertices,indices,{},profile!=P::GeometryUv).value();
    CHECK_ERROR(restored.owner->restore_initial(forged),ErrorCode::InvalidInput);CHECK_ERROR(restored.owner->current(),ErrorCode::NotFound);
    CHECK(restored.owner->restore_initial(product));auto held=restored.owner->acquire(restored.owner->current().value());CHECK(held);
    const auto original=held.value()->view().revision();
    Fixture live({},profile);const auto current=live.install(text);const auto prior=live.output->data;
    Child child;const auto desired=live.desire(text);CHECK(child.start(*live.owner,desired,text));child.complete(text);
    const auto imported_view=m::validate(child.stage->data.at("result.rasset"));CHECK(imported_view);
    child.stage->data["result.rasset"]=m::encode(aid(1),imported_view.value().stamp(),vertices,indices,{},profile!=P::GeometryUv).value();
    CHECK_ERROR(live.owner->finish(desired),ErrorCode::InvalidInput);CHECK(child.process->destroyed&&live.owner->current().value()==current&&live.output->data==prior);
    Child cancelled;const auto token=live.desire(text);CHECK(cancelled.start(*live.owner,token,text));CHECK(live.owner->invalidate());
    cancelled.complete(text);CHECK(live.owner->reap());CHECK(cancelled.process->destroyed&&live.owner->current().value()==current);
    restored.owner.reset();CHECK(held.value()->view().revision()==original);
  }
  Fixture legacy;auto uv_cooked=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(1),bytes(uv),P::GeometryUv).value()).value();
  CHECK_ERROR(legacy.owner->restore_initial(uv_cooked),ErrorCode::InvalidInput);
  CHECK_ERROR(imp::Reimport::create(1,aid(1),legacy.output,legacy.memory,{},static_cast<P>(3)),ErrorCode::IncompatibleVersion);
  std::size_t failures{};
  for(std::size_t step=0;step<64;++step){Fixture empty({},P::GeometryUv);desktop::profile::testing::fail_allocation_after(step);
    auto result=empty.owner->restore_initial(uv_cooked);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;CHECK(result.error().code==ErrorCode::Exhausted);CHECK_ERROR(empty.owner->current(),ErrorCode::NotFound);
    CHECK(empty.owner->restore_initial(uv_cooked));++failures;}
  CHECK(failures>0&&failures<64);
}
void supersession(){
  for(bool completed:{false,true}){
    Fixture f;const auto old=f.install();auto lease=f.owner->acquire(old);CHECK(lease);const auto revision=lease.value()->view().revision();
    Child child;auto first=f.desire();CHECK(child.start(*f.owner,first));if(completed)child.complete();
    auto second=f.desire(b);auto third=f.desire();CHECK(third.generation>second.generation&&second.generation>first.generation);
    CHECK(child.process->stopped&&!child.process->destroyed);CHECK_ERROR(f.owner->finish(first),ErrorCode::StaleHandle);
    Child refused;CHECK_ERROR(refused.start(*f.owner,third),ErrorCode::Busy);CHECK(!refused.process->started);
    if(!completed){CHECK_ERROR(f.owner->wait(0),ErrorCode::Timeout);CHECK_ERROR(f.owner->reap(),ErrorCode::Busy);}
    CHECK(f.owner->pending_source_bytes().value()==a.size());auto weak=std::weak_ptr<platform::RootedFiles>(child.stage);
    child.complete();child.stage.reset();CHECK(!weak.expired());CHECK(f.owner->reap());CHECK(weak.expired()&&child.process->destroyed);
    CHECK(f.owner->current().value()==old&&f.owner->pending_source_bytes().value()==0);
    Child replacement;CHECK(replacement.start(*f.owner,third));replacement.complete();CHECK(f.owner->finish(third));
    CHECK(lease.value()->view().revision()==revision);
  }
}
void wrong_identity_and_thread(){
  Fixture f;auto desired=f.desire();
  {Child child;auto wrong=desired;++wrong.owner;CHECK_ERROR(child.start(*f.owner,wrong),ErrorCode::StaleHandle);CHECK(!child.process->started);}
  {Child child;CHECK_ERROR(child.start(*f.owner,desired,b),ErrorCode::InvalidInput);CHECK(!child.process->started);}
  Child child;CHECK(child.start(*f.owner,desired));child.complete();
  for(bool service:{false,true}){
    child.process->snapshot.request=child.process->request;
    if(service)++child.process->snapshot.request.service;else ++child.process->snapshot.request.serial;
    CHECK_ERROR(f.owner->poll(),ErrorCode::Invariant);CHECK_ERROR(f.owner->wait(0),ErrorCode::Invariant);
    CHECK_ERROR(f.owner->finish(desired),ErrorCode::Invariant);CHECK_ERROR(f.owner->reap(),ErrorCode::Invariant);
    CHECK(!child.process->destroyed&&f.output->data.empty());
  }
  child.process->snapshot.request=child.process->request;child.process->snapshot.stderr_finished=false;
  CHECK_ERROR(f.owner->reap(),ErrorCode::Busy);CHECK_ERROR(f.owner->finish(desired),ErrorCode::Busy);CHECK(!child.process->destroyed);
  child.process->snapshot.stderr_finished=true;
  std::thread foreign([&]{
    CHECK_ERROR(f.owner->invalidate(),ErrorCode::WrongThread);
    CHECK_ERROR(f.owner->desire(hash(b)),ErrorCode::WrongThread);CHECK_ERROR(f.owner->poll(),ErrorCode::WrongThread);
    CHECK_ERROR(f.owner->wait(0),ErrorCode::WrongThread);CHECK_ERROR(f.owner->finish(desired),ErrorCode::WrongThread);
    CHECK_ERROR(f.owner->reap(),ErrorCode::WrongThread);CHECK_ERROR(f.owner->current(),ErrorCode::WrongThread);
    CHECK_ERROR(f.owner->pending_source_bytes(),ErrorCode::WrongThread);
  });foreign.join();CHECK(f.owner->finish(desired));
}
void preservation(){
  for(unsigned variant=0;variant<8;++variant){
    Fixture f;auto old=f.install();auto lease=f.owner->acquire(old);CHECK(lease);const auto expected=lease.value()->view().revision();
    Child child;auto desired=f.desire(b);CHECK(child.start(*f.owner,desired,b));child.complete(b);
    if(variant==0)child.process->snapshot.exit_code=7;
    if(variant==1)child.stage->data["result.rasset"].back()^=std::byte{1};
    if(variant==2)child.stage->data["result.rasset"]=mesh_pipeline::import_obj(aid(1),bytes(a)).value();
    if(variant==3)f.output->fail_write=f.output->writes;
    if(variant==4)f.output->throw_read=true;
    if(variant==5)f.output->fail_write=f.output->writes+1;
    if(variant==6){child.process->snapshot.failure=fail(ErrorCode::Exhausted,"fixture output overflow");}
    if(variant==7)child.stage->throw_read=true;
    CHECK(!f.owner->finish(desired));CHECK(f.owner->current().value()==old);
    CHECK(lease.value()->view().revision()==expected&&child.process->destroyed&&f.owner->pending_source_bytes().value()==0);
    f.output->throw_read=false;f.output->fail_write=SIZE_MAX;CHECK(f.install(b).table);
  }
  {Fixture f;auto old=f.install();Child child;auto desired=f.desire(b);CHECK(child.start(*f.owner,desired,b));child.process->fail_stop=true;
    CHECK_ERROR(f.owner->desire(hash(a)),ErrorCode::PlatformFailure);child.complete(b);
    CHECK_ERROR(f.owner->finish(desired),ErrorCode::StaleHandle);CHECK(f.owner->reap());CHECK(f.owner->current().value()==old);CHECK(f.install(b).table);}
}
void retention_and_exhaustion(){
  const auto product=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(1),bytes(a)).value()).value();
  imp::ReimportLimits limits;limits.residency.retained_revisions=2;limits.residency.retained_bytes=product.size()*2;
  Fixture f(limits);auto first=f.install();auto lease=f.owner->acquire(first);CHECK(lease);
  auto second=f.install(b);CHECK(f.owner->residency().retained_revisions==2&&f.owner->residency().retained_bytes==product.size()*2);
  CHECK_ERROR(f.owner->acquire(first),ErrorCode::StaleHandle);auto held=std::move(lease).value();
  Child third;auto desired=f.desire();CHECK(third.start(*f.owner,desired));third.complete();CHECK_ERROR(f.owner->finish(desired),ErrorCode::Exhausted);
  CHECK(f.owner->current().value()==second&&f.output->data.size()==4);
  held=m::Lease{nullptr};CHECK(f.owner->residency().retained_revisions==1);auto last=f.install();
  auto survivor=f.owner->acquire(last);CHECK(survivor);auto owned=std::move(survivor).value();f.owner.reset();
  CHECK(f.memory.snapshot().total.live==product.size());CHECK(owned->view().bytes().size()==product.size());owned=m::Lease{nullptr};CHECK(f.memory.snapshot().total.live==0);
  limits.generations=2;Fixture exhausted(limits);auto old=exhausted.install();Child live;auto final=exhausted.desire(b);CHECK(live.start(*exhausted.owner,final,b));
  CHECK_ERROR(exhausted.owner->desire(hash(a)),ErrorCode::Exhausted);CHECK(live.process->stopped&&!live.process->destroyed);
  CHECK_ERROR(exhausted.owner->finish(final),ErrorCode::StaleHandle);live.complete(b);CHECK(exhausted.owner->reap());
  CHECK_ERROR(exhausted.owner->desire(hash(b)),ErrorCode::Exhausted);CHECK(exhausted.owner->current().value()==old);
  {Fixture owned_fixture;Child active;auto d=owned_fixture.desire();CHECK(active.start(*owned_fixture.owner,d));
    active.stage.reset();owned_fixture.owner.reset();CHECK(active.process->destroyed&&active.process->stage.expired());}
}
void allocation_failures(){
  std::size_t failures{};
  for(std::size_t step=0;step<128;++step){
    Fixture f;auto old=f.install();Child child;auto desired=f.desire(b);CHECK(child.start(*f.owner,desired,b));child.complete(b);
    desktop::profile::testing::fail_allocation_after(step);auto result=f.owner->finish(desired);
    desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;
    CHECK(result.error().code==ErrorCode::Exhausted&&f.owner->current().value()==old&&child.process->destroyed);
    CHECK(f.owner->residency().retained_revisions==1);++failures;
  }
  CHECK(failures>10&&failures<128);std::printf("reimport publication allocation failure steps=%zu\n",failures);
}
void admission_failure(){
  Fixture f;auto old=f.install();Child child;auto desired=f.desire(b);child.process->fail_start=true;
  CHECK_ERROR(child.start(*f.owner,desired,b),ErrorCode::PlatformFailure);
  CHECK(child.process->destroyed&&!child.process->started&&f.owner->pending_source_bytes().value()==0);
  CHECK(f.owner->current().value()==old);
  Child retry;CHECK_ERROR(retry.start(*f.owner,desired,b),ErrorCode::Busy);CHECK(!retry.process->started);
  auto next=f.desire(b);Child valid;CHECK(valid.start(*f.owner,next,b));valid.complete(b);CHECK(f.owner->finish(next));
  Child again;CHECK_ERROR(again.start(*f.owner,next,b),ErrorCode::Busy);CHECK(!again.process->started);
  std::size_t failed{};
  for(std::size_t step=0;step<64;++step){
    Fixture sample;auto prior=sample.install();auto target=sample.desire(b);Child launch;auto process=std::make_unique<FakeProcess>(launch.process);
    desktop::profile::testing::fail_allocation_after(step);
    auto result=sample.owner->start(target,std::move(process),launch.stage,"C:/worker.exe","C:/stage",bytes(b));
    desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;
    CHECK(result.error().code==ErrorCode::Exhausted&&sample.owner->current().value()==prior);
    CHECK(launch.process->destroyed&&sample.owner->pending_source_bytes().value()==0);++failed;
  }
  CHECK(failed>3&&failed<64);std::printf("reimport start allocation failure steps=%zu\n",failed);
}
void initial_restoration(){
  const auto imported=mesh_pipeline::import_obj(aid(1),bytes(a)).value();const auto cooked=mesh_pipeline::cook(imported).value();
  Fixture f;std::thread foreign([&]{CHECK_ERROR(f.owner->restore_initial(cooked),ErrorCode::WrongThread);});foreign.join();
  CHECK_ERROR(f.owner->restore_initial(imported),ErrorCode::InvalidInput);CHECK_ERROR(f.owner->current(),ErrorCode::NotFound);
  auto corrupt=cooked;corrupt.back()^=std::byte{1};CHECK(!f.owner->restore_initial(corrupt));
  auto other=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(2),bytes(a)).value()).value();CHECK(!f.owner->restore_initial(other));
  auto input=cooked;auto restored=f.owner->restore_initial(input);CHECK(restored&&f.owner->current().value()==restored.value());
  auto lease=f.owner->acquire(restored.value());CHECK(lease);auto held=std::move(lease).value();std::fill(input.begin(),input.end(),std::byte{});
  CHECK(std::equal(held->view().bytes().begin(),held->view().bytes().end(),cooked.begin(),cooked.end())&&f.output->data.empty());
  CHECK_ERROR(f.owner->restore_initial(cooked),ErrorCode::Busy);const auto old=held->view().revision();CHECK(f.install(b).table);CHECK(held->view().revision()==old);
  f.owner.reset();CHECK(f.memory.snapshot().total.live==cooked.size());held=m::Lease{nullptr};CHECK(f.memory.snapshot().total.live==0);
  Fixture active;auto desired=active.desire();Child child;CHECK(child.start(*active.owner,desired));
  CHECK_ERROR(active.owner->restore_initial(cooked),ErrorCode::Busy);CHECK(!child.process->stopped&&!child.process->destroyed&&active.owner->pending_source_bytes().value()==a.size());
  child.complete();CHECK(active.owner->reap());CHECK_ERROR(active.owner->restore_initial(cooked),ErrorCode::Busy);
  std::size_t failures{};
  for(std::size_t step=0;step<64;++step){
    Fixture empty;desktop::profile::testing::fail_allocation_after(step);auto result=empty.owner->restore_initial(cooked);desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;
    CHECK(result.error().code==ErrorCode::Exhausted);CHECK_ERROR(empty.owner->current(),ErrorCode::NotFound);
    CHECK(empty.owner->residency().retained_bytes==0&&empty.output->data.empty());CHECK(empty.owner->restore_initial(cooked));++failures;
  }
  CHECK(failures>0&&failures<64);
}
#ifdef _WIN32
void native(const char* worker,const char* root){
  namespace fs=std::filesystem;using Clock=std::chrono::steady_clock;
  CHECK(fs::create_directory(root));const auto base=fs::absolute(root);CHECK(fs::create_directory(base/"output"));
  auto output=platform::make_rooted_files((base/"output").string());CHECK(output);
  auto memory=TrackedAllocator::create({64*1024*1024}).value();auto made=imp::Reimport::create(1,aid(1),output.value(),memory);CHECK(made);auto owner=std::move(made).value();
  std::string dense="v 0 0 0\nv 1 0 0\nv 0 1 0\n";for(unsigned n=0;n<12000;++n)dense+="f 1 2 3\n";
  std::ofstream receipt(base/"receipt.json");CHECK(receipt);receipt<<"{\"warmups\":2,\"trials\":9,\"samples\":[";
  std::optional<m::Lease> old;assets::Digest old_revision{};
  for(unsigned trial=0;trial<22;++trial){
    auto source=(trial%2!=0)?std::string_view(dense):b;auto stage_path=base/("stage-"+std::to_string(trial));CHECK(fs::create_directory(stage_path));
    auto stage=platform::make_rooted_files(stage_path.string());CHECK(stage);auto service=proc::make_windows_service(trial+1);CHECK(service);
    auto expected=mesh_pipeline::cook(mesh_pipeline::import_obj(aid(1),bytes(source)).value()).value();
    auto desired=owner->desire(hash(source));CHECK(desired);auto start=Clock::now();
    CHECK(owner->start(desired.value(),std::move(service).value(),stage.value(),worker,stage_path.string(),bytes(source)));auto admitted=Clock::now();
    CHECK(owner->pending_source_bytes().value()==source.size());CHECK(owner->wait(60000));auto completed=Clock::now();
    auto handle=owner->finish(desired.value());CHECK(handle);auto end=Clock::now();
    auto lease=owner->acquire(handle.value());CHECK(lease);const auto actual=lease.value()->view().bytes();CHECK(std::equal(actual.begin(),actual.end(),expected.begin(),expected.end()));
    CHECK(owner->pending_source_bytes().value()==0);
    const auto leaf=assets::hex(assets::digest(expected))+".rasset";CHECK(output.value()->read(leaf,m::Limits{}.product_bytes).value()==expected);
    if(!old){old.emplace(std::move(lease).value());old_revision=(*old)->view().revision();}else CHECK((*old)->view().revision()==old_revision);
    const auto ms=[](auto d){return std::chrono::duration<double,std::milli>(d).count();};
    if(trial>=4){if(trial>4)receipt<<',';receipt<<"{\"source_bytes\":"<<source.size()<<",\"admission_ms\":"<<ms(admitted-start)<<",\"worker_ms\":"<<ms(completed-admitted)<<",\"cook_publish_ms\":"<<ms(end-completed)<<",\"retained_bytes\":"<<owner->residency().retained_bytes<<",\"sha256\":\""<<assets::hex(assets::digest(expected))<<"\"}";}
  }
  owner.reset();CHECK(old&&(*old)->view().revision()==old_revision&&memory.snapshot().total.live>0);old.reset();CHECK(memory.snapshot().total.live==0);
  receipt<<"],\"byte_parity\":true,\"old_lease_survived\":true,\"final_retained_bytes\":0}";CHECK(receipt);
}
#endif
}
int main(int argc,char** argv){
#ifdef _WIN32
  if(argc==4&&std::string_view(argv[1])=="--native"){native(argv[2],argv[3]);return 0;}
#else
  (void)argc;(void)argv;
#endif
  supersession();wrong_identity_and_thread();preservation();retention_and_exhaustion();allocation_failures();admission_failure();initial_restoration();profiles();
  std::puts("live reimport admission passed");
}
