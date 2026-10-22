#pragma once
#include "probe_composition.hpp"

namespace qualification::composition {
void execute_sdk(Memory& memory) noexcept {
  static_assert(sizeof(Control)<=960*1024&&alignof(Control)==16);
  Arena arena({memory.sdk,heap_bytes});sdk_arena=&arena;Scratch scratch(memory.sdk+heap_bytes);
  JPH::Allocate=allocate;JPH::Reallocate=reallocate;JPH::Free=release;JPH::AlignedAllocate=aligned_allocate;JPH::AlignedFree=release;JPH::Trace=trace;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed=assertion;
#endif
  auto* control=std::construct_at(reinterpret_cast<Control*>(memory.control));memory.control_used=sizeof(Control);
  require(prepare_products(*control),"all composed fixture products validated before SDK body creation");
  initialize_primitives();
  {
    JPH::JobSystemSingleThreaded jobs(2048);prewarm_jobs(jobs);
    Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    JPH::PhysicsSettings settings;settings.mMaxInFlightBodyPairs=1024;settings.mNumVelocitySteps=10;settings.mNumPositionSteps=2;
    settings.mPenetrationSlop=.001f;settings.mSpeculativeContactDistance=.005f;settings.mManifoldTolerance=.001f;
    world.SetPhysicsSettings(settings);world.SetGravity(JPH::Vec3(0,-9.80665f,0));
    auto& api=world.GetBodyInterface();std::array<JPH::BodyID,256> ids;
    std::array<JPH::RefConst<JPH::Shape>,256> shapes;
    contact_owner_probe::Capture capture(*memory.phase,control->identities);world.SetContactListener(&capture);
    for(std::size_t i=0;i<ids.size();++i){
      auto& record=control->bodies[i];shapes[i]=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f),.005f);
      const auto& p=record.pose.position_m;const bool moving=record.configuration.motion==MotionType::Dynamic;
      JPH::BodyCreationSettings creation(shapes[i],JPH::RVec3(p[0],p[1],p[2]),JPH::Quat::sIdentity(),
          moving?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,moving?1:0);
      creation.mLinearDamping=creation.mAngularDamping=0;creation.mAllowSleeping=record.configuration.allow_sleep;
      creation.mIsSensor=record.configuration.sensor;creation.mFriction=static_cast<float>(record.configuration.friction);
      creation.mRestitution=static_cast<float>(record.configuration.restitution);creation.mUserData=i+1;
      creation.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;creation.mMassPropertiesOverride.mMass=1;
      ids[i]=api.CreateAndAddBody(creation,moving?JPH::EActivation::Activate:JPH::EActivation::DontActivate);
      require(!ids[i].IsInvalid(),"composed body capacity");record.adapter_id=ids[i].GetIndexAndSequenceNumber();
      IdentityRecord identity{};identity.occupied=true;identity.sensor=record.configuration.sensor;identity.adapter_id=record.adapter_id;
      identity.body={record.identity,race::sha256(record.product),record.configuration.revision};
      require(control->identities.install(i,identity),"composed identity map");
    }
    world.OptimizeBroadPhase();
    for(unsigned tick=0;tick<8;++tick){
      control->identities.begin_tick();memory.phase->event_count=0;
      const auto result=world.Update(PrimitiveProfile::solver_tick_seconds,1,&scratch,&jobs);
      require(result==JPH::EPhysicsUpdateError::None&&scratch.top==0&&!memory.phase->failed,"composed update and callbacks quiesce");
    }
    require(capture.added>0&&capture.persisted>0&&capture.sensor_contacts>0,"composed real callbacks and sensor observations");
    for(std::size_t i=0;i<ids.size();++i){
      const auto* identity=control->identities.current(ids[i].GetIndexAndSequenceNumber());require(identity!=nullptr,"composed copied identity");
      auto& pose=memory.phase->poses[i];pose.body=identity->body;pose.validity=1;
      const auto p=api.GetPosition(ids[i]);const auto q=api.GetRotation(ids[i]);const auto v=api.GetLinearVelocity(ids[i]);const auto w=api.GetAngularVelocity(ids[i]);
      pose.pose.position_m={p.GetX(),p.GetY(),p.GetZ()};pose.pose.rotation_xyzw={q.GetX(),q.GetY(),q.GetZ(),q.GetW()};
      pose.motion.linear_m_s={v.GetX(),v.GetY(),v.GetZ()};pose.motion.angular_rad_s={w.GetX(),w.GetY(),w.GetZ()};
      auto& candidate=control->candidate[i];candidate.body=identity->body;candidate.primitive=control->bodies[i].primitive;
      candidate.pose=pose.pose;candidate.category=candidate.mask=1;candidate.occupied=true;candidate.sensor=identity->sensor;
    }
    memory.phase->pose_count=256;
    struct View {
      trigger_owner_probe::Snapshots& previous;trigger_owner_probe::Snapshots& candidate;
      trigger_owner_probe::Pairs& previous_pairs;trigger_owner_probe::Pairs& candidate_pairs;PhaseValues& phase;
    } view{control->previous,control->candidate,control->previous_pairs,control->candidate_pairs,*memory.phase};
    require(trigger_owner_probe::reconcile(view,[&](std::size_t a,std::size_t b)noexcept{return trigger_owner_probe::sdk_overlap(api,ids[a],ids[b]);},memory.pair_tests),"composed trigger candidate");
    bool trigger=false,contact=false;
    for(std::size_t i=0;i<memory.phase->event_count;++i){trigger|=memory.phase->events[i].kind==EventKind::TriggerEnter;contact|=memory.phase->events[i].kind==EventKind::ContactPersist;}
    require(trigger&&contact,"contacts and triggers share the actual phase allocation");memory.events=memory.phase->event_count;
    struct Collector final:JPH::CollidePointCollector {
      QueryValues& result;const RemovalIdentities& identities;const IdentityRecord* body{};
      Collector(QueryValues& value,const RemovalIdentities& map):result(value),identities(map){}
      void OnBody(const JPH::Body& value) noexcept override {body=identities.current(value.GetID().GetIndexAndSequenceNumber());}
      void AddHit(const JPH::CollidePointResult&) noexcept override {
        require(body!=nullptr&&result.count<result.hits.size(),"composed bounded query capture");
        auto& hit=result.hits[result.count++];hit.body=body->body;hit.kind=QueryKind::PointOverlap;
      }
    };
    for(auto* output:memory.queries){Collector collector(*output,control->identities);world.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),collector);
      require(output->count==1&&output->hits[0].body.identity.object.low==129,"composed query owns exact stable hit");}
    world.SetContactListener(nullptr);
    for(const auto id:ids){api.RemoveBody(id);api.DestroyBody(id);}world.OptimizeBroadPhase();
    for(auto& shape:shapes)shape=nullptr;
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(arena.stats.live_bytes==0&&scratch.top==0,"composed SDK owners quiescent before backing release");
  memory.sdk_peak=arena.stats.peak_bytes;memory.scratch_peak=scratch.peak;std::destroy_at(control);
  JPH::Allocate=nullptr;JPH::Reallocate=nullptr;JPH::Free=nullptr;JPH::AlignedAllocate=nullptr;JPH::AlignedFree=nullptr;sdk_arena=nullptr;
}
} // namespace qualification::composition
