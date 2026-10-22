// Disposable child probe: measurements corroborate, but cannot replace, the
// complete allocator/fatal-path proof. No editor, Runtime or Scene is linked.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/STLLocalAllocator.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/PhysicsUpdateContext.h>
#include <Jolt/Physics/IslandBuilder.h>
#include <Jolt/Physics/LargeIslandSplitter.h>
#include <Jolt/Physics/Constraints/CalculateSolverSteps.h>
#include <Jolt/Physics/Constraints/ContactConstraintManager.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseQuadTree.h>
#include <Jolt/Physics/Collision/CollisionDispatch.h>
#include <Jolt/Physics/Collision/PhysicsMaterialSimple.h>
#include <Jolt/Physics/Collision/NarrowPhaseQuery.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/CollidePointResult.h>
#include <Jolt/Physics/Collision/CollisionCollector.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Geometry/EPAConvexHullBuilder.h>
#include "probe_arena.hpp"
#include "race/physics/profile.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <new>
#include <type_traits>

namespace {
std::atomic<std::size_t> crt_requests{},crt_live{},crt_peak{};
struct NewHeader {void* base;std::size_t bytes;};
[[noreturn]] void rejected(const char* reason) noexcept {std::fprintf(stderr,"RACE_PROBE_REJECT: %s\n",reason);std::fflush(stderr);std::abort();}
void require(bool passed,const char* reason) noexcept {if(!passed)rejected(reason);}
void* counted_new(std::size_t bytes,std::size_t alignment) {
  alignment=std::max(alignment,alignof(std::max_align_t));bytes=std::max(bytes,std::size_t{1});
  if(bytes>std::numeric_limits<std::size_t>::max()-alignment-sizeof(NewHeader))rejected("CRT allocation overflow");
  void* base=_aligned_malloc(bytes+alignment+sizeof(NewHeader),alignment);if(!base)rejected("CRT allocation failed");
  const auto value=(reinterpret_cast<std::uintptr_t>(base)+sizeof(NewHeader)+alignment-1)&~(alignment-1);
  NewHeader header{base,bytes};std::memcpy(reinterpret_cast<void*>(value-sizeof(header)),&header,sizeof(header));
  ++crt_requests;const auto live=crt_live.fetch_add(bytes)+bytes;
  auto peak=crt_peak.load();while(peak<live&&!crt_peak.compare_exchange_weak(peak,live)){}
  return reinterpret_cast<void*>(value);
}
void counted_delete(void* p) noexcept {
  if(!p)return;NewHeader header{};std::memcpy(&header,static_cast<std::byte*>(p)-sizeof(header),sizeof(header));
  crt_live.fetch_sub(header.bytes);_aligned_free(header.base);
}
qualification::Arena* sdk_arena{};
struct AllocationTrace {
  struct Entry {void* pointer;std::size_t requested,alignment,charged;bool live;};
  std::array<Entry,32> entries{};std::size_t count{},frees{};
  void add(void* p,std::size_t requested,std::size_t alignment,std::size_t charged){
    require(count<entries.size(),"allocation trace capacity");entries[count++]={p,requested,alignment,charged,true};
  }
  void remove(void* p){
    for(std::size_t i=count;i>0;--i)if(entries[i-1].pointer==p&&entries[i-1].live){entries[i-1].live=false;++frees;return;}
    rejected("unmatched allocation trace free");
  }
};
AllocationTrace* allocation_trace{};
void* aligned_allocate(std::size_t size,std::size_t alignment){
  const auto before=sdk_arena->stats.live_bytes;
  auto* p=sdk_arena->allocate(size,alignment);if(!p)rejected("SDK arena exhausted");
  if(allocation_trace)allocation_trace->add(p,size,alignment,sdk_arena->stats.live_bytes-before);
  return p;
}
void* allocate(std::size_t size){return aligned_allocate(size,16);}
void release(void* p){
  if(!sdk_arena->release(p))rejected("SDK invalid free");
  if(p&&allocation_trace)allocation_trace->remove(p);
}
void* reallocate(void* p,std::size_t old_size,std::size_t new_size){
  void* next=allocate(new_size);if(p){std::memcpy(next,p,std::min(old_size,new_size));release(p);}return next;
}
void trace(const char* format,...){std::va_list args;va_start(args,format);std::vfprintf(stderr,format,args);va_end(args);std::fputc('\n',stderr);std::fflush(stderr);}
#ifdef JPH_ENABLE_ASSERTS
bool assertion(const char* expression,const char* message,const char* file,JPH::uint line){
  std::fprintf(stderr,"Jolt assertion %s:%u: %s (%s)\n",file,line,expression,message?message:"");rejected("SDK assertion");
}
#endif
constexpr std::size_t heap_bytes=42U*1024U*1024U,scratch_bytes=16U*1024U*1024U;
// Approved 42/16/6 partition: the final 6 MiB includes the separately reserved
// 1 MiB PE stack. Concrete service storage must fit in the other 5 MiB.
constexpr std::size_t control_storage_bytes=5U*1024U*1024U,stack_reservation_bytes=1024U*1024U;
constexpr std::size_t backing_bytes=heap_bytes+scratch_bytes+control_storage_bytes;
static_assert(backing_bytes+stack_reservation_bytes==race::physics::PrimitiveProfile::service_bytes);
class Scratch final : public JPH::TempAllocator {
  std::byte* base_;
public:
  std::size_t top{},peak{},requests{};
  bool capture{};
  std::array<std::uint32_t,32> captured{};
  std::size_t captured_count{};
  explicit Scratch(std::byte* base) noexcept : base_(base){}
  void* Allocate(JPH::uint bytes) override {
    if(!bytes)return nullptr;const auto rounded=(std::size_t(bytes)+31)&~std::size_t{31};
    if(capture){require(captured_count<captured.size(),"scratch request capture capacity");captured[captured_count++]=bytes;}
    if(rounded>scratch_bytes-top)rejected("scratch exhausted");
    void* p=base_+top;top+=rounded;peak=std::max(peak,top);++requests;return p;
  }
  void Free(void* p,JPH::uint bytes) override {
    if(!p){require(bytes==0,"null scratch free");return;}
    const auto rounded=(std::size_t(bytes)+31)&~std::size_t{31};
    require(rounded<=top,"scratch underflow");top-=rounded;require(p==base_+top,"scratch LIFO");
  }
};
struct Layers final : JPH::BroadPhaseLayerInterface {
  JPH::uint GetNumBroadPhaseLayers() const override{return 2;}
  JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override{return JPH::BroadPhaseLayer(static_cast<JPH::BroadPhaseLayer::Type>(layer));}
};
struct ObjectBroadFilter final : JPH::ObjectVsBroadPhaseLayerFilter {
  bool ShouldCollide(JPH::ObjectLayer a,JPH::BroadPhaseLayer b) const override{return a!=0||b.GetValue()!=0;}
};
struct PairFilter final : JPH::ObjectLayerPairFilter {
  bool ShouldCollide(JPH::ObjectLayer a,JPH::ObjectLayer b) const override{return a!=0||b!=0;}
};
void initialize_primitives() {
  JPH::CollisionDispatch::sInit();JPH::ConvexShape::sRegister();JPH::SphereShape::sRegister();JPH::BoxShape::sRegister();
  JPH::PhysicsMaterial::sDefault=new JPH::PhysicsMaterialSimple("RACE qualification",JPH::Color::sGrey);
}
struct QueryHit {std::uint64_t identity{};float fraction{};};
using QueryStorage=std::array<std::array<QueryHit,race::physics::PrimitiveProfile::hits_per_query>,race::physics::PrimitiveProfile::queries>;
struct JobStorageLayout : JPH::JobSystem {
  static constexpr std::size_t slot_bytes=JPH::FixedSizeFreeList<Job>::ObjectStorageSize;
};
unsigned prewarmed_job_pages{};
void prewarm_jobs(JPH::JobSystemSingleThreaded& jobs){
  // The large lazy job page must be acquired before mutable session histories.
  // A successful warm job leaves the fixed page owned by jobs, with its slot free.
  require(allocation_trace==nullptr,"job prewarm requires exclusive allocator trace");
  const auto crt_before=crt_requests.load(),blocks=sdk_arena->stats.live_blocks;
  AllocationTrace trace;allocation_trace=&trace;
  {auto warm=jobs.CreateJob("startup warm",JPH::Color::sWhite,[]{});require(warm.IsDone(),"startup job completes synchronously");}
  allocation_trace=nullptr;
  require(trace.count==1&&trace.frees==0&&trace.entries[0].live&&
          trace.entries[0].requested==2048*JobStorageLayout::slot_bytes&&
          sdk_arena->stats.live_blocks==blocks+1&&crt_requests.load()==crt_before,"exact retained startup job page without CRT storage");
  ++prewarmed_job_pages;
}
template<class Function> void allocation_site(const char* name,std::size_t expected,Function exercise){
  require(allocation_trace==nullptr,"non-nested allocation trace");
  AllocationTrace trace;const auto live=sdk_arena->stats.live_bytes;
  allocation_trace=&trace;exercise(trace);allocation_trace=nullptr;
  require(trace.count==expected&&trace.frees==expected,"allocation site count and lifetime");
  require(sdk_arena->stats.live_bytes==live,"allocation site retained storage");
  std::printf("\"%s\":[",name);
  for(std::size_t i=0;i<trace.count;++i){
    const auto& e=trace.entries[i];require(!e.live,"allocation site quiescence");
    require(e.charged<=((e.requested+15)&~std::size_t{15})+272,"arena per-block bound");
    std::printf("%s{\"requested\":%zu,\"alignment\":%zu,\"charged\":%zu}",i?",":"",e.requested,e.alignment,e.charged);
  }
  std::printf("],\n");
}
std::size_t arena_charge_cases(){
  // Exhaust the offset/alignment/count residues and both sides of the split
  // threshold. The source algebra generalizes the count's 16-byte quotient;
  // these cases alone do not prove freedom from external fragmentation.
  alignas(128) std::array<std::byte,1024> backing{};std::size_t cases=0;
  require(qualification::Arena::block_bytes==64&&qualification::Arena::block_alignment==16,"arena bookkeeping ABI");
  for(std::size_t residue=0;residue<128;residue+=16)
    for(std::size_t alignment=16;alignment<=128;alignment*=2)
      for(std::size_t count=1;count<=16;++count)
        for(std::size_t tail=0;tail<=96;tail+=16){
          const auto offset=128+residue;
          const auto prefix=((offset+64+8+alignment-1)&~(alignment-1))-offset;
          const auto needed=prefix+16;
          qualification::Arena arena(std::span(backing).first(offset+needed+tail));
          auto* anchor=arena.allocate(offset-80,16);require(anchor!=nullptr,"arena residue anchor");
          const auto before=arena.stats.live_bytes;require(before==offset,"arena block residue");
          auto* p=arena.allocate(count,alignment);require(p!=nullptr,"arena charge boundary allocation");
          require(reinterpret_cast<std::uintptr_t>(p)%alignment==0,"arena charge alignment");
          const auto charged=arena.stats.live_bytes-before;
          require(charged==needed+(tail<96?tail:0)&&charged<=16+272,"arena split-tail charge");
          require(arena.release(p)&&arena.release(anchor)&&arena.stats.live_bytes==0,"arena boundary teardown");
          // After coalescence, the whole backing range must be reusable.
          auto* whole=arena.allocate(offset+needed+tail-80,16);
          require(whole!=nullptr&&arena.stats.live_blocks==1,"arena coalesced range");
          require(arena.release(whole)&&arena.stats.live_bytes==0,"arena coalesced teardown");++cases;
        }
  return cases;
}
void storage_bounds(){
  const auto before=crt_requests.load();
  const auto charge_cases=arena_charge_cases();
  initialize_primitives();
  std::printf("{\"schema\":1,\"sites\":{\n");
  allocation_site("contact_cache",6,[](AllocationTrace& trace){
    JPH::PhysicsSettings settings;JPH::ContactConstraintManager manager(settings);manager.Init(32768,32768);
    require(trace.count==6,"contact cache initialization requests");
    for(std::size_t i=0;i<2;++i){
      require(trace.entries[i*3].requested==6684672,"contact cache pool ABI");
      require(trace.entries[i*3+1].requested==131072&&trace.entries[i*3+2].requested==131072,"contact cache bucket ABI");
    }
    manager.FinalizeContactCacheAndCallContactPointRemovedCallbacks(0,0);
    manager.FinalizeContactCacheAndCallContactPointRemovedCallbacks(0,0);
    require(trace.count==6,"contact cache swap retains fixed storage");
  });
  Layers layers;
  allocation_site("body_manager",5,[&](AllocationTrace& trace){
    JPH::BodyManager manager;manager.Init(256,1,layers);
    require(trace.count==5,"body manager initialization requests");
    require(trace.entries[1].requested==256*sizeof(JPH::Body*)&&trace.entries[2].requested==256*sizeof(JPH::BodyID)&&
      trace.entries[3].requested==256*sizeof(JPH::BodyID)&&trace.entries[4].requested==256,"body manager fixed arrays");
  });
  allocation_site("island_links",1,[](AllocationTrace& trace){
    JPH::IslandBuilder islands;islands.Init(256);
    require(trace.count==1&&trace.entries[0].requested==2048,"island body links ABI");
  });
  allocation_site("primitive_shapes",2,[](AllocationTrace& trace){
    JPH::RefConst<JPH::Shape> box=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f));
    JPH::RefConst<JPH::Shape> sphere=new JPH::SphereShape(.5f);
    require(trace.count==2&&trace.entries[0].requested==sizeof(JPH::BoxShape)&&trace.entries[1].requested==sizeof(JPH::SphereShape),"direct primitive shape storage");
  });
  {
    JPH::BodyManager manager;manager.Init(256,1,layers);
    JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f));
    allocation_site("rigid_body_variants",4,[&](AllocationTrace& trace){
      std::array<JPH::Body*,4> bodies{};
      constexpr std::array types{JPH::EMotionType::Static,JPH::EMotionType::Kinematic,JPH::EMotionType::Dynamic,JPH::EMotionType::Static};
      for(std::size_t i=0;i<bodies.size();++i){
        JPH::BodyCreationSettings settings(shape,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),types[i],0);
        settings.mAllowDynamicOrKinematic=i==3;
        bodies[i]=manager.AllocateBody(settings);require(bodies[i]!=nullptr,"rigid body layout allocation");
      }
      require(trace.count==4&&trace.entries[0].requested==sizeof(JPH::Body),"static body ABI");
      require(trace.entries[1].requested==trace.entries[2].requested&&trace.entries[2].requested==trace.entries[3].requested&&
        trace.entries[1].requested>=sizeof(JPH::Body)+sizeof(JPH::MotionProperties),"body motion allocation ABI");
      for(auto* body:bodies)manager.FreeBody(body);
    });
  }
  allocation_site("job_slots",2,[](AllocationTrace& trace){
    JPH::JobSystemSingleThreaded jobs(race::physics::PrimitiveProfile::jobs);unsigned executed=0;
    for(unsigned cycle=0;cycle<2;++cycle){
      std::array<JPH::JobSystem::JobHandle,race::physics::PrimitiveProfile::jobs> handles{};
      for(auto& handle:handles)handle=jobs.CreateJob("storage",JPH::Color::sWhite,[&executed]{++executed;});
      require(trace.count==2,"fixed job page reused at full capacity");
    }
    require(executed==4096,"job storage lifecycle completed");
    require(trace.entries[0].requested==sizeof(void*)&&trace.entries[1].requested==2048*JobStorageLayout::slot_bytes,"job page ABI");
  });
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==before,"storage fixture complete teardown");
  std::printf("\"allocation_sites_complete\":false},\n");
  std::printf("\"query_hit_bytes\":%zu,\"query_hit_alignment\":%zu,\"query_storage_bytes\":%zu,\"query_storage_location\":\"stack\",\n",sizeof(QueryHit),alignof(QueryHit),sizeof(QueryStorage));
  std::printf("\"arena_object_bytes\":%zu,\"arena_block_bytes\":%zu,\"arena_block_alignment\":%zu,\"job_slot_bytes\":%zu,\n",sizeof(qualification::Arena),qualification::Arena::block_bytes,qualification::Arena::block_alignment,JobStorageLayout::slot_bytes);
  std::printf("\"arena_charge_cases\":%zu,\"per_block_round16_overhead_bound\":272,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"storage_bounds_passed\":true,\"allocation_proved\":false}\n",charge_cases);
}
#include "probe_lifetimes.hpp"
template<class Base> class QueryCollector final : public Base {
  std::span<QueryHit> output_;
  std::uint64_t identity_{};
public:
  std::size_t count{};bool overflow{};
  explicit QueryCollector(std::span<QueryHit> output) : output_(output){}
  void OnBody(const JPH::Body& body) override {identity_=body.GetUserData();}
  void AddHit(const typename Base::ResultType& hit) override {
    if(count==output_.size()){overflow=true;this->ForceEarlyOut();return;}
    float fraction=0;
    if constexpr(std::is_same_v<typename Base::ResultType,JPH::RayCastResult>)fraction=hit.mFraction;
    output_[count++]={identity_,fraction};
  }
};
struct EvenQueryFilter final : JPH::BodyFilter {
  bool ShouldCollideLocked(const JPH::Body& body) const override {return body.GetUserData()%2==0;}
};
void scratch_envelope(Scratch& scratch) {
  // Deliberately synthetic island links exercise all allocation maxima together.
  // This is an allocation fixture, not a physically realizable contact world.
  // Public SDK internal methods reveal the private contact-record size through
  // their real allocation request without changing access specifiers or source.
  require(scratch.top==0&&scratch.requests==0,"fresh envelope scratch");scratch.capture=true;
  constexpr std::uint32_t active=128,contacts=32768;
  const auto before=crt_requests.load();
  initialize_primitives();
  {
    // Compose the SDK internals explicitly so the splitter receives a real body
    // manager without exposing PhysicsSystem's private members. The default
    // PhysicsSystem below supplies only gravity during contact-record setup.
    Layers layers;JPH::BodyManager body_manager;body_manager.Init(256,1,layers);
    JPH::BroadPhaseQuadTree broad_phase;broad_phase.Init(&body_manager,layers);
    JPH::BodyLockInterfaceNoLock body_lock(body_manager);JPH::BodyInterface bodies;
    bodies.Init(body_lock,body_manager,broad_phase);
    JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f));
    std::array<JPH::BodyID,active+1> ids{};
    for(std::uint32_t i=0;i<ids.size();++i){
      JPH::BodyCreationSettings creation(shape,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),
        i<active?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,i<active?1:0);
      auto* body=bodies.CreateBody(creation);require(body!=nullptr,"envelope body creation");ids[i]=body->GetID();
    }
    auto batch=ids;auto prepared=bodies.AddBodiesPrepare(batch.data(),int(batch.size()));
    bodies.AddBodiesFinalize(batch.data(),int(batch.size()),prepared,JPH::EActivation::Activate);
    require(body_manager.GetNumActiveBodies(JPH::EBodyType::RigidBody)==active,"envelope active bodies");
    JPH::PhysicsSystem gravity_source;
    JPH::PhysicsSettings settings;settings.mNumVelocitySteps=10;settings.mNumPositionSteps=2;
    JPH::ContactConstraintManager manager(settings);
    manager.Init(32768,contacts);
    JPH::IslandBuilder islands;islands.Init(256);JPH::LargeIslandSplitter splitter;
    JPH::PhysicsUpdateContext context(scratch);context.mSteps.resize(1);
    context.mPhysicsSystem=&gravity_source;context.mBodyManager=&body_manager;
    context.mIslandBuilder=&islands;context.mStepDeltaTime=1.f/60.f;
    context.mBodyPairs=static_cast<JPH::BodyPair*>(scratch.Allocate(sizeof(JPH::BodyPair)*1024));
    manager.PrepareConstraintBuffer(&context);
    islands.PrepareContactConstraints(contacts,&scratch);islands.PrepareNonContactConstraints(0,&scratch);
    auto allocator=manager.GetContactAllocator();
    JPH::ContactManifold manifold{};manifold.mBaseOffset=JPH::RVec3::sZero();
    manifold.mWorldSpaceNormal=JPH::Vec3::sAxisY();manifold.mPenetrationDepth=0;
    manifold.mRelativeContactPointsOn1.push_back(JPH::Vec3::sZero());
    manifold.mRelativeContactPointsOn2.push_back(JPH::Vec3::sZero());
    auto& fixed=body_manager.GetBody(ids[active]);
    for(std::uint32_t i=0;i<active;++i){
      auto& moving=body_manager.GetBody(ids[i]);
      auto pair=manager.AddBodyPair(allocator,moving,fixed);require(pair!=nullptr,"envelope contact pair");
      bool activate_and_link=true;
      manager.AddContactConstraint(allocator,activate_and_link,pair,moving,fixed,manifold);
    }
    require(manager.GetNumConstraints()==active&&allocator.mErrors==JPH::EPhysicsUpdateError::None,"envelope contact records");
    // Repeat valid contact references to maximize storage without manufacturing
    // invalid SDK records or pretending these are physically distinct contacts.
    for(std::uint32_t i=active;i<contacts;++i)
      islands.LinkContact(i,body_manager.GetBody(ids[i%active]).GetIndexInActiveBodiesInternal());
    islands.Finalize(body_manager.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody),active,contacts,&scratch);
    require(islands.GetNumIslands()==active,"maximum separate island count");
    splitter.Prepare(islands,active,&scratch);
    auto* ccd_map=scratch.Allocate(active*sizeof(int)); // Allocated even with no CCD bodies.
    for(std::uint32_t i=0;i<active;++i){
      JPH::uint32 *begin,*end;islands.GetContactsInIsland(i,begin,end);
      require(end-begin==contacts/active,"envelope contacts per island");
      for(auto* c=begin;c!=end;++c)*c%=active;
      manager.ConstraintIdxToConstraintOffset(begin,end);
      JPH::CalculateSolverSteps steps(settings);
      require(splitter.SplitIsland(i,islands,body_manager,manager,nullptr,steps),"envelope split island");
    }
    // Consume the scheduler lifecycle, not a physical solve. Each synthetic
    // reference must be visited once per iteration before reset and teardown.
    const auto drain=[&](std::uint32_t iterations){
      std::uint32_t visits=0,first_visits=0,finished=0;
      for(;;){
        JPH::uint index;JPH::uint32 *cb,*ce,*pb,*pe;bool first;
        const auto status=splitter.FetchNextBatch(index,cb,ce,pb,pe,first);
        if(status==JPH::LargeIslandSplitter::EStatus::AllBatchesDone)break;
        require(status==JPH::LargeIslandSplitter::EStatus::BatchRetrieved,"envelope scheduler progress");
        require(cb==ce&&pe>pb,"envelope contact-only batch");
        visits+=std::uint32_t(pe-pb);if(first)first_visits+=std::uint32_t(pe-pb);
        require(visits<=contacts*iterations,"envelope iteration bound");
        bool last,final;splitter.MarkBatchProcessed(index,cb,ce,pb,pe,last,final);
        if(last&&final)++finished;
      }
      require(visits==contacts*iterations&&first_visits==contacts&&finished==active,"envelope complete batch lifecycle");
    };
    drain(settings.mNumVelocitySteps+1);
    splitter.PrepareForSolvePositions();drain(settings.mNumPositionSteps);
    // Every nonzero request, in lifetime order, from the pinned configuration.
    // Includes SortIslands' temporary 512 B array, freed before position counts.
    constexpr std::array<std::uint32_t,17> expected{
      7344,8192,15728640,131072,131072,512,516,131072,516,
      512,512,128,512,131072,131072,70656,512};
    require(scratch.captured_count==expected.size(),"exact scratch allocation inventory");
    for(std::size_t i=0;i<scratch.captured_count;++i)require(scratch.captured[i]==expected[i],"scratch allocation layout drift");
    require(scratch.top==16473472&&scratch.peak==16473472,"scratch lifetime envelope");
    scratch.Free(ccd_map,active*sizeof(int));splitter.Reset(&scratch);islands.ResetIslands(&scratch);
    manager.FinalizeContactCacheAndCallContactPointRemovedCallbacks(allocator.mNumBodyPairs,allocator.mNumManifolds);
    manager.FinishConstraintBuffer();scratch.Free(context.mBodyPairs,sizeof(JPH::BodyPair)*1024);context.mBodyPairs=nullptr;
    bodies.RemoveBodies(batch.data(),int(batch.size()));bodies.DestroyBodies(ids.data(),int(ids.size()));
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
  scratch.capture=false;require(scratch.top==0,"envelope teardown scratch");
  require(crt_requests.load()==before,"CRT allocation during scratch envelope");
}
void query_contracts() {
  initialize_primitives();Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
  {
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    JPH::RefConst<JPH::Shape> shape=new JPH::SphereShape(.5f);
    std::array<JPH::BodyID,256> ids{};auto& bodies=world.GetBodyInterface();
    for(unsigned i=0;i<256;++i){
      JPH::BodyCreationSettings settings(shape,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
      settings.mUserData=i+1;auto* body=bodies.CreateBody(settings);require(body!=nullptr,"query body creation");ids[i]=body->GetID();
    }
    auto batch=ids;auto prepared=bodies.AddBodiesPrepare(batch.data(),256);
    bodies.AddBodiesFinalize(batch.data(),256,prepared,JPH::EActivation::DontActivate);world.OptimizeBroadPhase();
    // Own every result simultaneously through all 64 synchronous queries. The
    // fixed collectors copy values, never retain Jolt collector context pointers.
    QueryStorage owned{};
    const auto before=crt_requests.load();
    for(unsigned query=0;query<64;++query){
      std::size_t count{};bool overflow{};
      if(query%2==0){
        QueryCollector<JPH::CastRayCollector> collector(owned[query]);
        world.GetNarrowPhaseQuery().CastRay(JPH::RRayCast(JPH::RVec3(-2,0,0),JPH::Vec3(4,0,0)),JPH::RayCastSettings{},collector);
        count=collector.count;overflow=collector.overflow;
      }else{
        QueryCollector<JPH::CollidePointCollector> collector(owned[query]);
        world.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),collector);count=collector.count;overflow=collector.overflow;
      }
      require(count==256&&!overflow,"full query result capacity");
      std::sort(owned[query].begin(),owned[query].end(),[](const auto& a,const auto& b){return a.identity<b.identity;});
      for(unsigned i=0;i<256;++i){
        require(owned[query][i].identity==i+1,"copied query identity");
        require(std::abs(owned[query][i].fraction-(query%2==0?.375f:0.f))<1e-6f,"analytical query fraction");
      }
    }
    // Separate filter and shortage fixtures. A truncated candidate is explicitly
    // rejected; it is never represented as a complete published query result.
    QueryCollector<JPH::CollidePointCollector> filtered(owned[0]);EvenQueryFilter filter;
    world.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),filtered,{}, {},filter);
    require(filtered.count==128&&!filtered.overflow,"query-only body visibility filter");
    for(std::size_t i=0;i<filtered.count;++i)require(owned[0][i].identity%2==0,"query filter identity");
    QueryCollector<JPH::CollidePointCollector> short_output(std::span(owned[1]).first(255));
    world.GetNarrowPhaseQuery().CollidePoint(JPH::RVec3::sZero(),short_output);
    require(short_output.count==255&&short_output.overflow,"query overflow is observable");
    require(crt_requests.load()==before,"CRT allocation during query capture");
    bodies.RemoveBodies(batch.data(),256);bodies.DestroyBodies(ids.data(),256);
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
}
void adversarial_tree() {
  initialize_primitives();Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
  {
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.05f),.005f);
    std::array<JPH::BodyID,256> ids{};auto& bodies=world.GetBodyInterface();
    for(unsigned i=0;i<256;++i){
      double position[3]{};if(i<108)position[i%3]=512.*std::pow(.49,double(i/3));
      JPH::BodyCreationSettings settings(shape,JPH::RVec3(position[0],position[1],position[2]),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
      auto* body=bodies.CreateBody(settings);require(body!=nullptr,"adversarial body creation");ids[i]=body->GetID();
    }
    // All shapes are 0.1 m boxes, all centres inside +/-512 m. No simulation,
    // individual insertion, invalid dimensions or excess body count is needed.
    std::fputs("RACE_TREE_CASE: 256 valid static boxes; batch preparation\n",stderr);std::fflush(stderr);
    auto batch=ids;auto prepared=bodies.AddBodiesPrepare(batch.data(),256);
    bodies.AddBodiesFinalize(batch.data(),256,prepared,JPH::EActivation::DontActivate);world.OptimizeBroadPhase();
    bodies.RemoveBodies(batch.data(),256);bodies.DestroyBodies(ids.data(),256);
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
}
void arena_fixtures(qualification::Arena& arena) {
  std::array<void*,512> blocks{};
  for(std::size_t i=0;i<blocks.size();++i){blocks[i]=arena.allocate(1+i%384,std::size_t{16}<<(i%4));require(blocks[i]!=nullptr,"arena fixture allocation");require(reinterpret_cast<std::uintptr_t>(blocks[i])%(std::size_t{16}<<(i%4))==0,"arena fixture alignment");std::memset(blocks[i],int(i%251),1+i%384);}
  for(std::size_t i=0;i<blocks.size();++i)for(std::size_t j=0;j<1+i%384;++j)require(static_cast<unsigned char*>(blocks[i])[j]==i%251,"arena allocations overlap");
  for(std::size_t i=0;i<blocks.size();i+=2)require(arena.release(blocks[i]),"arena fixture free");
  for(std::size_t i=0;i<blocks.size();i+=2){blocks[i]=arena.allocate(385,32);require(blocks[i]!=nullptr,"arena fragmented allocation");std::memset(blocks[i],int(i%251),385);}
  for(std::size_t i=0;i<blocks.size();++i)for(std::size_t j=0;j<(i%2?1+i%384:385);++j)require(static_cast<unsigned char*>(blocks[i])[j]==i%251,"fragmented arena data changed");
  for(auto* p:blocks)require(arena.release(p),"arena final free");
  require(arena.stats.live_blocks==0&&arena.stats.live_bytes==0,"arena coalescence");
  require(!arena.release(blocks[0]),"arena duplicate free accepted");
  const auto before=arena.stats.requests;
  require(arena.allocate(std::numeric_limits<std::size_t>::max(),16)==nullptr,"arena oversize accepted");
  require(arena.allocate(16,256)==nullptr,"arena unsupported alignment accepted");
  require(arena.stats.requests==before&&arena.stats.live_bytes==0,"rejected arena allocation changed accounting");
}
struct Workload {
  std::size_t ticks{},steps_with_crt_new{};double position_error{},velocity_error{};
};
Workload exercise(bool contacts,bool dense,JPH::JobSystemSingleThreaded& jobs,Scratch& scratch) {
  Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
  JPH::PhysicsSystem world;
  world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
  JPH::PhysicsSettings settings;
  settings.mMaxInFlightBodyPairs=1024;settings.mNumVelocitySteps=10;settings.mNumPositionSteps=2;
  settings.mPenetrationSlop=.001f;settings.mSpeculativeContactDistance=.005f;settings.mManifoldTolerance=.001f;
  world.SetPhysicsSettings(settings);world.SetGravity(JPH::Vec3(0,-9.80665f,0));
  std::array<JPH::BodyID,256> ids{};std::array<JPH::RefConst<JPH::Shape>,256> shapes{};
  Workload result;
  auto& bodies=world.GetBodyInterface();
  for(unsigned cycle=0;cycle<(contacts?4U:1U);++cycle) {
    for(unsigned i=0;i<256;++i) {
      const bool moving=i<128;
      if(contacts&&i==128)shapes[i]=new JPH::BoxShape(JPH::Vec3(10,.25f,10),.005f);
      else if(dense||(i&1))shapes[i]=new JPH::SphereShape(.5f);
      else shapes[i]=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f),.005f);
      JPH::RVec3 position;
      if(!moving)position=i==128&&contacts?JPH::RVec3(0,-.25,0):JPH::RVec3(100+double(i%16)*2,0,100+double(i/16)*2);
      else if(dense)position=JPH::RVec3(double(i%4)*.05,.5+double((i/4)%4)*.05,double(i/16)*.05);
      else position=JPH::RVec3(double(i%8)*1.1-4,contacts?.5+double(i/64):20.+double(i/64)*2,double((i/8)%8)*1.1-4);
      JPH::BodyCreationSettings body(shapes[i],position,JPH::Quat::sIdentity(),moving?JPH::EMotionType::Dynamic:JPH::EMotionType::Static,static_cast<JPH::ObjectLayer>(moving));
      body.mLinearDamping=body.mAngularDamping=0;body.mAllowSleeping=contacts;
      body.mOverrideMassProperties=JPH::EOverrideMassProperties::CalculateInertia;body.mMassPropertiesOverride.mMass=1;
      auto* created=bodies.CreateBody(body);require(created!=nullptr,"body capacity");ids[i]=created->GetID();
    }
    // Jolt may reorder the batch; retain stable ID order for analytical reads.
    auto batch=ids;auto prepared=bodies.AddBodiesPrepare(batch.data(),256);
    bodies.AddBodiesFinalize(batch.data(),256,prepared,JPH::EActivation::Activate);world.OptimizeBroadPhase();
    float velocity=0;double displacement=0;
    const unsigned ticks=contacts?(dense?32U:600U):8U;
    for(unsigned tick=0;tick<ticks;++tick) {
      const auto before=crt_requests.load();
      auto error=world.Update(race::physics::PrimitiveProfile::solver_tick_seconds,1,&scratch,&jobs);
      if(crt_requests.load()!=before)++result.steps_with_crt_new;
      require(error==JPH::EPhysicsUpdateError::None,"update error mask");require(scratch.top==0,"scratch retained after Update");
      if(!contacts){
        velocity+=-9.80665f*race::physics::PrimitiveProfile::solver_tick_seconds;
        displacement+=double(velocity)*double(race::physics::PrimitiveProfile::solver_tick_seconds);
        for(unsigned i=0;i<128;++i){const auto p=bodies.GetPosition(ids[i]);const auto v=bodies.GetLinearVelocity(ids[i]);result.position_error=std::max(result.position_error,std::abs(p.GetY()-(20.+double(i/64)*2+displacement)));result.velocity_error=std::max(result.velocity_error,std::abs(double(v.GetY())-double(velocity)));}
      }
      ++result.ticks;
    }
    batch=ids;bodies.RemoveBodies(batch.data(),256);bodies.DestroyBodies(ids.data(),256);
    for(auto& shape:shapes)shape=nullptr;
    // Update(0) skips dirty empty layers. The full wrapper path reclaims them
    // after query quiescence without advancing simulation time.
    world.OptimizeBroadPhase();
  }
  return result;
}
#include "probe_mutation_protocol.hpp"
#include "probe_deferred_edits.hpp"
#include "probe_wrapper_storage.hpp"
#include "probe_traversal_storage.hpp"
#include "probe_startup_storage.hpp"
} // namespace

void* operator new(std::size_t bytes){return counted_new(bytes,alignof(std::max_align_t));}
void* operator new[](std::size_t bytes){return ::operator new(bytes);}
void* operator new(std::size_t bytes,std::align_val_t alignment){return counted_new(bytes,static_cast<std::size_t>(alignment));}
void* operator new[](std::size_t bytes,std::align_val_t alignment){return ::operator new(bytes,alignment);}
void operator delete(void* p) noexcept{counted_delete(p);}
void operator delete[](void* p) noexcept{counted_delete(p);}
void operator delete(void* p,std::size_t) noexcept{counted_delete(p);}
void operator delete[](void* p,std::size_t) noexcept{counted_delete(p);}
void operator delete(void* p,std::align_val_t) noexcept{counted_delete(p);}
void operator delete[](void* p,std::align_val_t) noexcept{counted_delete(p);}
void operator delete(void* p,std::size_t,std::align_val_t) noexcept{counted_delete(p);}
void operator delete[](void* p,std::size_t,std::align_val_t) noexcept{counted_delete(p);}

#include "probe_epa_capacity.hpp"
#include "probe_owned_records.hpp"
#include "probe_query_operand.hpp"
#include "probe_contact_ownership.hpp"
#include "probe_trigger_ownership.hpp"
#include "probe_composed_sdk.hpp"
void run_owned_leases_probe();
void run_product_owner_probe();

int main(int argc,char** argv) {
  SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);_set_abort_behavior(0,_WRITE_ABORT_MSG|_CALL_REPORTFAULT);
  if(!JPH::VerifyJoltVersionID())return 65;
  if(argc==2&&std::strcmp(argv[1],"--composed-ownership")==0){
    const auto before=crt_requests.load();run_composed_ownership_probe();
    require(crt_requests.load()==before,"composed ownership has no CRT allocation fallback");return 0;
  }
  if(argc==2&&std::strcmp(argv[1],"--owned-product")==0){
    const auto before=crt_requests.load();run_product_owner_probe();
    require(crt_requests.load()==before,"owned product admission has no CRT allocation");return 0;
  }
  if(argc==2&&std::strcmp(argv[1],"--owned-leases")==0){
    const auto before=crt_requests.load();run_owned_leases_probe();
    require(crt_requests.load()==before,"owned output control blocks have no CRT fallback");return 0;
  }
  auto* backing=static_cast<std::byte*>(VirtualAlloc(nullptr,backing_bytes,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
  if(!backing)return 70;
  qualification::Arena arena({backing,heap_bytes});sdk_arena=&arena;Scratch scratch(backing+heap_bytes);
  JPH::Allocate=allocate;JPH::Reallocate=reallocate;JPH::Free=release;JPH::AlignedAllocate=aligned_allocate;JPH::AlignedFree=release;JPH::Trace=trace;
#ifdef JPH_ENABLE_ASSERTS
  JPH::AssertFailed=assertion;
#endif
  if(argc==2){
    if(std::strcmp(argv[1],"--sdk-trigger-ownership")==0){trigger_owner_probe::run(backing+heap_bytes+scratch_bytes);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-contact-ownership")==0){contact_owner_probe::run(backing+heap_bytes+scratch_bytes,scratch);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-query-operand")==0){query_operand_probe::run(backing+heap_bytes+scratch_bytes);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--owned-records")==0){owned_records_probe::run(backing+heap_bytes+scratch_bytes,control_storage_bytes);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-epa-capacity")==0){epa_capacity_probe::run();require(arena.stats.live_bytes==0,"EPA capacity teardown");VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-scratch-shortage")==0){JPH::TempAllocatorImpl small(128);small.Allocate(256);rejected("shortage did not terminate");}
    if(std::strcmp(argv[1],"--sdk-scratch-order")==0){JPH::TempAllocatorImpl small(128);auto* first=small.Allocate(32);small.Allocate(32);small.Free(first,32);rejected("LIFO fault did not terminate");}
    if(std::strcmp(argv[1],"--arena-shortage")==0){allocate(heap_bytes);rejected("arena fault did not terminate");}
    if(std::strcmp(argv[1],"--job-shortage")==0){JPH::JobSystemSingleThreaded jobs(1);auto a=jobs.CreateJob("a",JPH::Color::sWhite,[]{},1);auto b=jobs.CreateJob("b",JPH::Color::sWhite,[]{},1);(void)a;(void)b;rejected("job fault did not terminate");}
    if(std::strcmp(argv[1],"--sdk-broadphase-depth")==0){adversarial_tree();require(arena.stats.live_bytes==0,"adversarial SDK teardown");std::puts("{\"tree_case_passed\":true}");VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-query-contracts")==0){query_contracts();require(arena.stats.live_bytes==0,"query SDK teardown");std::puts("{\"query_case_passed\":true,\"queries\":64,\"owned_hits\":16384,\"filter_hits\":128,\"overflow_rejected\":true}");VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-storage-bounds")==0){storage_bounds();VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-mutable-lifetimes")==0){lifetime_probe::run();VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-mutation-protocol")==0){protocol_probe::run(scratch);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-deferred-edits")==0){deferred_probe::run(scratch);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-wrapper-storage")==0){wrapper_probe::run(scratch);VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-traversal-storage")==0){traversal_probe::run();VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-startup-storage")==0){startup_probe::run();VirtualFree(backing,0,MEM_RELEASE);return 0;}
    if(std::strcmp(argv[1],"--sdk-paused-empty-shortage")==0){protocol_probe::paused_empty_shortage(scratch);}
    if(std::strcmp(argv[1],"--sdk-tree-churn-shortage")==0){initialize_primitives();lifetime_probe::tree_case(false,false);rejected("tree churn fault did not terminate");}
    if(std::strcmp(argv[1],"--sdk-scratch-envelope")==0){scratch_envelope(scratch);require(arena.stats.live_bytes==0,"envelope SDK teardown");std::puts("{\"scratch_envelope_passed\":true,\"synthetic_allocation_fixture\":true,\"requests\":17,\"peak_bytes\":16473472,\"reservation_bytes\":16777216,\"split_islands\":128,\"synthetic_contact_links\":32768,\"velocity_contact_visits\":360448,\"position_contact_visits\":65536,\"scratch_live_at_shutdown\":0,\"allocation_proved\":false}");VirtualFree(backing,0,MEM_RELEASE);return 0;}
    return 64;
  }
  arena_fixtures(arena);arena.stats={};
  // Primitive dispatch only. ObjectStream and SDK factory serialization are not
  // used; full RegisterTypes would retain function-static RTTI allocations.
  initialize_primitives();
  const auto baseline=arena.stats.live_bytes;
  Workload freefall,stack,dense;
  {
    JPH::JobSystemSingleThreaded jobs(2048);
    prewarm_jobs(jobs);
    freefall=exercise(false,false,jobs,scratch);stack=exercise(true,false,jobs,scratch);dense=exercise(true,true,jobs,scratch);
  }
  require(arena.stats.live_bytes==baseline,"SDK objects outlived isolated worlds");
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(arena.stats.live_blocks==0&&arena.stats.live_bytes==0,"SDK retained allocation at shutdown");
  require(freefall.position_error<=.0001&&freefall.velocity_error<=.0001,"freefall numerical reference");
  require(freefall.steps_with_crt_new+stack.steps_with_crt_new+dense.steps_with_crt_new==0,"CRT new during Update");
  std::printf("{\"schema\":1,\"passed\":true,\"allocation_proved\":false,\"simulation_executed\":true,\"factory_registry_used\":false,\"reserved_bytes\":%zu,\"sdk_heap_peak_bytes\":%zu,\"sdk_heap_peak_blocks\":%zu,\"sdk_heap_requests\":%zu,\"sdk_max_request\":%zu,\"sdk_max_alignment\":%zu,\"sdk_heap_live_at_shutdown\":%zu,\"scratch_peak_bytes\":%zu,\"scratch_requests\":%zu,\"crt_new_requests\":%zu,\"crt_new_peak_bytes\":%zu,\"crt_new_live_bytes\":%zu,\"freefall_ticks\":%zu,\"stack_ticks\":%zu,\"dense_stress_ticks\":%zu,\"freefall_position_error_m\":%.17g,\"freefall_velocity_error_m_s\":%.17g,\"scope\":\"Disposable memory/numerical probe; dense overlap is stress, not admitted motion or production integration\"}\n",
    backing_bytes+stack_reservation_bytes,arena.stats.peak_bytes,arena.stats.peak_blocks,arena.stats.requests,arena.stats.max_request,arena.stats.max_alignment,arena.stats.live_bytes,scratch.peak,scratch.requests,crt_requests.load(),crt_peak.load(),crt_live.load(),freefall.ticks,stack.ticks,dense.ticks,freefall.position_error,freefall.velocity_error);
  JPH::Allocate=nullptr;JPH::Reallocate=nullptr;JPH::Free=nullptr;JPH::AlignedAllocate=nullptr;JPH::AlignedFree=nullptr;sdk_arena=nullptr;
  VirtualFree(backing,0,MEM_RELEASE);return 0;
}
