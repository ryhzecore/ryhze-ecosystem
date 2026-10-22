#pragma once
// Qualification-only owner/SDK protocol. This does not implement Scene, Runtime
// integration, a general body-command API, or an accepted complete memory bound.
namespace deferred_probe {
using Profile=race::physics::PrimitiveProfile;
enum class Outcome { Ready, Capacity, Stale, Busy, Invalid, Failed };
enum class Kind { Create, Erase, Position, Extent, Impulse };
enum class Motion { Static, Kinematic, Dynamic };
struct Stamp {std::uint64_t session{},generation{},tick{};bool operator==(const Stamp&)const=default;};
struct Row {
  std::uint64_t identity{1};bool present{};Motion motion{Motion::Static};
  std::array<double,3> position{};std::array<float,3> velocity{};float extent{.1f};
};
struct Command {Kind kind{};unsigned slot{},axis{};std::uint64_t identity{1};double value{};Motion motion{Motion::Static};};
struct Published {Stamp stamp;std::array<Row,Profile::bodies> rows{};std::array<Kind,Profile::commands> order{};unsigned order_count{};};
class Session {
  enum class Phase { Idle, Query, Apply, Failed, Stopped };
  Layers layers_;ObjectBroadFilter broad_filter_;PairFilter pair_filter_;JPH::PhysicsSystem world_;
  Published published_;
  std::array<Command,Profile::commands> pending_{};unsigned pending_count_{};
  std::array<JPH::BodyID,Profile::bodies> ids_{};
  std::array<JPH::RefConst<JPH::Shape>,Profile::shapes> shapes_{};
  Phase phase_{Phase::Idle};
  bool permitted()const{return phase_==Phase::Idle;}
  void maintenance(){world_.OptimizeBroadPhase();++maintenance_calls;}
  Outcome replay(std::array<Row,Profile::bodies>& rows)const{
    for(unsigned i=0;i<pending_count_;++i){
      const auto& c=pending_[i];auto& row=rows[c.slot];
      if(row.identity!=c.identity)return Outcome::Stale;
      if(c.kind==Kind::Create){
        if(row.present)return Outcome::Invalid;
        row=Row{};row.identity=c.identity;row.present=true;row.motion=c.motion;
        row.position={double(c.slot%16)*2,0,double(c.slot/16)*2};
      }else{
        if(!row.present)return Outcome::Invalid;
        switch(c.kind){
        case Kind::Erase:
          if(row.identity==std::numeric_limits<std::uint64_t>::max())return Outcome::Invalid;
          row.present=false;++row.identity;break;
        case Kind::Position:row.position[c.axis]=c.value;break;
        case Kind::Extent:row.extent=static_cast<float>(c.value);break;
        case Kind::Impulse:
          if(row.motion!=Motion::Dynamic)return Outcome::Invalid;
          // This fixture fixes mass at 1 kg and admits linear impulses only.
          // Preserve float additions in command order, including before erase.
          row.velocity[c.axis]+=static_cast<float>(c.value);
          if(!std::isfinite(row.velocity[c.axis])||std::abs(row.velocity[c.axis])>1.f)return Outcome::Invalid;
          break;
        default:return Outcome::Invalid;
        }
      }
    }
    unsigned moving=0;for(const auto& row:rows)if(row.present&&row.motion!=Motion::Static)++moving;
    return moving<=Profile::moving_bodies?Outcome::Ready:Outcome::Capacity;
  }
  void clear_world(){
    std::array<JPH::BodyID,Profile::bodies> batch{};unsigned count=0;
    for(const auto& id:ids_)if(!id.IsInvalid())batch[count++]=id;
    if(count){auto& api=world_.GetBodyInterface();api.RemoveBodies(batch.data(),int(count));api.DestroyBodies(batch.data(),int(count));}
    for(unsigned i=0;i<ids_.size();++i){ids_[i]=JPH::BodyID();shapes_[i]=nullptr;}
    maintenance();
  }
public:
  unsigned coalesced{},queue_high_water{},native_updates{},idle_updates{},maintenance_calls{},query_rejections{};
  explicit Session(std::uint64_t session){
    require(session!=0,"nonzero deferred session identity");published_.stamp.session=session;
    world_.Init(Profile::bodies,1,Profile::body_pairs,Profile::contacts,layers_,broad_filter_,pair_filter_);
    JPH::PhysicsSettings settings;settings.mMaxInFlightBodyPairs=Profile::in_flight_pairs;
    settings.mNumVelocitySteps=10;settings.mNumPositionSteps=2;
    world_.SetPhysicsSettings(settings);world_.SetGravity(JPH::Vec3(0,-9.80665f,0));
  }
  Session(const Session&)=delete;Session& operator=(const Session&)=delete;
  ~Session(){clear_world();}
  Published published()const{return published_;} // Owned copy; no mutable authority borrow escapes.
  Stamp stamp()const{return published_.stamp;}
  unsigned pending()const{return pending_count_;}
  Outcome enqueue(Stamp stamp,Command command){
    if(!permitted())return phase_==Phase::Failed||phase_==Phase::Stopped?Outcome::Failed:Outcome::Busy;
    if(stamp!=published_.stamp)return Outcome::Stale;
    if(command.slot>=Profile::bodies||command.axis>=3||command.identity==0||!std::isfinite(command.value))return Outcome::Invalid;
    if(command.kind!=Kind::Create&&command.kind!=Kind::Erase&&command.kind!=Kind::Position&&command.kind!=Kind::Extent&&command.kind!=Kind::Impulse)return Outcome::Invalid;
    if(command.kind==Kind::Create&&command.motion!=Motion::Static&&command.motion!=Motion::Kinematic&&command.motion!=Motion::Dynamic)return Outcome::Invalid;
    if(command.kind==Kind::Extent&&(command.value<.1||command.value>10))return Outcome::Invalid;
    if(command.kind==Kind::Position&&std::abs(command.value)>990)return Outcome::Invalid;
    if(command.kind==Kind::Impulse&&std::abs(command.value)>.1)return Outcome::Invalid;
    if(command.kind==Kind::Position||command.kind==Kind::Extent){
      // Only these two value assignments are declared replaceable here. Never
      // cross a create/delete/impulse ordering barrier, or an entity lifetime.
      for(unsigned i=pending_count_;i>0;--i){auto& old=pending_[i-1];
        if(old.kind!=Kind::Position&&old.kind!=Kind::Extent)break;
        if(old.kind==command.kind&&old.slot==command.slot&&old.identity==command.identity&&old.axis==command.axis){old=command;++coalesced;return Outcome::Ready;}
      }
    }
    if(pending_count_==pending_.size())return Outcome::Capacity;
    pending_[pending_count_++]=command;queue_high_water=std::max(queue_high_water,pending_count_);return Outcome::Ready;
  }
  Outcome cancel(Stamp stamp){
    if(!permitted())return Outcome::Busy;
    if(stamp!=published_.stamp)return Outcome::Stale;
    pending_count_=0;return Outcome::Ready;
  }
  // One explicit Resume/Step evaluation, exactly one 1/60 s SDK call. No
  // maintenance call advances time. Publication here is an isolated owner,
  // not a claim that Scene/P04.04 composition has been implemented.
  Outcome advance(Scratch& scratch,JPH::JobSystemSingleThreaded& jobs,bool reject_publication=false,bool fail_after_mutation=false){
    if(!permitted())return phase_==Phase::Failed||phase_==Phase::Stopped?Outcome::Failed:Outcome::Busy;
    if(published_.stamp.generation==std::numeric_limits<std::uint64_t>::max()||published_.stamp.tick==std::numeric_limits<std::uint64_t>::max())return Outcome::Invalid;
    auto next=published_.rows;const auto admitted=replay(next);if(admitted!=Outcome::Ready)return admitted;
    phase_=Phase::Apply;auto& api=world_.GetBodyInterface();
    std::array<bool,Profile::bodies> replace{};std::array<JPH::BodyID,Profile::bodies> removed{},added{};
    unsigned removed_count=0,added_count=0;
    const auto before_ids=ids_;
    for(unsigned i=0;i<next.size();++i){const auto& old=published_.rows[i];const auto& row=next[i];
      replace[i]=row.present&&(!old.present||old.identity!=row.identity||old.extent!=row.extent||old.motion!=row.motion);
      if(!ids_[i].IsInvalid()&&(!row.present||replace[i]))removed[removed_count++]=ids_[i];
    }
    // Retire ALL old bodies before assigning ANY replacement ID. Each SDK slot
    // can be reused at most once per tick. Prior cache identities differ by one
    // generation; even the uint8 wrap cannot alias that immediately prior ID.
    if(removed_count){api.RemoveBodies(removed.data(),int(removed_count));api.DestroyBodies(removed.data(),int(removed_count));}
    for(unsigned i=0;i<next.size();++i)if(!next[i].present||replace[i]){ids_[i]=JPH::BodyID();shapes_[i]=nullptr;}
    maintenance();
    if(fail_after_mutation){phase_=Phase::Failed;pending_count_=0;return Outcome::Failed;}
    for(unsigned i=0;i<next.size();++i){auto& row=next[i];if(!row.present)continue;
      const JPH::RVec3 position(row.position[0],row.position[1],row.position[2]);
      const JPH::Vec3 velocity(row.velocity[0],row.velocity[1],row.velocity[2]);
      if(replace[i]){
        shapes_[i]=new JPH::BoxShape(JPH::Vec3::sReplicate(row.extent*.5f),.005f);
        const auto motion=row.motion==Motion::Static?JPH::EMotionType::Static:row.motion==Motion::Kinematic?JPH::EMotionType::Kinematic:JPH::EMotionType::Dynamic;
        JPH::BodyCreationSettings creation(shapes_[i],position,JPH::Quat::sIdentity(),motion,row.motion==Motion::Static?0:1);
        creation.mLinearVelocity=velocity;creation.mLinearDamping=creation.mAngularDamping=0;
        creation.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;creation.mMassPropertiesOverride.mMass=1;
        auto* body=api.CreateBody(creation);require(body!=nullptr,"preflighted deferred body capacity");ids_[i]=body->GetID();added[added_count++]=ids_[i];
        for(const auto& old_id:before_ids)require(old_id.IsInvalid()||old_id!=ids_[i],"replacement ID cannot alias previous cache generation");
      }else{
        api.SetPosition(ids_[i],position,JPH::EActivation::DontActivate);
        if(row.motion!=Motion::Static)api.SetLinearVelocity(ids_[i],velocity);
      }
    }
    if(added_count){auto state=api.AddBodiesPrepare(added.data(),int(added_count));api.AddBodiesFinalize(added.data(),int(added_count),state,JPH::EActivation::DontActivate);}
    for(unsigned i=0;i<next.size();++i)if(next[i].present&&next[i].motion==Motion::Dynamic)api.ActivateBody(ids_[i]);
    maintenance();
    // Reachability excludes every cache-invalidation writer: geometry replaces
    // body identity; no mutable-shape, manifold-setting or explicit invalidation
    // API is exposed. Positive idle updates therefore have no list to drain.
    for(unsigned i=0;i<next.size();++i)if(next[i].present){JPH::BodyLockRead lock(world_.GetBodyLockInterface(),ids_[i]);require(lock.Succeeded()&&!lock.GetBody().IsCollisionCacheInvalid(),"deferred path never sets cache invalidation flags");}
    if(world_.GetNumActiveBodies(JPH::EBodyType::RigidBody)==0)++idle_updates;
    const auto error=world_.Update(Profile::solver_tick_seconds,1,&scratch,&jobs);++native_updates;
    require(scratch.top==0,"deferred update scratch quiescence");
    if(error!=JPH::EPhysicsUpdateError::None||reject_publication){phase_=Phase::Failed;pending_count_=0;return Outcome::Failed;}
    for(unsigned i=0;i<next.size();++i)if(next[i].present){const auto p=api.GetPosition(ids_[i]);const auto v=api.GetLinearVelocity(ids_[i]);
      next[i].position={p.GetX(),p.GetY(),p.GetZ()};next[i].velocity={v.GetX(),v.GetY(),v.GetZ()};
      for(unsigned axis=0;axis<3;++axis)if(!std::isfinite(next[i].position[axis])||!std::isfinite(next[i].velocity[axis])){phase_=Phase::Failed;pending_count_=0;return Outcome::Failed;}
    }
    published_.rows=next;published_.order_count=pending_count_;
    for(unsigned i=0;i<pending_count_;++i)published_.order[i]=pending_[i].kind;
    ++published_.stamp.generation;++published_.stamp.tick;pending_count_=0;phase_=Phase::Idle;return Outcome::Ready;
  }
  Outcome query(unsigned& hits,bool attempt_reentry=false){
    if(!permitted())return phase_==Phase::Failed||phase_==Phase::Stopped?Outcome::Failed:Outcome::Busy;
    struct Collector final:JPH::CollidePointCollector {
      Session& owner;bool reenter;unsigned count{};Collector(Session& s,bool r):owner(s),reenter(r){}
      void AddHit(const JPH::CollidePointResult&)override{++count;if(reenter){const auto requests=sdk_arena->stats.requests;
        require(owner.enqueue(owner.stamp(),{Kind::Erase,0})==Outcome::Busy&&owner.cancel(owner.stamp())==Outcome::Busy,"deferred query owns immutable SDK generation");
        require(requests==sdk_arena->stats.requests,"query rejection has no SDK allocation");++owner.query_rejections;}}
    } collector(*this,attempt_reentry);
    phase_=Phase::Query;world_.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),collector);phase_=Phase::Idle;hits=collector.count;return Outcome::Ready;
  }
  std::uint32_t sdk_id(unsigned slot)const{require(permitted(),"SDK identity inspection requires quiescence");return ids_[slot].GetIndexAndSequenceNumber();}
  bool contact(unsigned a,unsigned b)const{require(permitted(),"contact inspection requires quiescence");return world_.WereBodiesInContact(ids_[a],ids_[b]);}
};

void run(Scratch& scratch){
  const auto crt_before=crt_requests.load();initialize_primitives();
  unsigned total_updates=0,idle_updates=0,reused=0;std::size_t stable=0;
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);prewarm_jobs(jobs);Session owner(7);auto stamp=owner.published().stamp;
    const auto empty_requests=sdk_arena->stats.requests;
    require(owner.enqueue(stamp,{Kind::Erase,0})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Invalid&&owner.native_updates==0&&sdk_arena->stats.requests==empty_requests,"invalid complete batch rejected before SDK mutation");
    require(owner.cancel(stamp)==Outcome::Ready,"cancel invalid pending batch");
    require(owner.enqueue(stamp,{Kind::Create,0})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"initial deferred publication");
    stamp=owner.published().stamp;const auto requests=sdk_arena->stats.requests;
    for(unsigned i=0;i<4096;++i)require(owner.enqueue(stamp,{Kind::Position,0,0,1,1.+double(i%3)})==Outcome::Ready,"replaceable paused assignment");
    require(owner.pending()==1&&owner.coalesced==4095&&owner.native_updates==1&&requests==sdk_arena->stats.requests,"paused input bounded without SDK mutation or time advance");
    unsigned hits=0;require(owner.query(hits,true)==Outcome::Ready&&hits==1&&owner.published().stamp==stamp,"paused query retains last published generation");
    require(owner.cancel({8,stamp.generation,stamp.tick})==Outcome::Stale&&owner.pending()==1,"stale cancellation cannot discard current pending work");
    require(owner.advance(scratch,jobs)==Outcome::Ready&&owner.published().rows[0].position[0]==1.&&owner.published().stamp.tick==stamp.tick+1,"one explicit tick atomically publishes coalesced state");
    stamp=owner.published().stamp;
    require(owner.enqueue(stamp,{Kind::Create,10,0,1,0,Motion::Dynamic})==Outcome::Ready,"ordered create");
    require(owner.enqueue(stamp,{Kind::Impulse,10,0,1,.01})==Outcome::Ready,"ordered first impulse");
    require(owner.enqueue(stamp,{Kind::Erase,10})==Outcome::Ready,"ordered erase");
    require(owner.enqueue(stamp,{Kind::Create,10,0,2,0,Motion::Dynamic})==Outcome::Ready,"ordered new lifetime");
    require(owner.enqueue(stamp,{Kind::Impulse,10,0,2,.02})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"ordered recreated impulse");
    const auto& published=owner.published();
    require(published.order_count==5&&published.order[0]==Kind::Create&&published.order[1]==Kind::Impulse&&published.order[2]==Kind::Erase&&published.order[3]==Kind::Create&&published.order[4]==Kind::Impulse,"ordered commands preserved by owner replay");
    require(published.rows[10].identity==2&&std::abs(published.rows[10].velocity[0]-.02f)<1e-6f,"deleted lifetime impulse cannot leak to recreation");
    stamp=published.stamp;
    for(unsigned i=0;i<256;++i)require(owner.enqueue(stamp,{Kind::Impulse,10,0,2,0})==Outcome::Ready,"ordered queue capacity");
    require(owner.enqueue(stamp,{Kind::Impulse,10,0,2,0})==Outcome::Capacity&&owner.pending()==256,"visible backpressure preserves accepted queue");
    require(owner.cancel(stamp)==Outcome::Ready&&owner.enqueue(stamp,{Kind::Erase,10,0,2})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"cancel pending commands and retire moving layer");
    // Thereafter all positive-dt updates take the real no-active-body path.
    // Geometry replacement occurs once per final body state, never SetShape.
    const auto original_id=owner.sdk_id(0);
    for(unsigned tick=0;tick<512;++tick){stamp=owner.published().stamp;
      require(owner.enqueue(stamp,{Kind::Extent,0,0,1,tick%2?.1:.2})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"idle immutable replacement without invalidation ceiling");
      if(owner.sdk_id(0)==original_id)++reused;
      if(tick==0)stable=sdk_arena->stats.live_bytes;else require(sdk_arena->stats.live_bytes==stable,"idle replacement retains bounded storage across ID wrap");
    }
    require(reused>=2&&owner.idle_updates>=512&&owner.queue_high_water==256&&owner.query_rejections>0,"idle ID reuse and queue/query coverage");
    total_updates=owner.native_updates;idle_updates=owner.idle_updates;
  }
  // Old queued commands carry a session stamp and cannot enter a new owner.
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);prewarm_jobs(jobs);Session owner(8);const auto initial=owner.published().stamp;
    require(owner.enqueue({7,0,0},{Kind::Create,0})==Outcome::Stale&&owner.pending()==0,"old-session command rejected");
    require(owner.enqueue(initial,{Kind::Create,0})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"failure fixture initial state");
    const auto stable_stamp=owner.published().stamp;const auto old_x=owner.published().rows[0].position[0];
    require(owner.enqueue(stable_stamp,{Kind::Position,0,0,1,2.})==Outcome::Ready&&owner.advance(scratch,jobs,true)==Outcome::Failed,"publication failure is terminal");
    unsigned hits=0;require(owner.published().stamp==stable_stamp&&owner.published().rows[0].position[0]==old_x&&owner.query(hits)==Outcome::Failed&&owner.advance(scratch,jobs)==Outcome::Failed,"last published state retained and mutated SDK unavailable");
  }
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);prewarm_jobs(jobs);Session owner(9);const auto initial=owner.published().stamp;
    require(owner.enqueue(initial,{Kind::Create,0})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"pre-solve failure initial body");
    const auto before=owner.published().stamp;
    require(owner.enqueue(before,{Kind::Extent,0,0,1,.2})==Outcome::Ready&&owner.advance(scratch,jobs,false,true)==Outcome::Failed,"injected failure after old body retirement");
    require(owner.published().stamp==before&&owner.published().rows[0].present&&owner.published().rows[0].extent==.1f&&owner.native_updates==1,"pre-solve failure retains published body without another time advance");
  }
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);prewarm_jobs(jobs);Session owner(10);
    auto stamp=owner.published().stamp;
    for(unsigned i=0;i<256;++i)require(owner.enqueue(stamp,{Kind::Create,i,0,1,0,i<128?Motion::Static:Motion::Kinematic})==Outcome::Ready,"full two-layer pending batch");
    require(owner.advance(scratch,jobs)==Outcome::Ready,"full two-layer publication");
    std::size_t retained=0;
    for(unsigned tick=0;tick<32;++tick){stamp=owner.published().stamp;
      for(unsigned i=0;i<256;++i)require(owner.enqueue(stamp,{Kind::Extent,i,0,1,tick%2?.1:.2})==Outcome::Ready,"full two-layer immutable replacement");
      require(owner.advance(scratch,jobs)==Outcome::Ready,"both empty layers reclaimed before replacement publication");
      if(tick==0)retained=sdk_arena->stats.live_bytes;else require(sdk_arena->stats.live_bytes==retained,"full-batch retained storage stable");
    }
    require(owner.idle_updates==33,"full batches use positive no-active SDK path");
  }
  {
    JPH::JobSystemSingleThreaded jobs(Profile::jobs);prewarm_jobs(jobs);Session owner(11);auto stamp=owner.published().stamp;
    require(owner.enqueue(stamp,{Kind::Create,0})==Outcome::Ready&&owner.enqueue(stamp,{Kind::Create,1,0,1,0,Motion::Dynamic})==Outcome::Ready,"contact-cache seed bodies");
    require(owner.enqueue(stamp,{Kind::Position,1,0,1,0})==Outcome::Ready&&owner.enqueue(stamp,{Kind::Position,1,1,1,.1})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"contact-cache seed step");
    require(owner.contact(0,1),"real old contact cache populated");const auto cached_id=owner.sdk_id(1);
    stamp=owner.published().stamp;
    require(owner.enqueue(stamp,{Kind::Erase,1})==Outcome::Ready&&owner.enqueue(stamp,{Kind::Create,1,0,2,0,Motion::Static})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"positive idle step retires old contacts");
    require(!owner.contact(0,1),"no stale contact published after static replacement");
    unsigned reused_contact_id=0;
    for(unsigned tick=0;tick<256;++tick){stamp=owner.published().stamp;
      require(owner.enqueue(stamp,{Kind::Extent,1,0,2,tick%2?.1:.2})==Outcome::Ready&&owner.advance(scratch,jobs)==Outcome::Ready,"idle contact-key reuse cycle");
      if(owner.sdk_id(1)==cached_id)++reused_contact_id;
      require(!owner.contact(0,1),"reused SDK body ID cannot resurrect retired contact cache");
    }
    require(reused_contact_id==1,"old cached contact key crossed actual SDK generation wrap");
  }
  require(prewarmed_job_pages==5,"all deferred job pages prewarmed before session construction");
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"deferred owner complete SDK teardown");
  std::printf("{\"deferred_edits_passed\":true,\"prewarmed_job_pages\":5,\"qualification_only\":true,\"paused_assignments\":4096,\"coalesced_assignments\":4095,\"queue_capacity\":256,\"ordered_operations\":5,\"idle_replacement_ticks\":512,\"full_two_layer_replacements\":32,\"retired_contact_id_wraps\":1,\"sdk_id_reuses\":%u,\"native_updates\":%u,\"idle_updates\":%u,\"session_stack_bytes\":%zu,\"command_bytes\":%zu,\"published_bytes\":%zu,\"idle_retained_bytes\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",reused,total_updates,idle_updates,sizeof(Session),sizeof(Command),sizeof(Published),stable);
}
} // namespace deferred_probe
