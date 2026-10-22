#pragma once
#include "owned_records.hpp"

namespace trigger_owner_probe {
using namespace qualification::owned;
constexpr std::size_t count=PrimitiveProfile::bodies,pair_count=count*(count-1)/2;
using Snapshots=std::array<QuerySnapshotValue,count>;
using Pairs=std::array<bool,pair_count>;
struct State {Snapshots previous{},candidate{};Pairs previous_pairs{},candidate_pairs{};PhaseValues phase{};};
constexpr std::size_t index(std::size_t i,std::size_t j) noexcept {
  if(i>j)std::swap(i,j);
  return i*(2*count-i-1)/2+j-i-1;
}
auto key(const BodyIdentity& id) noexcept {return std::array{id.section.high,id.section.low,id.object.high,id.object.low,id.incarnation};}
template<class S>bool append(S& s,const QuerySnapshotValue& a,const QuerySnapshotValue& b,EventKind kind) noexcept {
  ContactValue event{};event.first=a.body;event.second=b.body;event.kind=kind;
  if(key(event.first.identity)>key(event.second.identity))std::swap(event.first,event.second);
  return s.phase.append(event); // Pair-level triggers advertise no contact geometry.
}
// Input admission/stamping and Scene commit are separate obligations. This
// bounded reconciliation runs only after solver quiescence, on owned snapshots.
// The overlap callable may synchronously borrow candidate shapes, never retain
// them or inspect a previous SDK body. Previous authority is unchanged on failure.
template<class S,class Overlap>bool reconcile(S& s,Overlap overlap,std::size_t& pair_tests) noexcept {
  if(s.phase.failed)return false;
  s.candidate_pairs.fill(false);pair_tests=0;
  std::array<std::uint16_t,count> old_to_new{},new_to_old{};
  old_to_new.fill(count);new_to_old.fill(count);
  for(std::size_t i=0;i<count;++i)if(s.previous[i].occupied)
    for(std::size_t j=0;j<count;++j)if(s.candidate[j].occupied&&s.previous[i].body.identity==s.candidate[j].body.identity){
      if(old_to_new[i]!=count||new_to_old[j]!=count){s.phase.failed=true;return false;}
      old_to_new[i]=static_cast<std::uint16_t>(j);new_to_old[j]=static_cast<std::uint16_t>(i);
    }
  for(std::size_t i=0;i<count;++i)for(std::size_t j=i+1;j<count;++j){
    const auto& a=s.candidate[i];const auto& b=s.candidate[j];
    if(!a.occupied||!b.occupied||!(a.sensor||b.sensor)||!(a.category&b.mask)||!(b.category&a.mask))continue;
    ++pair_tests;s.candidate_pairs[index(i,j)]=overlap(i,j);
  }
  // Exit records use old revisions and survive slot reuse; new entries use the
  // candidate revisions. Identity remapping also handles mere slot permutation.
  for(std::size_t i=0;i<count;++i)for(std::size_t j=i+1;j<count;++j)if(s.previous_pairs[index(i,j)]){
    const auto a=old_to_new[i],b=old_to_new[j];
    if(a==count||b==count||!s.candidate_pairs[index(a,b)])
      if(!append(s,s.previous[i],s.previous[j],EventKind::TriggerExit))return false;
  }
  for(std::size_t i=0;i<count;++i)for(std::size_t j=i+1;j<count;++j)if(s.candidate_pairs[index(i,j)]){
    const auto a=new_to_old[i],b=new_to_old[j];
    const bool existed=a!=count&&b!=count&&s.previous_pairs[index(a,b)];
    if(!append(s,s.candidate[i],s.candidate[j],existed?EventKind::TriggerPersist:EventKind::TriggerEnter))return false;
  }
  s.phase.order_events();return true;
}
void commit_fixture(State& s) noexcept {
  require(!s.phase.failed,"never commit failed trigger candidate");
  s.previous=s.candidate;s.previous_pairs=s.candidate_pairs;s.phase.event_count=0;
}
struct Hit final:JPH::CollideShapeCollector {
  bool overlap{};
  void AddHit(const JPH::CollideShapeResult&) noexcept override {overlap=true;ForceEarlyOut();}
};
bool sdk_overlap(const JPH::BodyInterface& api,JPH::BodyID a,JPH::BodyID b) noexcept {
  // Preserve actual rounded BoxShape geometry and pinned SDK tolerances. This
  // is pair work, not a public query against tentative authority. No collector
  // result, shape reference or native transform escapes this synchronous call.
  const auto first=api.GetShape(a),second=api.GetShape(b);
  const auto ta=api.GetCenterOfMassTransform(a),tb=api.GetCenterOfMassTransform(b);
  const auto base=ta.GetTranslation();const auto x=ta.PostTranslated(-base).ToMat44(),y=tb.PostTranslated(-base).ToMat44();
  if(!first->GetWorldSpaceBounds(x,JPH::Vec3::sOne()).Overlaps(second->GetWorldSpaceBounds(y,JPH::Vec3::sOne())))return false;
  JPH::CollideShapeSettings settings;settings.mMaxSeparationDistance=0;
  settings.mCollisionTolerance=JPH::cDefaultCollisionTolerance;settings.mPenetrationTolerance=JPH::cDefaultPenetrationTolerance;
  settings.mCollectFacesMode=JPH::ECollectFacesMode::NoFaces;
  Hit hit;JPH::CollisionDispatch::sCollideShapeVsShape(first,second,JPH::Vec3::sOne(),JPH::Vec3::sOne(),x,y,
      JPH::SubShapeIDCreator{},JPH::SubShapeIDCreator{},settings,hit);
  return hit.overlap;
}
QuerySnapshotValue snapshot(std::uint64_t object,bool sensor) noexcept {
  QuerySnapshotValue value{};value.occupied=true;value.sensor=sensor;value.category=value.mask=1;
  value.body.identity={{1,1},{1,object},1};value.body.configuration=1;return value;
}
void reset(State& s) noexcept {
  // In-place clears avoid a megabyte temporary on the separately bounded stack.
  for(auto& v:s.previous)v={};for(auto& v:s.candidate)v={};
  s.previous_pairs.fill(false);s.candidate_pairs.fill(false);s.phase.event_count=0;s.phase.failed=false;
}
void run(std::byte* control){
  static_assert(sizeof(State)<control_storage_bytes&&sizeof(Pairs)==32640);
  const auto before=crt_requests.load();initialize_primitives();
  auto* s=std::construct_at(reinterpret_cast<State*>(control));std::size_t pair_tests{},native_pairs{},max_pairs{};
  // Real static/sleeping world shapes remain available independently of contact
  // callbacks. Sphere/sphere, rounded box/sphere and rounded box/box are covered
  // at large positive and negative world origins, rebased before float geometry.
  for(unsigned variant=0;variant<3;++variant)for(double origin:std::array{0.,1e9,-1e9}){
    reset(*s);Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    auto& api=world.GetBodyInterface();std::array<JPH::BodyID,2> ids;
    for(unsigned i=0;i<2;++i){
      JPH::RefConst<JPH::Shape> shape=(variant==0||(variant==1&&i==1))?
          static_cast<JPH::Shape*>(new JPH::SphereShape(.5f)):
          static_cast<JPH::Shape*>(new JPH::BoxShape(JPH::Vec3::sReplicate(.5f),.005f));
      JPH::BodyCreationSettings creation(shape,JPH::RVec3(origin,0,0),JPH::Quat::sRotation(JPH::Vec3::sAxisY(),.25f*float(i)),
          i==0?JPH::EMotionType::Static:JPH::EMotionType::Dynamic,i==0?0:1);
      creation.mIsSensor=i==0;ids[i]=api.CreateAndAddBody(creation,JPH::EActivation::DontActivate);
      require(!ids[i].IsInvalid(),"trigger body admission");s->candidate[i]=snapshot(i+1,i==0);
    }
    const auto allocations=sdk_arena->stats.requests;
    const auto overlap=[&](std::size_t a,std::size_t b)noexcept{++native_pairs;return sdk_overlap(api,ids[a],ids[b]);};
    require(reconcile(*s,overlap,pair_tests)&&pair_tests==1&&s->phase.event_count==1&&s->phase.events[0].kind==EventKind::TriggerEnter,"real geometric trigger entry");
    commit_fixture(*s);require(!api.IsActive(ids[1]),"trigger body remains sleeping");
    require(reconcile(*s,overlap,pair_tests)&&s->phase.event_count==1&&s->phase.events[0].kind==EventKind::TriggerPersist,"sleeping geometric overlap persists");
    commit_fixture(*s);api.SetPosition(ids[1],JPH::RVec3(origin+3,0,0),JPH::EActivation::DontActivate);
    require(reconcile(*s,overlap,pair_tests)&&s->phase.event_count==1&&s->phase.events[0].kind==EventKind::TriggerExit&&s->phase.events[0].validity==ContactValidity::None,"separation exits with owned identities only");
    require(sdk_arena->stats.requests==allocations,"synchronous trigger pair work allocates no SDK storage");
    for(const auto id:ids){api.RemoveBody(id);api.DestroyBody(id);}world.OptimizeBroadPhase();
  }
  reset(*s);s->candidate[0]=snapshot(1,true);s->candidate[1]=snapshot(2,false);
  const auto yes=[](std::size_t,std::size_t)noexcept{return true;};
  require(reconcile(*s,yes,pair_tests),"trigger value setup");commit_fixture(*s);
  std::swap(s->candidate[0],s->candidate[1]);
  require(reconcile(*s,yes,pair_tests)&&s->phase.event_count==1&&s->phase.events[0].kind==EventKind::TriggerPersist,"slot permutation preserves identity continuity");commit_fixture(*s);
  ++s->candidate[0].body.identity.incarnation;
  require(reconcile(*s,yes,pair_tests)&&s->phase.event_count==2,"replacement produces old exit and new enter");
  bool enter=false,exit=false;
  for(unsigned i=0;i<2;++i){const auto& e=s->phase.events[i];enter|=e.kind==EventKind::TriggerEnter&&e.second.identity.incarnation==2;exit|=e.kind==EventKind::TriggerExit&&e.second.identity.incarnation==1;}
  require(enter&&exit,"replacement event incarnations");commit_fixture(*s);
  s->candidate[0].mask=0;
  require(reconcile(*s,yes,pair_tests)&&pair_tests==0&&s->phase.event_count==1&&s->phase.events[0].kind==EventKind::TriggerExit,"filter removal does not query or retain overlap");
  reset(*s);for(std::size_t i=0;i<count;++i)s->candidate[i]=snapshot(i+1,true);
  const auto no=[](std::size_t,std::size_t)noexcept{return false;};
  require(reconcile(*s,no,pair_tests)&&pair_tests==pair_count&&s->phase.event_count==0,"complete bounded candidate pair enumeration");max_pairs=pair_tests;
  require(!reconcile(*s,yes,pair_tests)&&s->phase.failed&&s->phase.event_count==4096,"dense derived trigger overflow is explicit");
  require(std::none_of(s->previous_pairs.begin(),s->previous_pairs.end(),[](bool x){return x;}),"failed trigger candidate preserves previous pair authority");
  std::destroy_at(s);JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==before,"trigger fixture teardown and no CRT allocation");
  std::printf("{\"trigger_owner_passed\":true,\"native_pairs\":%zu,\"max_pair_tests\":%zu,\"pair_table_bytes\":%zu,\"state_bytes\":%zu,\"sleeping_persists\":true,\"replacement_and_filter_tested\":true,\"overflow_rejected\":true,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"numeric_boundary_qualified\":false,\"scene_publication_qualified\":false,\"allocation_proved\":false}\n",native_pairs,max_pairs,sizeof(Pairs),sizeof(State));
}
} // namespace trigger_owner_probe
