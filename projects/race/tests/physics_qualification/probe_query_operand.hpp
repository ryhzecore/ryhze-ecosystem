#pragma once
#include <Jolt/Physics/Collision/CollideShape.h>

namespace query_operand_probe {
// A private synchronous operand. Only this method can observe the placement
// shape pointer; no public handle or shared owner can retain it. Recursive
// queries are rejected before construction. Bodies continue to own 256 shapes.
class Operand {
  std::byte* storage_;
  bool active_{};
public:
  explicit Operand(std::byte* storage):storage_(storage){}
  bool collide(const JPH::PhysicsSystem& world,bool box,JPH::CollideShapeCollector& collector) noexcept {
    if(active_)return false;
    active_=true;
    const auto requests=sdk_arena->stats.requests,live=sdk_arena->stats.live_bytes;
    const auto refs=JPH::PhysicsMaterial::sDefault->GetRefCount();
    if(box){
      auto* shape=::new(static_cast<void*>(storage_)) JPH::BoxShape(JPH::Vec3::sReplicate(.5f),.005f,JPH::PhysicsMaterial::sDefault);
      world.GetNarrowPhaseQuery().CollideShape(shape,JPH::Vec3::sReplicate(1),JPH::RMat44::sIdentity(),
                                             JPH::CollideShapeSettings{},JPH::RVec3::sZero(),collector);
      require(shape->GetRefCount()==0,"temporary box acquired no escaped SDK reference");
      shape->~BoxShape();
    }else{
      auto* shape=::new(static_cast<void*>(storage_)) JPH::SphereShape(.5f,JPH::PhysicsMaterial::sDefault);
      world.GetNarrowPhaseQuery().CollideShape(shape,JPH::Vec3::sReplicate(1),JPH::RMat44::sIdentity(),
                                             JPH::CollideShapeSettings{},JPH::RVec3::sZero(),collector);
      require(shape->GetRefCount()==0,"temporary sphere acquired no escaped SDK reference");
      shape->~SphereShape();
    }
    require(JPH::PhysicsMaterial::sDefault->GetRefCount()==refs,"query operand released material reference");
    require(sdk_arena->stats.requests==requests&&sdk_arena->stats.live_bytes==live,"query operand and traversal need no SDK allocation in this workload");
    active_=false;return true;
  }
};
struct Collector final : JPH::CollideShapeCollector {
  std::array<std::uint64_t,256> identities{};
  const JPH::PhysicsSystem& world;
  Operand& operand;
  std::uint64_t identity{};
  std::size_t count{},reentrant_rejections{};
  Collector(const JPH::PhysicsSystem& w,Operand& op):world(w),operand(op){}
  void OnBody(const JPH::Body& body) noexcept override {identity=body.GetUserData();}
  void AddHit(const JPH::CollideShapeResult&) noexcept override {
    require(count<identities.size()&&GetContext()!=nullptr,"bounded temporary operand query capture");
    identities[count++]=identity;
    require(!operand.collide(world,false,*this),"reentrant operand rejected before traversal");
    ++reentrant_rejections;
  }
};
void run(std::byte* storage){
  static_assert(sizeof(JPH::BoxShape)<=512&&sizeof(JPH::SphereShape)<=512);
  static_assert(alignof(JPH::BoxShape)<=16&&alignof(JPH::SphereShape)<=16);
  require(reinterpret_cast<std::uintptr_t>(storage)%16==0,"temporary query storage aligned");
  const auto before=crt_requests.load();
  initialize_primitives();
  std::size_t captured{},rejected_reentry{};
  {
    Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    auto& bodies=world.GetBodyInterface();std::array<JPH::BodyID,256> ids{};
    for(unsigned i=0;i<ids.size();++i){
      // Every world body owns a distinct immutable shape; the placement operand
      // is additional storage, never an unaccounted 257th arena allocation.
      JPH::RefConst<JPH::Shape> shape=new JPH::SphereShape(.5f);
      JPH::BodyCreationSettings settings(shape,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
      settings.mUserData=i+1;auto* body=bodies.CreateBody(settings);
      require(body!=nullptr,"full world capacity for operand query");ids[i]=body->GetID();
    }
    auto batch=ids;auto prepared=bodies.AddBodiesPrepare(batch.data(),256);
    bodies.AddBodiesFinalize(batch.data(),256,prepared,JPH::EActivation::DontActivate);world.OptimizeBroadPhase();
    Operand operand(storage);
    for(unsigned i=0;i<2;++i){
      Collector collector(world,operand);
      require(operand.collide(world,i!=0,collector),"serialized operand construction and query");
      require(collector.count==256&&collector.reentrant_rejections==256,"full operand output and rejected recursive admission");
      std::sort(collector.identities.begin(),collector.identities.end());
      for(unsigned n=0;n<256;++n)require(collector.identities[n]==n+1,"copied temporary operand query identity");
      captured+=collector.count;rejected_reentry+=collector.reentrant_rejections;
    }
    bodies.RemoveBodies(batch.data(),256);bodies.DestroyBodies(ids.data(),256);
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==before,"operand world and material complete teardown without CRT allocation");
  std::printf("{\"query_operand_passed\":true,\"world_shapes\":256,\"temporary_slots\":1,\"temporary_storage_bytes\":512,\"box_bytes\":%zu,\"sphere_bytes\":%zu,\"box_alignment\":%zu,\"sphere_alignment\":%zu,\"owner_bytes\":%zu,\"captured_hits\":%zu,\"reentrant_rejections\":%zu,\"sdk_heap_live_at_shutdown\":%zu,\"crt_new_requests\":%zu,\"allocation_proved\":false}\n",
    sizeof(JPH::BoxShape),sizeof(JPH::SphereShape),alignof(JPH::BoxShape),alignof(JPH::SphereShape),sizeof(Operand),captured,rejected_reentry,sdk_arena->stats.live_bytes,crt_requests.load()-before);
}
} // namespace query_operand_probe
