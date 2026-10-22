#pragma once
// Included only by allocation_probe.cpp, after its qualification instrumentation.
// Direct SDK internals here do not grant application admission or add an adapter.
namespace lifetime_probe {
struct TreeObservation {
  std::size_t cycles{},maximum_live_nodes{},maximum_temporary_bytes{},maximum_temporary_requests{};
};

template<class Function> void temporary(TreeObservation& observation,Function exercise){
  AllocationTrace allocations;const auto baseline=sdk_arena->stats.live_bytes;
  sdk_arena->stats.peak_bytes=baseline;allocation_trace=&allocations;
  exercise();allocation_trace=nullptr;
  require(allocations.count==allocations.frees&&sdk_arena->stats.live_bytes==baseline,"tree temporary lifetime");
  for(std::size_t i=0;i<allocations.count;++i)require(!allocations.entries[i].live,"tree temporary retained pointer");
  observation.maximum_temporary_bytes=std::max(observation.maximum_temporary_bytes,sdk_arena->stats.peak_bytes-baseline);
  observation.maximum_temporary_requests=std::max(observation.maximum_temporary_requests,allocations.count);
}

// Legal public free-list operations count remaining slots without reading or
// modifying private fields. Dummy nodes are unreachable and immediately freed.
// This also deliberately commits both pages before the measured tree lifecycle.
std::size_t live_nodes(JPH::QuadTree::Allocator& allocator){
  std::array<JPH::uint32,512> slots{};std::size_t count=0;
  for(;;){
    const auto slot=allocator.ConstructObject(false);
    if(slot==JPH::QuadTree::Allocator::cInvalidObjectIndex)break;
    require(count<slots.size(),"tree allocator capacity drift");slots[count++]=slot;
  }
  for(std::size_t i=0;i<count;++i)allocator.DestructObject(slots[i]);
  return slots.size()-count;
}

TreeObservation tree_case(bool adversarial,bool maintain){
  Layers layers;JPH::BodyManager manager;manager.Init(256,1,layers);
  JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.05f),.005f);
  std::array<JPH::BodyID,256> ids{};
  for(unsigned i=0;i<ids.size();++i){
    double p[3]{double(i%16),double(i/16),0};
    if(adversarial){p[0]=p[1]=p[2]=0;if(i<108)p[i%3]=512.*std::pow(.49,double(i/3));}
    JPH::BodyCreationSettings creation(shape,JPH::RVec3(p[0],p[1],p[2]),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
    auto* body=manager.AllocateBody(creation);require(body&&manager.AddBody(body),"tree fixture body creation");ids[i]=body->GetID();
  }
  JPH::QuadTree::TrackingVector tracking;tracking.resize(256);
  for(auto& entry:tracking){entry.mBroadPhaseLayer=0;entry.mObjectLayer=0;}
  JPH::QuadTree::Allocator allocator;
  // Exactly BroadPhaseQuadTree::Init's 256-body capacity and 256-slot pages.
  constexpr unsigned leaves=(256+1)/2,nodes=2*(leaves+(leaves+2)/3);
  static_assert(nodes==342&&JPH::QuadTree::Allocator::ObjectStorageSize==128);
  allocator.Init(nodes,256);JPH::QuadTree tree,empty_layer;tree.Init(allocator);empty_layer.Init(allocator);
  TreeObservation observation;
  if(maintain)require(live_nodes(allocator)==2,"two initial tree roots");
  for(unsigned cycle=0;cycle<(maintain?16U:32U);++cycle){
    auto batch=ids;JPH::QuadTree::AddState add;
    if(!maintain){
      tree.AddBodiesPrepare(manager.GetBodies(),tracking,batch.data(),256,add);
      tree.AddBodiesFinalize(tracking,256,add);
      tree.RemoveBodies(manager.GetBodies(),tracking,batch.data(),256);
      continue;
    }
    // Abort must return every prepared node to the pool. Nothing is published.
    temporary(observation,[&]{tree.AddBodiesPrepare(manager.GetBodies(),tracking,batch.data(),256,add);});
    temporary(observation,[&]{tree.AddBodiesAbort(tracking,add);});
    require(live_nodes(allocator)==2,"aborted tree nodes reclaimed");
    for(auto id:ids)require(tracking[id.GetIndex()].mBodyLocation==JPH::QuadTree::Tracking::cInvalidBodyLocation,"aborted body tracking cleared");
    temporary(observation,[&]{tree.AddBodiesPrepare(manager.GetBodies(),tracking,batch.data(),256,add);});
    tree.AddBodiesFinalize(tracking,256,add);
    const auto old=live_nodes(allocator);require(old>2,"published batch owns tree nodes");
    JPH::QuadTree::UpdateState update;
    temporary(observation,[&]{tree.UpdatePrepare(manager.GetBodies(),tracking,update,true);});
    const auto overlap=live_nodes(allocator);
    require(overlap>old,"old and new trees overlap during prepare");
    tree.UpdateFinalize(manager.GetBodies(),tracking,update);
    require(live_nodes(allocator)==overlap,"finalize retains old tree nodes");
    require(!tree.CanBeUpdated(),"old tree blocks another rebuild");
    // No query or update exists in this synchronous fixture. In BroadPhaseQuadTree
    // the matching reclamation is protected by FrameSync's old-query lock.
    tree.DiscardOldTree();
    require(live_nodes(allocator)==overlap-(old-1)&&tree.CanBeUpdated(),"quiescent discard reclaims old tree only");
    const auto current=live_nodes(allocator);
    tree.RemoveBodies(manager.GetBodies(),tracking,batch.data(),256);
    require(live_nodes(allocator)==current,"body removal retains tree nodes");
    temporary(observation,[&]{tree.UpdatePrepare(manager.GetBodies(),tracking,update,true);});
    tree.UpdateFinalize(manager.GetBodies(),tracking,update);tree.DiscardOldTree();
    require(live_nodes(allocator)==2,"empty rebuild reclaims removed-body tree");
    observation.maximum_live_nodes=std::max(observation.maximum_live_nodes,overlap);++observation.cycles;
  }
  return observation;
}

void run(){
  const auto crt_before=crt_requests.load();initialize_primitives();
  std::printf("{\"schema\":1,\"sites\":{\n");
  Layers layers;
  {
    JPH::BodyManager manager;manager.Init(256,1,layers);
    allocation_site("broadphase_initialization",4,[&](AllocationTrace& allocations){
      JPH::BroadPhaseQuadTree broad;broad.Init(&manager,layers);
      require(allocations.count==4,"broadphase initialization request count");
      require(allocations.entries[0].requested==256*sizeof(JPH::QuadTree::Tracking)&&
        allocations.entries[1].requested==2*sizeof(void*)&&allocations.entries[3].requested==32768,"broadphase fixed tables and first page");
    });
  }
  std::size_t cache_overlap=0,cache_peak_delta=0;
  {
    JPH::BodyManager manager;manager.Init(256,1,layers);
    JPH::BroadPhaseQuadTree broad;broad.Init(&manager,layers);
    JPH::BodyLockInterfaceNoLock lock(manager);JPH::BodyInterface bodies;bodies.Init(lock,manager,broad);
    JPH::RefConst<JPH::Shape> shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.5f));
    JPH::BodyCreationSettings creation(shape,JPH::RVec3::sZero(),JPH::Quat::sIdentity(),JPH::EMotionType::Static,0);
    std::array<JPH::BodyID,256> ids{};
    for(auto& id:ids){auto* body=bodies.CreateBody(creation);require(body!=nullptr,"registered body fixture");id=body->GetID();}
    allocation_site("create_before_register_rejection",1,[&](AllocationTrace& allocations){
      require(bodies.CreateBody(creation)==nullptr&&manager.GetNumBodies()==256,"full registry rejects after allocation");
      require(allocations.count==1&&allocations.frees==1,"rejected body allocation released");
    });
    allocation_site("unassigned_bodies",2,[&](AllocationTrace&){
      auto* first=bodies.CreateBodyWithoutID(creation);auto* second=bodies.CreateBodyWithoutID(creation);
      require(first&&second&&first->GetID().IsInvalid()&&second->GetID().IsInvalid(),"unassigned bodies live outside full registry");
      require(!bodies.AssignBodyID(first)&&manager.GetNumBodies()==256,"unassigned registration separately bounded");
      bodies.DestroyBodyWithoutID(second);bodies.DestroyBodyWithoutID(first);
    });
    allocation_site("shape_replacement_overlap",2,[&](AllocationTrace& allocations){
      JPH::RefConst<JPH::Shape> old_shape=new JPH::BoxShape(JPH::Vec3::sReplicate(.4f));
      JPH::RefConst<JPH::Shape> next_shape=new JPH::SphereShape(.4f);
      bodies.SetShape(ids[0],old_shape,true,JPH::EActivation::DontActivate);
      require(old_shape->GetRefCount()==2&&next_shape->GetRefCount()==1,"initial shape owners");
      bodies.SetShape(ids[0],next_shape,true,JPH::EActivation::DontActivate);
      require(old_shape->GetRefCount()==1&&next_shape->GetRefCount()==2&&allocations.frees==0,"external old shape outlives replacement");
      old_shape=nullptr;require(allocations.frees==1,"last old shape owner frees storage");
      bodies.SetShape(ids[0],shape,true,JPH::EActivation::DontActivate);
    });
    AllocationTrace cache;allocation_trace=&cache;
    for(auto id:ids){auto& body=manager.GetBody(id);manager.InvalidateContactCacheForBody(body);manager.InvalidateContactCacheForBody(body);}
    require(cache.count==9&&cache.frees==8,"cache growth through 256 entries");
    const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
    for(unsigned i=0;i<32;++i){
      // Trace cache growth only; body allocations/frees are outside this sample.
      allocation_trace=nullptr;bodies.DestroyBody(ids[i]);
      auto* body=bodies.CreateBody(creation);require(body!=nullptr,"body churn fixture");ids[i]=body->GetID();
      allocation_trace=&cache;manager.InvalidateContactCacheForBody(*body);manager.InvalidateContactCacheForBody(*body);
    }
    require(manager.GetNumBodies()==256&&cache.count==10&&cache.frees==9,"288 distinct body lifetimes grow cache beyond live-body limit");
    for(std::size_t i=0;i<cache.count;++i)require(cache.entries[i].requested==(std::size_t{4}<<i),"cache doubling request inventory");
    cache_overlap=cache.entries[8].charged+cache.entries[9].charged;
    cache_peak_delta=sdk_arena->stats.peak_bytes-baseline;
    require(cache_peak_delta>=cache.entries[9].charged,"new cache storage overlaps old allocation");
    const auto retained=sdk_arena->stats.live_bytes;
    manager.ValidateContactCacheForAllBodies();
    require(sdk_arena->stats.live_bytes==retained&&cache.count==10&&cache.frees==9,"cache clear retains capacity");
    for(auto id:ids){auto& body=manager.GetBody(id);require(!body.IsCollisionCacheInvalid(),"cache flags cleared");manager.InvalidateContactCacheForBody(body);}
    manager.ValidateContactCacheForAllBodies();
    require(cache.count==10&&cache.frees==9,"cache high-water storage reused");
    allocation_trace=nullptr;
    // Final retained cache allocation is released by manager destruction below;
    // complete SDK teardown is checked after all fixture owners have gone away.
  }
  const auto balanced=tree_case(false,true),adversarial=tree_case(true,true);
  JPH::PhysicsMaterial::sDefault=nullptr;
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"mutable lifetime complete SDK teardown");
  std::printf("\"site_inventory_complete\":false},\n");
  std::printf("\"cache_registered_bodies\":256,\"cache_distinct_lifetimes\":288,\"cache_capacity_entries\":512,\"cache_allocation_requests\":10,\"cache_overlap_charged_bytes\":%zu,\"cache_peak_delta_bytes\":%zu,\n",cache_overlap,cache_peak_delta);
  std::printf("\"tree_cycles\":%zu,\"tree_slot_capacity\":512,\"tree_slot_bytes\":128,\"tree_pages_prewarmed\":true,\"tree_observed_max_live_nodes\":%zu,\n",balanced.cycles+adversarial.cycles,std::max(balanced.maximum_live_nodes,adversarial.maximum_live_nodes));
  std::printf("\"tree_observed_max_temporary_bytes\":%zu,\"tree_observed_max_temporary_requests\":%zu,\"mutable_lifetimes_passed\":true,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",std::max(balanced.maximum_temporary_bytes,adversarial.maximum_temporary_bytes),std::max(balanced.maximum_temporary_requests,adversarial.maximum_temporary_requests));
}
} // namespace lifetime_probe
