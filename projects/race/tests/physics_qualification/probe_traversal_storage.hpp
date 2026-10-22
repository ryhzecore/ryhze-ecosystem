#pragma once
// Exercise the pinned SDK containers at a conservative traversal capacity.
// This is synthetic storage corroboration, not a reachable tree-shape claim.
namespace traversal_probe {
void run(){
  using IDs=JPH::Array<JPH::uint32,JPH::STLLocalAllocator<JPH::uint32,128>>;
  using Fractions=JPH::Array<float,JPH::STLLocalAllocator<float,128>>;
  struct ValidationEntry {JPH::uint32 node,parent;};
  using Validation=JPH::Array<ValidationEntry,JPH::STLLocalAllocator<ValidationEntry,128>>;
  static_assert(sizeof(ValidationEntry)==8&&sizeof(JPH::uint32)==4&&sizeof(float)==4);
  constexpr unsigned frontier_bound=512+256,capacity_bound=1024;
  const auto crt_before=crt_requests.load();
  std::size_t rebuild_peak{},query_peak{},validation_peak{};
  std::printf("{\"schema\":1,\"sites\":{\n");
  allocation_site("rebuild_spill",5,[&](AllocationTrace& trace){
    const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
    {
      // Private NodeID's new[] routes to JPH::Allocate; use the same allocator
      // for its verified four-byte payload without exposing the private type.
      auto* body_ids=JPH::Allocate(256*sizeof(JPH::uint32));
      IDs stack;stack.reserve(128);
      for(unsigned i=0;i<frontier_bound;++i)stack.push_back(i);
      require(stack.capacity()==capacity_bound,"rebuild traversal capacity bound");
      for(unsigned i=0;i<frontier_bound;++i)require(stack[i]==i,"reallocated traversal IDs preserved");
      stack.clear(); // Capacity remains live across BuildTree's center storage.
      auto* centers=new JPH::Vec3[256];
      require(trace.count==5&&trace.entries[0].live&&trace.entries[3].live&&trace.entries[4].live,"rebuild retained stack and array overlap");
      delete[] centers;JPH::Free(body_ids);
    }
    rebuild_peak=sdk_arena->stats.peak_bytes-baseline;
    require(rebuild_peak<=9696,"rebuild charged upper bound");
  });
  allocation_site("ray_spill",6,[&](AllocationTrace& trace){
    const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
    {
      IDs stack;Fractions fractions;stack.resize(128);fractions.resize(128);
      for(unsigned i=0;i<128;++i){stack[i]=i;fractions[i]=float(i);}
      for(unsigned capacity=256;capacity<=capacity_bound;capacity*=2){
        // WalkTree resizes IDs first, then the ray visitor's fraction array.
        stack.resize(capacity);fractions.resize(capacity);
        for(unsigned i=0;i<capacity/2;++i)require(stack[i]==i&&fractions[i]==float(i),"ray stacks preserved through growth");
        for(unsigned i=capacity/2;i<capacity;++i){stack[i]=i;fractions[i]=float(i);}
      }
      require(trace.count==6&&trace.entries[4].live&&trace.entries[5].live,"two retained ray stack arrays");
    }
    query_peak=sdk_arena->stats.peak_bytes-baseline;
    require(query_peak<=10720,"ray old/new charged overlap bound");
  });
  allocation_site("validation_spill",2,[&](AllocationTrace&){
    const auto baseline=sdk_arena->stats.live_bytes;sdk_arena->stats.peak_bytes=baseline;
    {
      Validation stack;stack.reserve(128);
      for(unsigned i=0;i<512;++i)stack.push_back({i,i+1});
      require(stack.capacity()==512,"node-only validation capacity bound");
      for(unsigned i=0;i<512;++i)require(stack[i].node==i&&stack[i].parent==i+1,"validation entries preserved through growth");
    }
    validation_peak=sdk_arena->stats.peak_bytes-baseline;
    require(validation_peak<=6464,"validation old/new charged overlap bound");
  });
  require(sdk_arena->stats.live_bytes==0&&crt_requests.load()==crt_before,"traversal storage complete teardown");
  std::printf("\"complete_allocation_inventory\":false},\"traversal_storage_passed\":true,\"synthetic_capacity_fixture\":true,\"frontier_bound\":%u,\"capacity_bound\":%u,\"rebuild_observed_overlap\":%zu,\"ray_observed_overlap\":%zu,\"validation_observed_overlap\":%zu,\"rebuild_charge_bound\":9696,\"ray_charge_bound\":10720,\"validation_charge_bound\":6464,\"sdk_heap_live_at_shutdown\":0,\"crt_new_requests\":0,\"allocation_proved\":false}\n",frontier_bound,capacity_bound,rebuild_peak,query_peak,validation_peak);
}
} // namespace traversal_probe
