#include "race/render/fixture.hpp"
#include "test.hpp"
#include <atomic>
#include <limits>
#include <semaphore>
#include <thread>
using namespace race;
using namespace race::render;
using namespace std::chrono_literals;
namespace {
SceneSnapshot snapshot() {
  SceneSnapshot s;s.world_scope={1,1};s.object={2,1};s.resource={3,1};
  s.generation=1;s.origin_revision=1;s.resource_revision[0]=std::byte{1};return s;
}
void inputs() {
  auto s=snapshot();auto f=prepare(s,128,96,{});CHECK(f);
  CHECK(f.value().positions[0][1]==.7f);
  s.world_from_local[3]=.1;auto moved=prepare(s,64,64,{});CHECK(moved);
  CHECK(moved.value().positions[0][0]==-.6f);
  s.origin[0]=.1;CHECK(prepare(s,64,64,{}).value().positions==f.value().positions);
  for (auto size:{0u,15u,513u,std::numeric_limits<unsigned>::max()}) {
    CHECK_ERROR(prepare(snapshot(),size,32,{}),ErrorCode::Exhausted);
    CHECK_ERROR(prepare(snapshot(),32,size,{}),ErrorCode::Exhausted);
  }
  Capabilities noFence{Execution::SoftwareGraphics};
  CHECK_ERROR(prepare(snapshot(),32,32,noFence),ErrorCode::Unsupported);
  noFence.completion_fence=true;CHECK(prepare(snapshot(),512,512,noFence));
  noFence.max_width=31;CHECK_ERROR(prepare(snapshot(),32,32,noFence),ErrorCode::Exhausted);
  s=snapshot();s.world_scope={};CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::InvalidInput);
  s=snapshot();s.resource_revision={};CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::InvalidInput);
  s=snapshot();s.color[0]=std::numeric_limits<float>::quiet_NaN();CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::InvalidInput);
  s=snapshot();s.color[3]=.5f;CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::Unsupported);
  s=snapshot();s.world_from_local[0]=std::numeric_limits<double>::infinity();CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::InvalidInput);
  s=snapshot();s.vertices[2]=s.vertices[0];CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::InvalidInput);
  s=snapshot();s.world_from_local[11]=.1;CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::Unsupported);
  s=snapshot();s.world_from_local[3]=2;CHECK_ERROR(prepare(s,32,32,{}),ErrorCode::Unsupported);
  // Same object ID is legal in distinct caller-supplied World scopes.
  auto other=snapshot();other.world_scope={1,2};CHECK(prepare(other,32,32,{}));
  CHECK(other.world_scope!=snapshot().world_scope);
}
void lifetime() {
  Flight queue,foreign;auto owner=std::make_shared<int>(42);std::weak_ptr<int> weak=owner;
  CHECK_ERROR(queue.submit(snapshot(),32,32,{},{}),ErrorCode::InvalidInput);
  CHECK_ERROR(queue.submit(snapshot(),32,32,{}, {owner,1024*1024+1}),ErrorCode::Exhausted);
  auto ticket=queue.submit(snapshot(),32,32,{}, {owner,sizeof(int)});CHECK(ticket);owner.reset();
  CHECK_ERROR(queue.wait(ticket.value(),-1ms),ErrorCode::InvalidInput);
  CHECK_ERROR(queue.wait(ticket.value(),0ms),ErrorCode::Timeout);CHECK(!weak.expired());
  CHECK_ERROR(queue.take(ticket.value()),ErrorCode::Busy);
  CHECK_ERROR(foreign.frame(ticket.value()),ErrorCode::StaleHandle);
  auto other=std::make_shared<int>(3);
  CHECK_ERROR(queue.submit(snapshot(),32,32,{}, {other,4}),ErrorCode::Busy);
  CHECK_ERROR(queue.acknowledge_quiescence(ticket.value(),Execution::SoftwareGraphics,Terminal::Complete),ErrorCode::InvalidInput);
  CHECK(!weak.expired());CHECK(queue.frame(ticket.value()).value().scene.object==PersistentId{2,1});
  // Deterministic completion gate: no tight wall-time or sleeping assertion.
  std::binary_semaphore ready(0),release(0);
  std::thread backend([&]{ready.release();release.acquire();CHECK(queue.acknowledge_quiescence(ticket.value(),Execution::CpuContract,Terminal::Complete));});
  ready.acquire();CHECK(queue.cancel(ticket.value()));
  CHECK_ERROR(queue.wait(ticket.value(),0ms),ErrorCode::Timeout);CHECK(!weak.expired());
  release.release();backend.join();
  CHECK(queue.wait(ticket.value(),std::chrono::milliseconds::max()).value()==Terminal::Cancelled);
  CHECK_ERROR(queue.acknowledge_quiescence(ticket.value(),Execution::CpuContract,Terminal::Complete),ErrorCode::Busy);
  CHECK(!weak.expired());CHECK(queue.take(ticket.value()).value()==Terminal::Cancelled);CHECK(weak.expired());
  CHECK_ERROR(queue.frame(ticket.value()),ErrorCode::StaleHandle);
  auto next=queue.submit(snapshot(),64,64,{}, {other,4});CHECK(next);CHECK(next.value()!=ticket.value());
  CHECK_ERROR(queue.cancel(ticket.value()),ErrorCode::StaleHandle);
  CHECK(queue.cancel(next.value()));CHECK(queue.acknowledge_quiescence(next.value(),Execution::CpuContract,Terminal::Failed));
  CHECK(queue.take(next.value()).value()==Terminal::Failed);
  for(int i=0;i<1000;++i) {
    auto t=queue.submit(snapshot(),32,32,{}, {other,4});CHECK(t);
    CHECK(queue.acknowledge_quiescence(t.value(),Execution::CpuContract,Terminal::Complete));
    CHECK(queue.take(t.value()).value()==Terminal::Complete);
  }
}
void immutable() {
  Flight queue;auto s=snapshot();auto owner=std::make_shared<int>(1);
  auto t=queue.submit(s,128,96,{}, {owner,4});CHECK(t);
  s.object={9,9};s.vertices[0][0]=.2;
  auto copy=queue.frame(t.value());CHECK(copy);CHECK(copy.value().scene.object==PersistentId{2,1});
  copy.value().scene.color[0]=1;
  CHECK(queue.frame(t.value()).value().scene.color[0]==.25f);
  CHECK(queue.acknowledge_quiescence(t.value(),Execution::CpuContract,Terminal::Complete));
  CHECK(queue.take(t.value()));
}
}
void viewport_geometry_tests();
int main() { viewport_geometry_tests();inputs();lifetime();immutable();std::puts("render CPU contracts: passed (not graphics execution)"); }
