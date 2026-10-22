#include "mesh_import_job.hpp"
#include "assets_support.hpp"
#include "../apps/desktop_profile.hpp"

namespace proc=race::platform::process;
namespace m=race::assets::mesh;
namespace imp=race::mesh_import;
namespace {
constexpr std::string_view triangle="v 0 0 0\nv 1 0 0\nv 0 1 0\nf 1 2 3\n";
constexpr std::string_view mapped="v 0 0 0\nv 1 0 0\nv 0 1 0\nvt 0 0\nvt 1 0\nvt 0 1\nf 1/1 2/2 3/3\n";
auto bytes(std::string_view text){return std::as_bytes(std::span(text));}
struct BoundedFiles:FakeFiles {
  Result<std::vector<std::byte>> read(std::string_view leaf,std::size_t limit) override {
    if(!limit)return fail(ErrorCode::InvalidInput,"native zero read limit");
    return FakeFiles::read(leaf,limit);
  }
};
struct ProcessState {
  inline static std::uint64_t identity{};
  proc::Snapshot snapshot;
  ProcessState(){snapshot.request={++identity,1};}
  std::weak_ptr<platform::RootedFiles> stage;
  bool started{},stopped{},destroyed{},fail_start{};
  mesh_pipeline::Profile profile{mesh_pipeline::Profile::Geometry};
  void complete(std::optional<Error> error={}){
    snapshot.phase=proc::Phase::Completed;snapshot.exit_code=error?1:0;
    snapshot.stdout_finished=snapshot.stderr_finished=true;snapshot.failure=error;
  }
};
class FakeProcess final:public proc::Service {
  std::shared_ptr<ProcessState> state_;
public:
  explicit FakeProcess(std::shared_ptr<ProcessState> state):state_(std::move(state)){}
  ~FakeProcess(){CHECK(!state_->stage.expired());state_->complete();state_->destroyed=true;}
  Result<proc::Request> start(const proc::Launch& launch) override {
    CHECK(launch.arguments.size()==(state_->profile==mesh_pipeline::Profile::GeometryUv?3U:2U)&&launch.arguments[0]=="C:/stage"&&launch.arguments[1]==assets::id_text(aid(1)));
    if(state_->profile==mesh_pipeline::Profile::GeometryUv)CHECK(launch.arguments[2]=="geometry-uv");
    if(state_->fail_start)return fail(ErrorCode::PlatformFailure,"fake launch failure");
    state_->started=true;return state_->snapshot.request;
  }
  Result<proc::Snapshot> poll(proc::Request r) noexcept override {if(r!=state_->snapshot.request)return fail(ErrorCode::StaleHandle,"fake stale");return state_->snapshot;}
  Result<proc::Snapshot> wait(proc::Request r,std::uint32_t) noexcept override {
    auto value=poll(r);if(!value)return value.error();
    if(value.value().phase!=proc::Phase::Completed)return fail(ErrorCode::Timeout,"fake completion timeout");return value;
  }
  Result<void> request_stop(proc::Request) noexcept override {state_->stopped=true;return {};}
  Result<std::span<const std::byte>> stdout_prefix(proc::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<std::span<const std::byte>> stderr_prefix(proc::Request) const noexcept override{return std::span<const std::byte>{};}
  Result<proc::Completion> take(proc::Request) noexcept override{return fail(ErrorCode::Unsupported,"fake take not used");}
};
struct TestJob {
  std::shared_ptr<BoundedFiles> stage=std::make_shared<BoundedFiles>(),output=std::make_shared<BoundedFiles>();
  std::shared_ptr<ProcessState> process=std::make_shared<ProcessState>();
  std::unique_ptr<imp::Job> job;
  TestJob(){process->stage=stage;}
  Result<void> start(std::span<const std::byte> source=bytes(triangle),mesh_pipeline::Profile profile=mesh_pipeline::Profile::Geometry){
    process->profile=profile;
    auto result=imp::Job::start(std::make_unique<FakeProcess>(process),stage,output,"C:/worker.exe","C:/stage",aid(1),source,profile);
    if(!result)return result.error();job=std::move(result).value();return {};
  }
  void product(std::string_view source=triangle){stage->data["result.rasset"]=mesh_pipeline::import_obj(aid(1),bytes(source)).value();process->complete();}
};
void profiles(){
  using P=mesh_pipeline::Profile;
  for(auto profile:{P::Geometry,P::GeometryUv}){
    const auto source=profile==P::GeometryUv?mapped:triangle;
    TestJob success;CHECK(success.start(bytes(source),profile));success.product(source);
    auto published=success.job->publish();CHECK(published);
    CHECK(m::validate(success.output->data.at(published.value())).value().has_uv()==(profile==P::GeometryUv));
    // Valid native bytes with matching ID/stamp but the opposite schema reject.
    TestJob forged;CHECK(forged.start(bytes(source),profile));forged.product(source);
    auto view=m::validate(forged.stage->data.at("result.rasset"));CHECK(view);
    std::array<m::Vertex,3> vertices;for(std::size_t i=0;i<vertices.size();++i)vertices[i]=view.value().vertex(i).value();
    const std::array<std::uint32_t,3> indices{0,1,2};
    forged.stage->data["result.rasset"]=m::encode(aid(1),view.value().stamp(),vertices,indices,{},profile!=P::GeometryUv).value();
    CHECK_ERROR(forged.job->take_product(),ErrorCode::InvalidInput);CHECK(forged.output->data.empty());
    TestJob cancelled;CHECK(cancelled.start(bytes(source),profile));cancelled.product(source);CHECK(cancelled.job->cancel());
    CHECK_ERROR(cancelled.job->take_product(),ErrorCode::Cancelled);CHECK(cancelled.output->data.empty());
  }
  TestJob unknown;CHECK_ERROR(unknown.start(bytes(mapped),static_cast<P>(3)),ErrorCode::IncompatibleVersion);
  CHECK(!unknown.process->started&&unknown.stage->data.empty()&&unknown.output->data.empty());
  TestJob wrong;CHECK(wrong.start(bytes(mapped)));wrong.product(mapped);
  CHECK_ERROR(wrong.job->publish(),ErrorCode::InvalidInput);CHECK(wrong.output->data.empty());
}
void lifetime(){
  TestJob f;std::string source(triangle);CHECK(f.start(bytes(source)));source.assign(source.size(),'x');
  CHECK(f.stage->data["input.obj"]==std::vector<std::byte>(bytes(triangle).begin(),bytes(triangle).end()));
  CHECK_ERROR(f.job->wait(0),ErrorCode::Timeout);CHECK_ERROR(f.job->publish(),ErrorCode::Busy);
  CHECK(f.job->retained_source_bytes()==triangle.size()&&!f.process->destroyed&&f.output->data.empty());
  auto weak=std::weak_ptr<platform::RootedFiles>(f.stage);f.stage.reset();CHECK(!weak.expired());
  f.job.reset();CHECK(f.process->destroyed&&weak.expired());
  for(bool already_done:{false,true}){
    TestJob cancelled;CHECK(cancelled.start());cancelled.product();if(!already_done)cancelled.process->snapshot.phase=proc::Phase::Running;
    CHECK(cancelled.job->cancel());cancelled.process->complete();
    CHECK_ERROR(cancelled.job->publish(),ErrorCode::Cancelled);CHECK(cancelled.output->data.empty());
  }
}
void publication(){
  auto shared_output=std::make_shared<BoundedFiles>();std::vector<std::byte> first;
  for(unsigned i=0;i<2;++i){TestJob f;f.output=shared_output;CHECK(f.start());f.product();
    auto published=f.job->publish();CHECK(published);CHECK(shared_output->data.size()==1);
    const auto& product=shared_output->data.at(published.value());
    if(!i)first=product;else CHECK(product==first);
    CHECK_ERROR(f.job->publish(),ErrorCode::Busy);CHECK_ERROR(f.job->cancel(),ErrorCode::Busy);
  }
  for(unsigned variant=0;variant<5;++variant){TestJob f;CHECK(f.start());f.product();
    const auto product=f.stage->data.at("result.rasset");const auto name=assets::hex(assets::digest(product))+".rasset";
    f.output->data["prior.rasset"]={std::byte{7}};
    if(variant==0)f.output->fail_write=0;
    if(variant==1)f.output->throw_read=true;
    if(variant==2)f.output->data[name]={std::byte{8}};
    if(variant==3)f.stage->throw_read=true;
    if(variant==4)f.process->complete(fail(ErrorCode::Exhausted,"fixture output overflow"));
    CHECK(!f.job->publish());CHECK(f.output->data.at("prior.rasset")==std::vector<std::byte>{std::byte{7}});
    if(variant==1)CHECK(f.output->data.at(name)==product); // write succeeded, readback failed
    else if(variant==2)CHECK(f.output->data.at(name)==std::vector<std::byte>{std::byte{8}});
    else CHECK(!f.output->data.contains(name));
  }
}
void admission(){
  for(unsigned variant=0;variant<12;++variant){TestJob f;CHECK(f.start());f.product();
    auto product=f.stage->data.at("result.rasset");auto view=m::validate(product);CHECK(view);
    auto stamp=view.value().stamp();auto id=view.value().id();
    if(variant==0)id=aid(2);
    if(variant==1)stamp.source[0]^=std::byte{1};
    if(variant==2)stamp.settings[0]^=std::byte{1};
    if(variant==3)stamp.importer[0]^=std::byte{1};
    if(variant<4){std::array<m::Vertex,3> vertices;for(std::size_t i=0;i<3;++i)vertices[i]=view.value().vertex(i).value();
      const std::array<std::uint32_t,3> indices{0,1,2};product=m::encode(id,stamp,vertices,indices).value();}
    if(variant==4)product=mesh_pipeline::cook(product).value();
    if(variant==5)product.back()^=std::byte{1};
    if(variant==6)product.resize(15);
    if(variant==7)product.resize(m::Limits{}.product_bytes+1);
    f.stage->data["result.rasset"]=product;
    if(variant==8)f.stage->data.erase("result.rasset");
    if(variant==9)f.process->snapshot.exit_code=7;
    if(variant==10)f.process->snapshot.stdout_finished=false;
    if(variant==11)f.process->snapshot.exit_code.reset();
    CHECK(!f.job->publish());CHECK(f.output->data.empty());CHECK_ERROR(f.job->publish(),ErrorCode::Busy);
  }
  {TestJob f;f.process->fail_start=true;CHECK_ERROR(f.start(),ErrorCode::PlatformFailure);CHECK(f.process->destroyed&&f.output->data.empty());}
  {TestJob f;f.stage->fail_write=0;CHECK_ERROR(f.start(),ErrorCode::IoFailure);CHECK(!f.process->started&&f.process->destroyed);}
  for(auto leaf:{"input.obj","result.rasset"})for(std::size_t length:{0U,2U}){TestJob f;f.stage->data[leaf].resize(length);CHECK_ERROR(f.start(),ErrorCode::InvalidInput);CHECK(!f.process->started);}
  // Inject allocations across copied source, staging and process admission.
  std::size_t failed{};
  for(std::size_t step=0;step<128;++step){TestJob f;auto process=std::make_unique<FakeProcess>(f.process);
    desktop::profile::testing::fail_allocation_after(step);
    auto result=imp::Job::start(std::move(process),f.stage,f.output,"C:/worker.exe","C:/stage",aid(1),bytes(triangle));
    desktop::profile::testing::fail_next_allocation_attempts(0);
    if(result)break;
    CHECK(result.error().code==ErrorCode::Exhausted&&f.process->destroyed&&f.output->data.empty());++failed;
  }
  CHECK(failed>3&&failed<128);std::printf("mesh import admission allocation failure steps=%zu\n",failed);
}
}
int main(){lifetime();publication();admission();profiles();std::puts("trusted mesh import job ownership/admission passed");}
