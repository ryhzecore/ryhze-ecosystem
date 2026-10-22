// TEST-ONLY executable composition. A CPU uint64 fixture controls color, not a
// media importer. The separate Python software adapter owns all graphics handles.
#include "race/render/fixture.hpp"
#include "race/scene/scene.hpp"
#include "race/assets/assets.hpp"
#include "race/build_identity.hpp"
#include <cstdio>
#include <iostream>
#include <string>
#include <thread>
using namespace race;
using namespace std::chrono_literals;
namespace {
void require(bool value,const char *reason) { if (!value) { std::fprintf(stderr,"fixture failure: %s\n",reason);std::abort(); } }
template<class T> T need(Result<T> value) { if (!value) { std::fprintf(stderr,"fixture failure: %s\n",value.error().message);std::abort(); } return std::move(value).value(); }
void need(Result<void> value) { require(bool(value),"operation failed"); }
}
int main(int argc,char **argv) {
  if (argc!=4) { std::fprintf(stderr,"usage: race_render_fixture ROOT cpu|software base|changed\n");return 2; }
  const bool software=std::string_view(argv[2])=="software",changed=std::string_view(argv[3])=="changed";
  if ((!software && std::string_view(argv[2])!="cpu") || (!changed && std::string_view(argv[3])!="base")) return 2;
  auto platform=need(platform::make_services());
  auto files=need(platform::make_rooted_files(argv[1]));
  assets::Input input;input.id={30,1};input.seed=11;input.count=16;
  auto catalog=need(assets::cook(std::span(&input,1),*files,{}));
  auto allocator=need(TrackedAllocator::create({16*1024*1024}));
  auto scheduler=need(core::Scheduler::create(*platform.threads,{32,1,8}));
  auto service=need(assets::Service::create(catalog,{},allocator,files,*platform.threads,*scheduler));
  auto schemas=need(scene::Schemas::create({{2,1,"cpufixture",{{1,"asset",PersistentId{}}}}}));
  scene::Limits limits;limits.objects=4;limits.template_objects=4;limits.components=8;limits.history_groups=4;
  auto world=need(scene::World::create(allocator,schemas,limits));
  scene::Object object;object.id={20,1};object.components={scene::transform_component(),{2,1,{{1,input.id}}}};
  need(world->apply({{scene::Command::create(object)}}));
  if (changed) need(world->apply({{scene::Command::set(object.id,scene::transform_type,scene::translation_field,scene::Vec3{.15,0,0})}}));
  auto reads=service->stats().reads;
  render::SceneSnapshot snapshot;
  {
    auto view=need(world->read());auto handle=need(view.find(object.id));
    require(!view.find({999,1}),"missing object must fail");
    snapshot.world_scope={10,1};snapshot.object=object.id;snapshot.generation=view.generation();
    snapshot.resource=std::get<PersistentId>(need(view.value(handle,2,1)));
    snapshot.world_from_local=need(view.world_transform(handle)).m;
    auto origin=view.origin();snapshot.origin={origin.position.x,origin.position.y,origin.position.z};snapshot.origin_revision=origin.revision;
  }
  require(world->stats().readers==0,"render extraction must release read view");
  require(service->stats().reads==reads,"Scene lookup must not perform Assets IO");
  auto request=need(service->request(snapshot.resource));
  auto deadline=std::chrono::steady_clock::now()+10s;
  assets::Receipt receipt;
  do { need(service->drain());receipt=need(service->poll(request));
    require(std::chrono::steady_clock::now()<deadline,"Assets watchdog");std::this_thread::yield();
  } while(receipt.state==assets::RequestState::Pending);
  require(receipt.state==assets::RequestState::Ready,"asset fixture ready");
  auto retained=std::make_shared<assets::Lease>(need(service->acquire(receipt.resource)));
  const auto &lease=*retained;snapshot.resource_revision=lease->revision();
  require(lease->values().size()==16 && lease->values()[0]==assets::fixture_value(11,0),"checked uint64 CPU fixture");
  // Explicit test-only mapping: fixture value determines opaque triangle RGB.
  auto v=lease->values()[0];snapshot.color={float(64+(v&31))/255,float(128+((v>>8)&31))/255,float(208+((v>>16)&31))/255,1};
  std::weak_ptr<assets::Lease> weak=retained;
  const auto bytes=service->stats().payload_bytes;
  render::Flight flight;render::Capabilities caps;
  caps.execution=software?render::Execution::SoftwareGraphics:render::Execution::CpuContract;caps.completion_fence=software;
  auto ticket=need(flight.submit(snapshot,128,96,caps,{retained,bytes}));
  // All source services may stop while copied render input and lease remain live.
  need(service->release(request));need(service->retire(receipt.resource));
  require(!service->acquire(receipt.resource),"retired resource handle must fail");
  need(service->stop(10s));service.reset();retained.reset();
  need(world->apply({{scene::Command::destroy(object.id)}}));world.reset();
  need(scheduler->shutdown(core::ShutdownMode::Drain,10s));
  auto frame=need(flight.frame(ticket));
  require(!weak.expired(),"Flight must retain the only Lease wrapper");
  require((*weak.lock())->values()[0]==assets::fixture_value(11,0),"CPU lease survives facade teardown");
  auto timed=flight.wait(ticket,0ms);require(!timed && timed.error().code==ErrorCode::Timeout,"pending preserves owner");
  std::printf("{\"kind\":\"RACE test-only CPU Assets/Scene triangle\",\"source_commit\":\"%s\",\"authored_sha256\":\"%s\",\"world_scope\":[10,1],\"object\":[20,1],\"resource\":[30,1],\"resource_revision\":\"%s\",\"generation\":%llu,\"origin_revision\":%llu,\"retained_bytes\":%zu,\"positions\":[",build::source_commit,build::authored_input_sha256,assets::hex(snapshot.resource_revision).c_str(),(unsigned long long)snapshot.generation,(unsigned long long)snapshot.origin_revision,bytes);
  for (int i=0;i<3;++i) { const auto &p=frame.positions[i];std::printf("%s[%.9g,%.9g,%.9g,%.9g]",i?",":"",p[0],p[1],p[2],p[3]); }
  std::printf("],\"color\":[%.9g,%.9g,%.9g,%.9g]}\n",snapshot.color[0],snapshot.color[1],snapshot.color[2],snapshot.color[3]);std::fflush(stdout);
  render::Terminal outcome=render::Terminal::Complete;
  if (software) {
    // Trusted test protocol: orchestrator replies only AFTER real Vulkan fence
    // completion/readback/device quiescence. Unknown reply/EOF never releases it.
    std::array<char, 64> buffer{};
    std::cin.getline(buffer.data(),buffer.size());
    const std::string_view reply(buffer.data());
    if (!std::cin || (reply!="software-fence-complete" && reply!="software-quiescent-failure")) {
      std::fprintf(stderr,"No valid completion acknowledgement: holding ownership until process teardown\n");
      return 3; // Flight destructor deliberately waits; external watchdog owns process.
    }
    if(reply=="software-quiescent-failure") outcome=render::Terminal::Failed;
  }
  need(flight.acknowledge_quiescence(ticket,caps.execution,outcome));
  require(need(flight.take(ticket))==outcome,"terminal outcome");
  require(weak.expired(),"Flight releases final Lease after completion");
  require(allocator.snapshot().total.live==0,"tracked CPU fixture storage released");
  std::fprintf(stderr,"fixture: completion=%s, source services stopped, retained CPU fixture verified, snapshot copied\n",software?"software fence acknowledged":"CPU contract only");
  return outcome==render::Terminal::Complete?0:1;
}
