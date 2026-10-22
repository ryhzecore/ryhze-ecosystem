#pragma once
#include <Jolt/Physics/Collision/ContactListener.h>
#include "owned_records.hpp"

// Private single-thread primitive-profile qualification. No callbacks publish
// events or call application code. Sensor callbacks are deliberately suppressed:
// their sleep/removal semantics cannot determine geometric trigger continuity.
namespace contact_owner_probe {
using namespace qualification::owned;
struct State {PhaseValues phase;RemovalIdentities identities;};
class Capture final:public JPH::ContactListener {
  PhaseValues& phase_;
  const RemovalIdentities& identities_;
  static auto key(const BodyIdentity& id) noexcept {
    return std::array{id.section.high,id.section.low,id.object.high,id.object.low,id.incarnation};
  }
  static void canonicalize(ContactValue& value) noexcept {
    if(key(value.first.identity)>key(value.second.identity)){
      std::swap(value.first,value.second);std::swap(value.point_first_m,value.point_second_m);
      for(auto& x:value.normal_first_to_second)x=-x;
      for(auto& x:value.relative_pre_solve_velocity_m_s)x=-x;
    }
  }
  template<class V>static Triple copy(V v) noexcept {return {double(v.GetX()),double(v.GetY()),double(v.GetZ())};}
  void contact(const JPH::Body& first,const JPH::Body& second,const JPH::ContactManifold& manifold,
               const JPH::ContactSettings& settings,EventKind kind) noexcept {
    if(phase_.failed)return;
    const auto* a=identities_.current(first.GetID().GetIndexAndSequenceNumber());
    const auto* b=identities_.current(second.GetID().GetIndexAndSequenceNumber());
    if(!a||!b){phase_.failed=true;return;}
    if(a->sensor||b->sensor){++sensor_contacts;return;}
    if(settings.mIsSensor||manifold.mRelativeContactPointsOn1.size()!=manifold.mRelativeContactPointsOn2.size()||
       manifold.mRelativeContactPointsOn1.empty()){phase_.failed=true;return;}
    for(JPH::uint i=0;i<manifold.mRelativeContactPointsOn1.size();++i){
      ContactValue value{};value.first=a->body;value.second=b->body;value.kind=kind;
      const auto p=manifold.GetWorldSpaceContactPointOn1(i),q=manifold.GetWorldSpaceContactPointOn2(i);
      value.point_first_m=copy(p);value.point_second_m=copy(q);value.normal_first_to_second=copy(manifold.mWorldSpaceNormal);
      value.penetration_m=manifold.mPenetrationDepth;
      value.relative_pre_solve_velocity_m_s=copy(second.GetPointVelocity(q)-first.GetPointVelocity(p));
      value.friction=settings.mCombinedFriction;value.restitution=settings.mCombinedRestitution;
      value.validity=static_cast<ContactValidity>(7);canonicalize(value);
      if(!phase_.append(value))return;
    }
  }
public:
  unsigned added{},persisted{},removed{},sensor_contacts{},sensor_removals{};
  Capture(PhaseValues& phase,const RemovalIdentities& identities) noexcept:phase_(phase),identities_(identities){}
  void OnContactAdded(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold& m,JPH::ContactSettings& s) noexcept override {
    ++added;contact(a,b,m,s,EventKind::ContactEnter);
  }
  void OnContactPersisted(const JPH::Body& a,const JPH::Body& b,const JPH::ContactManifold& m,JPH::ContactSettings& s) noexcept override {
    ++persisted;contact(a,b,m,s,EventKind::ContactPersist);
  }
  void OnContactRemoved(const JPH::SubShapeIDPair& pair) noexcept override {
    ++removed;if(phase_.failed)return;
    // Never inspect a Body here: either owner may already have been destroyed.
    const auto* a=identities_.removed(pair.GetBody1ID().GetIndexAndSequenceNumber());
    const auto* b=identities_.removed(pair.GetBody2ID().GetIndexAndSequenceNumber());
    if(!a||!b){phase_.failed=true;return;}
    if(a->sensor||b->sensor){++sensor_removals;return;}
    ContactValue value{};value.first=a->body;value.second=b->body;value.kind=EventKind::ContactExit;
    canonicalize(value);phase_.append(value);
  }
};
struct Observations {unsigned added{},persisted{},removed{},sensor_removals{};bool overflow{};};
void exercise(State& state,Scratch& scratch,bool sensor,bool overflow,Observations& observation){
  state.phase.event_count=0;state.phase.failed=false;
  // Reinitialize in the reserved region instead of forming a 45 KB assignment
  // temporary in the owner frame (present even in the optimized native build).
  std::destroy_at(&state.identities);std::construct_at(&state.identities);
  JPH::JobSystemSingleThreaded jobs(race::physics::PrimitiveProfile::jobs);prewarm_jobs(jobs);
  Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
  JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);world.SetGravity(JPH::Vec3::sZero());
  Capture capture(state.phase,state.identities);world.SetContactListener(&capture);auto& api=world.GetBodyInterface();
  std::array<JPH::BodyID,3> ids;std::array<JPH::RefConst<JPH::Shape>,3> shapes;
  for(unsigned i=0;i<3;++i){
    shapes[i]=i==0?static_cast<JPH::Shape*>(new JPH::BoxShape(JPH::Vec3::sReplicate(.5f),.005f)):
                       static_cast<JPH::Shape*>(new JPH::SphereShape(.5f));
    JPH::BodyCreationSettings creation(shapes[i],JPH::RVec3(i==2?20.:0.,i==1?.9:0.,0.),JPH::Quat::sIdentity(),
        i==0?JPH::EMotionType::Static:JPH::EMotionType::Dynamic,i==0?0:1);
    creation.mIsSensor=sensor&&i==0;creation.mAllowSleeping=false;creation.mGravityFactor=0;
    ids[i]=api.CreateAndAddBody(creation,i==0?JPH::EActivation::DontActivate:JPH::EActivation::Activate);
    require(!ids[i].IsInvalid(),"callback body creation");
    IdentityRecord record{};record.occupied=true;record.sensor=creation.mIsSensor;record.adapter_id=ids[i].GetIndexAndSequenceNumber();
    record.body.identity={{1,1},{1,3-i},1};record.body.configuration=10+i;record.body.collision[i]=std::byte{1};
    require(state.identities.install(i,record),"callback identity admission");
  }
  auto step=[&]{
    const auto error=world.Update(race::physics::PrimitiveProfile::solver_tick_seconds,1,&scratch,&jobs);
    require(error==JPH::EPhysicsUpdateError::None&&scratch.top==0,"callbacks quiesce with SDK update");
  };
  state.identities.begin_tick();step();require(capture.added>0&&!state.phase.failed,"actual added callback");
  ContactValue previous{};
  if(sensor)require(state.phase.event_count==0&&capture.sensor_contacts>0,"sensor added callback does not publish trigger");
  else {
    require(state.phase.event_count>0,"copied contact enter");previous=state.phase.events[0];
    require(previous.kind==EventKind::ContactEnter&&previous.validity==static_cast<ContactValidity>(7)&&
            previous.first.identity.object.low==2&&previous.second.identity.object.low==3,"canonical copied identities and validity");
    require(previous.normal_first_to_second[1]<0&&previous.point_first_m[1]<previous.point_second_m[1],"swapped geometry follows canonical identities");
  }
  state.phase.event_count=0;state.identities.begin_tick();step();
  require(capture.persisted>0&&!state.phase.failed,"actual persisted callback");
  if(overflow){
    state.phase.event_count=0;
    for(unsigned i=0;i<race::physics::PrimitiveProfile::events;++i)require(state.phase.append(previous),"seed bounded overflow destination");
    state.identities.begin_tick();step();
    require(state.phase.failed&&state.phase.event_count==4096,"actual callback overflows without out-of-bounds write");
    // The isolated coordinator discards this candidate only after Update has
    // returned. No further step is admitted; old accepted copied values survive.
    require(previous.kind==EventKind::ContactEnter&&previous.first.identity.incarnation==1,"old copied result survives failed candidate");
    observation.overflow=true;
  } else if(sensor){
    state.phase.event_count=0;state.identities.begin_tick();api.DeactivateBody(ids[1]);step();
    require(!state.phase.failed&&capture.sensor_removals>0&&state.phase.event_count==0,"sleep removal is not trigger exit");
    observation.sensor_removals=capture.sensor_removals;
  } else {
    state.phase.event_count=0;state.identities.begin_tick();
    api.RemoveBody(ids[1]);api.DestroyBody(ids[1]);require(state.identities.retire(1),"retire destroyed contact body");
    // Keep the old shape solely for this fixture's replacement creation. The
    // copied callback carries only value identity/revision, never this owner.
    JPH::BodyCreationSettings replacement(shapes[1],JPH::RVec3(40,0,0),JPH::Quat::sIdentity(),JPH::EMotionType::Dynamic,1);
    replacement.mAllowSleeping=false;replacement.mGravityFactor=0;
    ids[1]=api.CreateAndAddBody(replacement,JPH::EActivation::Activate);
    IdentityRecord record{};record.occupied=true;record.adapter_id=ids[1].GetIndexAndSequenceNumber();
    record.body.identity={{1,1},{1,2},2};record.body.configuration=99;
    require(!ids[1].IsInvalid()&&state.identities.install(1,record),"replacement identity admitted independently");
    step();require(!state.phase.failed&&capture.removed>0&&state.phase.event_count==1,"actual removal after body destruction");
    const auto& exit=state.phase.events[0];
    require(exit.kind==EventKind::ContactExit&&exit.validity==ContactValidity::None&&exit.first==previous.first&&exit.second==previous.second&&
            exit.point_first_m==Triple{}&&exit.relative_pre_solve_velocity_m_s==Triple{},"removal preserves old identity without fabricated geometry");
  }
  observation.added+=capture.added;observation.persisted+=capture.persisted;observation.removed+=capture.removed;
  world.SetContactListener(nullptr); // All single-thread Update work has returned.
  for(const auto id:ids){api.RemoveBody(id);api.DestroyBody(id);}world.OptimizeBroadPhase();
  for(auto& shape:shapes)shape=nullptr;
}
void run(std::byte* control,Scratch& scratch){
  static_assert(sizeof(State)<control_storage_bytes);
  const auto before=crt_requests.load();initialize_primitives();
  auto* state=std::construct_at(reinterpret_cast<State*>(control));Observations observation;
  exercise(*state,scratch,false,false,observation);
  exercise(*state,scratch,true,false,observation);
  exercise(*state,scratch,false,true,observation);
  std::destroy_at(state);JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&scratch.top==0&&crt_requests.load()==before,"callback owned values and SDK teardown");
  std::printf("{\"contact_owner_passed\":true,\"added\":%u,\"persisted\":%u,\"removed\":%u,\"sensor_sleep_removals\":%u,\"seeded_overflow_rejected\":%s,\"state_bytes\":%zu,\"listener_bytes\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"single_thread_only\":true,\"trigger_reconciliation_qualified\":false,\"scene_publication_qualified\":false,\"allocation_proved\":false}\n",
              observation.added,observation.persisted,observation.removed,observation.sensor_removals,observation.overflow?"true":"false",sizeof(State),sizeof(Capture));
}
} // namespace contact_owner_probe
