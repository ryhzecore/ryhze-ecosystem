#pragma once
// Actual PhysicsSystem initialization plus the primitive registry/material.
namespace startup_probe {
void run(){
  const auto crt_before=crt_requests.load();
  std::size_t startup_raw{},startup_charged{},material_raw{},material_charged{};
  std::printf("{\"schema\":1,\"sites\":{\n");
  allocation_site("primitive_registration_material",2,[&](AllocationTrace& trace){
    initialize_primitives();
    require(trace.count==2&&trace.entries[0].requested==sizeof(JPH::PhysicsMaterialSimple),"material object and SDK-owned name allocation");
    for(std::size_t i=0;i<trace.count;++i){material_raw+=trace.entries[i].requested;material_charged+=trace.entries[i].charged;}
    JPH::PhysicsMaterial::sDefault=nullptr;
  });
  allocation_site("physics_system_initialization",17,[&](AllocationTrace& trace){
    Layers layers;ObjectBroadFilter broad_filter;PairFilter pair_filter;
    JPH::PhysicsSystem world;world.Init(256,1,32768,32768,layers,broad_filter,pair_filter);
    require(trace.count==17&&trace.frees==0,"complete immediate initialization allocation sequence");
    require(trace.entries[5].requested==sizeof(JPH::BroadPhaseQuadTree),"allocated broadphase wrapper object ABI");
    for(std::size_t i=0;i<trace.count;++i){startup_raw+=trace.entries[i].requested;startup_charged+=trace.entries[i].charged;}
  });
  // LargeIslandSplitter's Debug validation uses this real SDK container.
  // Standalone storage exercise: it does not claim every split uses this path.
  allocation_site("split_validation_bits",1,[&](AllocationTrace& trace){
    JPH::Array<bool> used(128,false);
    require(trace.count==1&&trace.entries[0].requested==128,"Debug split validation bool-array ABI");
    for(unsigned i=0;i<128;++i){require(!used[i],"validation bits initialized");used[i]=true;}
  });
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"startup storage complete teardown");
  std::printf("\"complete_allocation_inventory\":false},\"startup_storage_passed\":true,\"split_bits_synthetic\":true,\"broadphase_object_bytes\":%zu,\"broadphase_object_alignment\":%zu,\"material_object_bytes\":%zu,\"initialization_raw_bytes\":%zu,\"initialization_charged_bytes\":%zu,\"material_raw_bytes\":%zu,\"material_charged_bytes\":%zu,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",sizeof(JPH::BroadPhaseQuadTree),alignof(JPH::BroadPhaseQuadTree),sizeof(JPH::PhysicsMaterialSimple),startup_raw,startup_charged,material_raw,material_charged);
}
} // namespace startup_probe
