#pragma once
// Actual public PhysicsSystem/BodyInterface wrapper allocation lifetimes.
namespace wrapper_probe {
void run(Scratch& scratch){
  const auto crt_before=crt_requests.load();initialize_primitives();
  std::size_t prepare_peak=0,rebuild_peak=0;
  std::printf("{\"schema\":1,\"sites\":{\n");
  {
    Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
    JPH::JobSystemSingleThreaded jobs(race::physics::PrimitiveProfile::jobs);
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.05f),.005f);
    std::array<JPH::BodyID,256> ids{};auto& api=world.GetBodyInterface();
    for(unsigned i=0;i<256;++i){
      JPH::BodyCreationSettings creation(shape,JPH::RVec3(double(i%16)*2,0,double(i/16)*2),JPH::Quat::sIdentity(),
        i<128?JPH::EMotionType::Static:JPH::EMotionType::Kinematic,i<128?0:1);
      auto* body=api.CreateBody(creation);require(body!=nullptr,"wrapper storage fixture body");ids[i]=body->GetID();
    }
    auto prepare=[&](AllocationTrace& trace,auto& batch){
      const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
      auto state=api.AddBodiesPrepare(batch.data(),256);
      constexpr auto alignment=alignof(JPH::QuadTree::AddState);
      constexpr auto one=(2*sizeof(JPH::BodyID*)+sizeof(JPH::QuadTree::AddState)+alignment-1)&~(alignment-1);
      require(trace.count==3&&trace.frees==2&&trace.entries[0].live&&!trace.entries[1].live&&!trace.entries[2].live,"wrapper prepare retains only LayerState allocation");
      require(trace.entries[0].requested==2*one&&trace.entries[1].requested==128*sizeof(JPH::Vec3)&&trace.entries[2].requested==128*sizeof(JPH::Vec3),"two-layer wrapper array and sequential center arrays");
      prepare_peak=std::max(prepare_peak,sdk_arena->stats.peak_bytes-baseline);return state;
    };
    allocation_site("wrapper_prepare_abort",3,[&](AllocationTrace& trace){
      auto batch=ids;auto state=prepare(trace,batch);api.AddBodiesAbort(batch.data(),256,state);
      for(auto id:ids)require(!api.IsAdded(id),"wrapper abort leaves all bodies unpublished");
    });
    allocation_site("wrapper_prepare_finalize",3,[&](AllocationTrace& trace){
      auto batch=ids;auto state=prepare(trace,batch);api.AddBodiesFinalize(batch.data(),256,state,JPH::EActivation::DontActivate);
      for(auto id:ids)require(api.IsAdded(id),"wrapper finalize publishes every body");
    });
    // Warm the second lazily allocated node page before temporary-only tracing.
    // Pages remain charged to the fixed pool and are not called temporary bytes.
    world.OptimizeBroadPhase();
    allocation_site("wrapper_full_rebuild",4,[&](AllocationTrace& trace){
      const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
      world.OptimizeBroadPhase();rebuild_peak=sdk_arena->stats.peak_bytes-baseline;
      for(std::size_t i=0;i<4;++i)require(trace.entries[i].requested==(i%2?128*sizeof(JPH::Vec3):128*sizeof(JPH::BodyID)),"both full rebuild temporary families");
    });
    for(auto id:ids)api.SetPosition(id,api.GetPosition(id)+JPH::RVec3(.01,0,0),JPH::EActivation::DontActivate);
    for(unsigned layer=0;layer<2;++layer){
      allocation_site(layer==0?"wrapper_partial_layer_0":"wrapper_partial_layer_1",2,[&](AllocationTrace& trace){
        require(world.Update(0,1,&scratch,&jobs)==JPH::EPhysicsUpdateError::None&&scratch.top==0,"ordinary paused wrapper update");
        require(trace.entries[0].requested==128*sizeof(JPH::BodyID)&&trace.entries[1].requested==128*sizeof(JPH::Vec3),"one-layer partial-update temporary storage");
      });
    }
    allocation_site("wrapper_clean_update",0,[&](AllocationTrace&){require(world.Update(0,1,&scratch,&jobs)==JPH::EPhysicsUpdateError::None,"clean paused wrapper update");});
    auto batch=ids;api.RemoveBodies(batch.data(),256);
    allocation_site("wrapper_empty_reclamation",0,[&](AllocationTrace&){world.OptimizeBroadPhase();});
    api.DestroyBodies(ids.data(),256);
  }
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"wrapper storage complete SDK teardown");
  std::printf("\"complete_allocation_inventory\":false},\"wrapper_storage_passed\":true,\"add_state_bytes\":%zu,\"add_state_alignment\":%zu,\"prepare_observed_overlap\":%zu,\"rebuild_observed_overlap\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",sizeof(JPH::QuadTree::AddState),alignof(JPH::QuadTree::AddState),prepare_peak,rebuild_peak);
}
} // namespace wrapper_probe
