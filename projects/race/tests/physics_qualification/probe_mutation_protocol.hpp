#pragma once
// Isolated qualification protocol, not a Runtime adapter or an accepted profile.
// All SDK mutation is serialized here; no body, shape or query-context escapes.
namespace protocol_probe {
using Profile=race::physics::PrimitiveProfile;
enum class Admission { Ready, Busy, Capacity, Invalid };
class Session {
  enum class Phase { Idle, Prepared, Query, Update, Failed };
  struct Slot {JPH::BodyID id;JPH::RefConst<JPH::Shape> shape;bool invalid{},moving{};};
  struct Pending {JPH::Body* body{};JPH::RefConst<JPH::Shape> shape;unsigned slot{};bool moving{};};
  Layers layers_;ObjectBroadFilter broad_filter_;PairFilter pair_filter_;
  JPH::PhysicsSystem world_;
  std::array<Slot,Profile::bodies> slots_{};
  std::array<Pending,Profile::bodies> pending_{};
  std::array<JPH::RefConst<JPH::Shape>,Profile::shapes> retired_{};
  Phase phase_{Phase::Idle};
  unsigned bodies_{},moving_{},shapes_{},pending_count_{},pending_moving_{},retired_count_{};
  // Conservative admission hold, NOT an assertion that Update(0) clears the SDK
  // vector. Credits cover distinct lifetimes since an actual active solver step.
  unsigned invalidations_{};
  void maintenance(){world_.OptimizeBroadPhase();++maintenance_calls;}
public:
  unsigned maximum_allocated_bodies{},maximum_owned_shapes{},maintenance_calls{},completed_steps{};
  Session(){world_.Init(Profile::bodies,1,Profile::body_pairs,Profile::contacts,layers_,broad_filter_,pair_filter_);}
  Session(const Session&)=delete;
  Session& operator=(const Session&)=delete;
  ~Session(){require(phase_==Phase::Idle&&bodies_==0&&pending_count_==0&&shapes_==0,"protocol owners must quiesce before world teardown");}
  unsigned invalidations()const{return invalidations_;}
  unsigned bodies()const{return bodies_;}
  Admission stage(unsigned count,unsigned moving){
    if(phase_!=Phase::Idle)return Admission::Busy;
    if(!count||moving>count)return Admission::Invalid;
    if(count>Profile::commands||count>Profile::bodies-bodies_||moving>Profile::moving_bodies-moving_||count>Profile::shapes-shapes_)
      return Admission::Capacity;
    // Check the entire batch before either shape or body allocation. Pending
    // bodies consume the same 256-body limit even before registry assignment.
    phase_=Phase::Prepared;pending_count_=count;pending_moving_=moving;
    unsigned next=0;
    for(unsigned i=0;i<count;++i){
      while(slots_[next].shape!=nullptr)++next;
      auto& p=pending_[i];p.slot=next++;p.moving=i<moving;
      p.shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.05f),.005f);
      JPH::BodyCreationSettings creation(p.shape,JPH::RVec3(double(p.slot%16)*2,0,double(p.slot/16)*2),
        JPH::Quat::sIdentity(),p.moving?JPH::EMotionType::Kinematic:JPH::EMotionType::Static,p.moving?1:0);
      p.body=world_.GetBodyInterface().CreateBodyWithoutID(creation);
      require(p.body!=nullptr&&p.body->GetID().IsInvalid(),"admitted pending body");
    }
    shapes_+=count;
    maximum_allocated_bodies=std::max(maximum_allocated_bodies,bodies_+pending_count_);
    maximum_owned_shapes=std::max(maximum_owned_shapes,shapes_);
    return Admission::Ready;
  }
  Admission cancel(){
    if(phase_!=Phase::Prepared)return Admission::Busy;
    for(unsigned i=0;i<pending_count_;++i){auto& p=pending_[i];world_.GetBodyInterface().DestroyBodyWithoutID(p.body);p.body=nullptr;p.shape=nullptr;}
    shapes_-=pending_count_;pending_count_=pending_moving_=0;phase_=Phase::Idle;return Admission::Ready;
  }
  Admission publish(){
    if(phase_!=Phase::Prepared)return Admission::Busy;
    // Full wrapper maintenance includes dirty empty layers and both per-layer
    // generations. It is legal only with no borrowed query or update in flight.
    maintenance();auto& api=world_.GetBodyInterface();std::array<JPH::BodyID,Profile::bodies> batch{};
    for(unsigned i=0;i<pending_count_;++i){
      auto& p=pending_[i];require(api.AssignBodyID(p.body),"preflighted body registration");batch[i]=p.body->GetID();
      auto& slot=slots_[p.slot];slot.id=batch[i];slot.shape=p.shape;slot.invalid=false;slot.moving=p.moving;p.body=nullptr;p.shape=nullptr;
    }
    auto add=api.AddBodiesPrepare(batch.data(),int(pending_count_));
    api.AddBodiesFinalize(batch.data(),int(pending_count_),add,JPH::EActivation::DontActivate);
    bodies_+=pending_count_;moving_+=pending_moving_;pending_count_=pending_moving_=0;
    maintenance();phase_=Phase::Idle;return Admission::Ready;
  }
  Admission remove_all(bool retain_shapes=false){
    if(phase_!=Phase::Idle)return Admission::Busy;
    if(!bodies_)return Admission::Ready;
    std::array<JPH::BodyID,Profile::bodies> ids{};unsigned count=0;
    for(const auto& slot:slots_)if(slot.shape!=nullptr)ids[count++]=slot.id;
    auto& api=world_.GetBodyInterface();api.RemoveBodies(ids.data(),int(count));api.DestroyBodies(ids.data(),int(count));
    for(auto& slot:slots_)if(slot.shape!=nullptr){
      if(retain_shapes){require(retired_count_<retired_.size(),"retired shape ownership bound");retired_[retired_count_++]=slot.shape;}
      else --shapes_;
      slot.shape=nullptr;slot.id=JPH::BodyID();slot.invalid=false;slot.moving=false;
    }
    bodies_=moving_=0;maintenance();return Admission::Ready;
  }
  Admission release_retired(){
    if(phase_!=Phase::Idle)return Admission::Busy;
    for(unsigned i=0;i<retired_count_;++i)retired_[i]=nullptr;
    shapes_-=retired_count_;retired_count_=0;return Admission::Ready;
  }
  Admission invalidate(){
    if(phase_!=Phase::Idle)return Admission::Busy;
    unsigned fresh=0;for(const auto& slot:slots_)if(slot.shape!=nullptr&&!slot.invalid)++fresh;
    // This hold is explicit and preserved across paused/no-active updates. It is
    // a bounded alternative for qualification, not approval to restrict editing.
    if(fresh>Profile::commands-invalidations_)return Admission::Capacity;
    for(auto& slot:slots_)if(slot.shape!=nullptr&&!slot.invalid){world_.GetBodyInterface().InvalidateContactCache(slot.id);slot.invalid=true;}
    invalidations_+=fresh;return Admission::Ready;
  }
  Admission activate_moving(){
    if(phase_!=Phase::Idle)return Admission::Busy;
    for(const auto& slot:slots_)if(slot.shape!=nullptr&&slot.moving)world_.GetBodyInterface().ActivateBody(slot.id);
    return Admission::Ready;
  }
  Admission update(bool advance,Scratch& scratch,JPH::JobSystemSingleThreaded& jobs){
    if(phase_!=Phase::Idle)return Admission::Busy;
    const bool clears=advance&&world_.GetNumActiveBodies(JPH::EBodyType::RigidBody)>0;
    phase_=Phase::Update;
    const auto result=world_.Update(advance?Profile::solver_tick_seconds:0.f,1,&scratch,&jobs);
    require(scratch.top==0,"protocol update scratch quiescence");
    if(result!=JPH::EPhysicsUpdateError::None){phase_=Phase::Failed;return Admission::Invalid;}
    if(clears){invalidations_=0;for(auto& slot:slots_)slot.invalid=false;++completed_steps;}
    phase_=Phase::Idle;return Admission::Ready;
  }
  void check_flags(bool expected){
    for(const auto& slot:slots_)if(slot.shape!=nullptr){
      JPH::BodyLockRead lock(world_.GetBodyLockInterface(),slot.id);
      require(lock.Succeeded()&&lock.GetBody().IsCollisionCacheInvalid()==expected,"actual SDK invalidation flags");
    }
  }
  Admission query_reentry(){
    if(phase_!=Phase::Idle)return Admission::Busy;
    struct Collector final:JPH::CollidePointCollector {
      Session& owner;unsigned hits{};explicit Collector(Session& session):owner(session){}
      void AddHit(const JPH::CollidePointResult&)override{
        const auto requests=sdk_arena->stats.requests;
        require(owner.stage(1,0)==Admission::Busy&&owner.remove_all()==Admission::Busy&&owner.release_retired()==Admission::Busy,"query borrow excludes mutations and frees");
        require(requests==sdk_arena->stats.requests,"query rejection precedes SDK allocation");++hits;
      }
    } collector(*this);
    phase_=Phase::Query;world_.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),collector);phase_=Phase::Idle;
    require(collector.hits==1,"real query callback exercised protocol rejection");return Admission::Ready;
  }
};

void run(Scratch& scratch){
  const auto crt_before=crt_requests.load();initialize_primitives();
  unsigned maintenance=0,steps=0;std::size_t cache_requests=0,stable_bytes=0;
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);Session session;
    const auto original=sdk_arena->stats.live_bytes;
    require(session.stage(256,0)==Admission::Ready,"full pending batch admitted");
    auto requests=sdk_arena->stats.requests;
    require(session.stage(1,0)==Admission::Busy&&session.query_reentry()==Admission::Busy&&requests==sdk_arena->stats.requests,"pending batch exclusive ownership");
    require(session.cancel()==Admission::Ready&&sdk_arena->stats.live_bytes==original,"cancel frees all unassigned owners");
    require(session.stage(256,0)==Admission::Ready&&session.publish()==Admission::Ready,"full static batch publication");
    requests=sdk_arena->stats.requests;
    require(session.stage(1,0)==Admission::Capacity&&requests==sdk_arena->stats.requests,"full body registry rejected before allocation");
    AllocationTrace cache;allocation_trace=&cache;
    require(session.invalidate()==Admission::Ready&&session.invalidate()==Admission::Ready,"duplicate invalidation consumes no extra credit");
    allocation_trace=nullptr;cache_requests=cache.count;
    require(cache.count==9&&cache.frees==8&&session.invalidations()==256,"bounded initial cache growth");
    session.check_flags(true);require(session.query_reentry()==Admission::Ready,"query quiescence transition");
    require(session.update(false,scratch,jobs)==Admission::Ready&&session.invalidations()==256,"paused Update does not release invalidation credits");session.check_flags(true);
    require(session.update(true,scratch,jobs)==Admission::Ready&&session.invalidations()==256,"no-active-body Update does not release credits");session.check_flags(true);
    require(session.remove_all(true)==Admission::Ready,"old shapes retained beyond body removal");
    requests=sdk_arena->stats.requests;
    require(session.stage(1,0)==Admission::Capacity&&requests==sdk_arena->stats.requests,"retained shapes count against admission");
    require(session.release_retired()==Admission::Ready&&session.stage(256,0)==Admission::Ready&&session.publish()==Admission::Ready,"recreated static batch");
    requests=sdk_arena->stats.requests;
    require(session.invalidate()==Admission::Capacity&&requests==sdk_arena->stats.requests,"paused churn rejected before vector growth");session.check_flags(false);
    require(session.remove_all()==Admission::Ready&&session.stage(1,1)==Admission::Ready&&session.publish()==Admission::Ready,"active-step recovery setup");
    require(session.activate_moving()==Admission::Ready&&session.update(true,scratch,jobs)==Admission::Ready&&session.invalidations()==0,"only completed active step releases credits");
    require(session.invalidate()==Admission::Ready&&session.invalidations()==1,"post-step invalidation admitted");session.check_flags(true);
    require(session.activate_moving()==Admission::Ready&&session.update(true,scratch,jobs)==Admission::Ready&&session.invalidations()==0,"active step clears actual flags");session.check_flags(false);
    require(session.remove_all()==Admission::Ready,"active body retirement");
    for(unsigned cycle=0;cycle<32;++cycle){
      require(session.stage(256,128)==Admission::Ready&&session.publish()==Admission::Ready,"two-layer paused churn publish");
      require(session.update(false,scratch,jobs)==Admission::Ready,"real paused Update path");
      require(session.remove_all()==Admission::Ready&&session.update(false,scratch,jobs)==Admission::Ready,"dirty empty layers maintained without time advance");
      if(cycle==0)stable_bytes=sdk_arena->stats.live_bytes;
      else require(sdk_arena->stats.live_bytes==stable_bytes,"paused churn retained allocation stability");
    }
    require(session.maximum_allocated_bodies==256&&session.maximum_owned_shapes==256&&session.bodies()==0,"pending and retained ownership ceilings");
    maintenance=session.maintenance_calls;steps=session.completed_steps;
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"protocol complete SDK teardown");
  std::printf("{\"protocol_passed\":true,\"qualification_only\":true,\"paused_invalidation_hold\":true,\"no_active_invalidation_hold\":true,\"cache_admission_ceiling\":256,\"cache_requests\":%zu,\"pending_body_ceiling\":256,\"owned_shape_ceiling\":256,\"paused_two_layer_cycles\":32,\"maintenance_calls\":%u,\"active_steps\":%u,\"session_stack_bytes\":%zu,\"retained_sdk_bytes_after_empty\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",cache_requests,maintenance,steps,sizeof(Session),stable_bytes);
}

void paused_empty_shortage(Scratch& scratch){
  initialize_primitives();Layers layers;ObjectBroadFilter broad;PairFilter pair;
  JPH::JobSystemSingleThreaded jobs(Profile::jobs);JPH::PhysicsSystem world;
  world.Init(256,1,32768,32768,layers,broad,pair);
  JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.05f),.005f);
  std::array<JPH::BodyID,256> ids{};auto& api=world.GetBodyInterface();
  for(unsigned i=0;i<256;++i){
    JPH::BodyCreationSettings creation(shape,JPH::RVec3(double(i%16)*2,0,double(i/16)*2),JPH::Quat::sIdentity(),
      i<128?JPH::EMotionType::Kinematic:JPH::EMotionType::Static,i<128?1:0);
    auto* body=api.CreateBody(creation);require(body!=nullptr,"paused shortage body creation");ids[i]=body->GetID();
  }
  for(unsigned cycle=0;cycle<32;++cycle){
    auto batch=ids;auto state=api.AddBodiesPrepare(batch.data(),256);
    api.AddBodiesFinalize(batch.data(),256,state,JPH::EActivation::DontActivate);
    api.RemoveBodies(batch.data(),256);
    require(world.Update(0,1,&scratch,&jobs)==JPH::EPhysicsUpdateError::None,"ordinary paused maintenance only");
  }
  rejected("paused empty-layer fault did not terminate");
}
} // namespace protocol_probe
