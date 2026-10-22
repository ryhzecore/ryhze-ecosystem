#pragma once
#include "owned_records.hpp"

namespace owned_records_probe {
void run(std::byte* control,std::size_t capacity){
  using namespace qualification::owned;
  const auto before=crt_requests.load();
  require(capacity>=sizeof(Workspace)&&reinterpret_cast<std::uintptr_t>(control)%alignof(Workspace)==0,
          "owned workspace fits reserved control backing");
  auto* work=::new(static_cast<void*>(control)) Workspace;
  // These bytes deliberately exercise ownership only. They are not a validated
  // collision product or evidence of composition-level Assets admission.
  ProductBytes source;source.fill(std::byte{0x35});
  work->pending[0].product=source;source.fill(std::byte{0x72});
  work->bodies[0].product=work->pending[0].product;
  work->pending[0].product.fill(std::byte{});
  require(std::all_of(work->bodies[0].product.begin(),work->bodies[0].product.end(),
                     [](auto b){return b==std::byte{0x35};}),"body owns independent fixed product bytes");

  IdentityRecord old;old.adapter_id=0x01000003;old.occupied=true;
  old.body.identity={{1,2},{3,4},1};old.body.collision.fill(std::byte{0x55});old.body.configuration=7;
  require(work->identities.install(3,old),"initial identity installation");
  require(!work->identities.install(3,old),"occupied identity overwrite refused");
  require(!work->identities.install(4,old),"duplicate adapter identity refused");
  work->identities.begin_tick();
  require(work->identities.retire(3),"retire predecessor identity");
  auto current=old;current.body.identity.incarnation=2;current.body.configuration=8;
  current.body.collision.fill(std::byte{0x66});
  require(work->identities.install(3,current),"one slot reuse with wrapped adapter identity");
  require(work->identities.removed(old.adapter_id)&&work->identities.removed(old.adapter_id)->body==old.body,
          "removal resolves predecessor without a live SDK body");
  require(work->identities.removed(0xdeadbeef)==nullptr,"unknown removal has no invented identity");
  require(work->identities.retire(3)&&!work->identities.install(3,current),"second slot assignment in one tick refused");

  ResultStamp stamp;stamp.world={1,2};stamp.session={3,4};stamp.tick=8;stamp.input_generation=9;
  stamp.producer.fill(std::byte{1});stamp.products.fill(std::byte{2});stamp.configuration.fill(std::byte{3});
  stamp.origin={5,6};stamp.origin_revision=10;stamp.frame=11;stamp.origin_position_m={12,13,14};
  for(auto& query:work->queries){
    query.stamp=stamp;query.count=PrimitiveProfile::hits_per_query;
    for(std::size_t i=0;i<query.hits.size();++i){
      auto& hit=query.hits[i];hit.body=old.body;hit.point_m={double(i),2,3};hit.normal={0,1,0};
      hit.distance_m=double(i);hit.validity=static_cast<QueryValidity>(7);
    }
  }
  work->identities.begin_tick();
  require(work->identities.install(3,current),"next tick permits checked identity reuse");
  for(const auto& query:work->queries){
    require(query.stamp==stamp&&query.count==256,"all copied query stamps and hit counts retained");
    for(std::size_t i=0;i<query.hits.size();++i)
      require(query.hits[i].body==old.body&&query.hits[i].point_m[0]==double(i),"owned hits unaffected by identity retirement");
  }
  auto& phase=work->phase;phase.stamp=stamp;
  for(std::size_t i=0;i<phase.events.size();++i){
    ContactValue event;event.first=old.body;event.second=current.body;
    event.point_first_m={double(phase.events.size()-i),0,0};
    event.kind=EventKind::ContactPersist;event.validity=ContactValidity::Geometry;
    require(phase.append(event),"bounded contact capacity");
  }
  phase.order_events();
  for(std::size_t i=0;i<phase.events.size();++i)
    require(phase.events[i].point_first_m[0]==double(i+1),"deterministic in-place event ordering");
  require(!phase.append({})&&phase.failed&&phase.event_count==4096,"event overflow rejects candidate without overwrite");
  require(!phase.append({}),"capture failure is sticky");
  phase.event_count=0;phase.failed=false;
  ContactValue invalid;invalid.point_first_m[0]=std::numeric_limits<double>::quiet_NaN();
  require(!phase.append(invalid)&&phase.failed&&phase.event_count==0,"NaN rejected before event ordering");
  phase.failed=false;invalid={};invalid.validity=ContactValidity::Geometry;
  require(!phase.append(invalid)&&phase.failed,"exit geometry cannot be advertised as available");
  phase.failed=false;invalid={};
  require(phase.append(invalid),"explicitly unavailable exit geometry retained");
  require(crt_requests.load()==before&&sdk_arena->stats.live_bytes==0,"typed storage and ordering allocate no CRT or SDK memory");
  std::printf("{\"owned_records_passed\":true,\"body_bytes\":%zu,\"command_bytes\":%zu,\"identity_bytes\":%zu,\"snapshot_bytes\":%zu,\"pose_bytes\":%zu,\"event_bytes\":%zu,\"hit_bytes\":%zu,\"stamp_bytes\":%zu,\"phase_bytes\":%zu,\"query_bytes\":%zu,\"maps_bytes\":%zu,\"workspace_bytes\":%zu,\"workspace_alignment\":%zu,\"copied_hits\":16384,\"captured_events\":4096,\"control_blocks_qualified\":false,\"product_validation_qualified\":false,\"allocation_proved\":false}\n",
    sizeof(BodyRecord),sizeof(PendingCommand),sizeof(IdentityRecord),sizeof(QuerySnapshotValue),sizeof(PoseValue),sizeof(ContactValue),sizeof(QueryValue),sizeof(ResultStamp),sizeof(PhaseValues),sizeof(QueryValues),sizeof(RemovalIdentities),sizeof(Workspace),alignof(Workspace));
  work->~Workspace();
}
} // namespace owned_records_probe
