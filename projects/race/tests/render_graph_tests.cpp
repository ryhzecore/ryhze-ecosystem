#include "race/render/graph.hpp"
#include "test.hpp"
#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <vector>

using namespace race;
using namespace race::render;
using namespace race::render::graph;
namespace {
Resource buffer(Memory memory,State state,bool valid,std::uint32_t flags=CopySource|CopyDestination) {
  return {{ResourceKind::Buffer,ResourceFormat::Bytes,flags,256,1,1,1,1},
      Lifetime::Persistent,memory,state,state,valid,false};
}
Resource texture(ResourceFormat format,std::uint32_t flags,State initial,State terminal,bool valid,
    Lifetime lifetime=Lifetime::Persistent) {
  return {{ResourceKind::Texture2D,format,flags,0,4,4,1,1},lifetime,Memory::Device,initial,terminal,valid,false};
}
struct Fixture {
  std::vector<Resource> resources;
  std::vector<Pass> passes;
  std::vector<Use> uses;
  ResourceId resource(Resource r) { resources.push_back(r);return static_cast<ResourceId>(resources.size()-1); }
  void pass(Operation op,std::span<const Use> next,PassMask prerequisites=0) {
    passes.push_back({op,Queue::Direct,prerequisites,static_cast<std::uint16_t>(uses.size()),static_cast<std::uint16_t>(next.size())});
    uses.insert(uses.end(),next.begin(),next.end());
  }
  void pass(Operation op,std::initializer_list<Use> next,PassMask prerequisites=0) {
    pass(op,std::span(next.begin(),next.size()),prerequisites);
  }
  Result<Plan> run(Scratch& scratch) const { return compile({resources,passes,uses},scratch); }
};
// This fixture explicitly lists the actual grouped viewport operations. Draw
// and mip-copy counts do not multiply unique graph resources or uses.
Fixture viewport(std::size_t textures,bool cold) {
  Fixture f;std::vector<ResourceId> gpu,upload;
  for (std::size_t i=0;i<textures;++i) {
    auto r=texture(i%2 ? ResourceFormat::Rgba8Unorm : ResourceFormat::Rgba8Srgb,
        CopyDestination|ShaderSample,cold ? State::CopyDestination : State::SampledRead,State::SampledRead,!cold);
    r.description.width=1024;r.description.height=1;r.description.mip_levels=11;gpu.push_back(f.resource(r));
  }
  if (cold) for (std::size_t i=0;i<textures;++i) {
    auto staging=buffer(Memory::Upload,State::UploadRead,true,CopySource);
    staging.description.buffer_bytes=16384;upload.push_back(f.resource(staging));
  }
  auto vertices=buffer(Memory::Upload,State::UploadRead,true,VertexInput);
  vertices.description.buffer_bytes=228*32;const auto geometry=f.resource(vertices);
  vertices.description.buffer_bytes=(3*196608+4096)*32;const auto mesh=f.resource(vertices);
  const auto color=f.resource(texture(ResourceFormat::Rgba8Unorm,ColorTarget|CopySource,
      State::CopySource,State::CopySource,false));
  const auto depth=f.resource(texture(ResourceFormat::Depth32Float,DepthTarget,
      State::DepthAttachment,State::DepthAttachment,false));
  const auto back=f.resource(texture(ResourceFormat::Rgba8Unorm,CopyDestination,
      State::Present,State::Present,false,Lifetime::Imported));
  auto readback=buffer(Memory::Readback,State::CopyDestination,false,CopyDestination);
  readback.description.buffer_bytes=32;const auto timing=f.resource(readback);
  std::vector<Use> uses;
  if (cold) {
    for (std::size_t i=0;i<textures;++i) {
      uses.push_back({upload[i],Access::Read,State::UploadRead,false});
      uses.push_back({gpu[i],Access::Write,State::CopyDestination,true});
    }
    f.pass(Operation::Upload,uses);uses.clear();
  }
  for (const auto id:gpu) uses.push_back({id,Access::Read,State::SampledRead,false});
  uses.push_back({geometry,Access::Read,State::UploadRead,false});uses.push_back({mesh,Access::Read,State::UploadRead,false});
  uses.push_back({color,Access::Write,State::ColorAttachment,true});uses.push_back({depth,Access::Write,State::DepthAttachment,true});
  f.pass(Operation::Opaque,uses);
  f.pass(Operation::GridSelection,{{geometry,Access::Read,State::UploadRead,false},
      {mesh,Access::Read,State::UploadRead,false},{depth,Access::Read,State::DepthAttachment,false},
      {color,Access::ReadWrite,State::ColorAttachment,false}});
  f.pass(Operation::Overlay,{{mesh,Access::Read,State::UploadRead,false},{color,Access::ReadWrite,State::ColorAttachment,false}});
  f.pass(Operation::PresentCopy,{{color,Access::Read,State::CopySource,false},{back,Access::Write,State::CopyDestination,true}});
  const auto present=static_cast<PassMask>(1u<<(f.passes.size()-1));
  f.pass(Operation::TimingResolve,{{timing,Access::Write,State::CopyDestination,false}},present);
  return f;
}
void viewport_contracts() {
  Scratch scratch;auto f=viewport(1,true);auto result=f.run(scratch);CHECK(result);const auto& plan=result.value();
  CHECK(plan.resources().size()==8);CHECK(plan.steps().size()==6);CHECK(plan.uses().size()==16);
  const std::array<Transition,5> expected{{
    {0,State::CopyDestination,State::SampledRead},{4,State::CopySource,State::ColorAttachment},
    {4,State::ColorAttachment,State::CopySource},{6,State::Present,State::CopyDestination},
    {6,State::CopyDestination,State::Present}
  }};
  CHECK(std::equal(plan.transitions().begin(),plan.transitions().end(),expected.begin(),expected.end()));
  const std::array<PassMask,6> prerequisites{0,1,2,4,8,16};
  CHECK(std::equal(plan.dependencies().begin(),plan.dependencies().end(),prerequisites.begin(),prerequisites.end()));
  for (std::size_t i=0;i<plan.steps().size();++i) CHECK(plan.steps()[i].pass==static_cast<PassId>(i));
  CHECK(plan.steps()[1].before_count==2);CHECK(plan.steps()[4].before_count==2);
  CHECK(plan.steps()[4].after_count==1);CHECK(plan.steps()[5].before_count==0);
  CHECK(plan.lifetimes()[0].first==0);CHECK(plan.lifetimes()[0].last==1);
  CHECK(plan.lifetimes()[4].first==1);CHECK(plan.lifetimes()[4].last==4);
  CHECK(plan.lifetimes()[0].content_valid_after);CHECK(!plan.lifetimes()[7].content_valid_after);
  CHECK(!f.resources[0].content_valid); // Compilation never publishes GPU residency.
  CHECK(f.resources[0].initial==State::CopyDestination);
  // Plan owns descriptions; later source mutation and scratch reuse cannot alter it.
  f.resources[0].description.width=1;f.uses[0].resource=63;
  CHECK(plan.resources()[0].description.width==1024);CHECK(plan.uses()[0].resource==1);
  CHECK_ERROR(f.run(scratch),ErrorCode::InvalidInput);
  CHECK(plan.transitions()[0]==expected[0]);
  auto warm=viewport(1,false);auto warmed=warm.run(scratch);CHECK(warmed);
  const std::array<Transition,4> warm_expected{{
    {3,State::CopySource,State::ColorAttachment},{3,State::ColorAttachment,State::CopySource},
    {5,State::Present,State::CopyDestination},{5,State::CopyDestination,State::Present}
  }};
  CHECK(std::equal(warmed.value().transitions().begin(),warmed.value().transitions().end(),warm_expected.begin(),warm_expected.end()));
  auto maximum=viewport(16,true);auto admitted=maximum.run(scratch);CHECK(admitted);
  CHECK(admitted.value().resources().size()==38);CHECK(admitted.value().steps().size()==6);
  CHECK(admitted.value().uses().size()==61);CHECK(admitted.value().transitions().size()==20);
  CHECK(validate_alias_plan(admitted.value(),{},{},{},0)); // Committed resources, zero alias plan.
  Fixture capture;
  capture.resource(texture(ResourceFormat::Rgba8Unorm,CopySource,State::CopySource,State::CopySource,true,Lifetime::Imported));
  auto readback=buffer(Memory::Readback,State::CopyDestination,false,CopyDestination);
  readback.description.buffer_bytes=1024;capture.resource(readback);
  capture.pass(Operation::CaptureCopy,{{0,Access::Read,State::CopySource,false},{1,Access::Write,State::CopyDestination,false}});
  auto copied=capture.run(scratch);CHECK(copied);CHECK(copied.value().transitions().empty());
}
void hazards() {
  Scratch scratch;Fixture f;
  f.resource(buffer(Memory::Device,State::CopySource,true));f.resource(buffer(Memory::Device,State::CopySource,true));
  f.pass(Operation::CaptureCopy,{{0,Access::Read,State::CopySource,false}});
  f.pass(Operation::Upload,{{0,Access::Write,State::CopyDestination,true}});
  f.pass(Operation::CaptureCopy,{{0,Access::Read,State::CopySource,false}});
  f.pass(Operation::Upload,{{0,Access::Write,State::CopyDestination,false}});
  f.pass(Operation::CaptureCopy,{{1,Access::Read,State::CopySource,false}});
  auto result=f.run(scratch);CHECK(result);
  // WAR 0->1, RAW 1->2, WAW 1->3 plus WAR 2->3. Pass 4 is independent.
  const std::array<PassMask,5> expected{0,1,2,6,0};
  CHECK(std::equal(result.value().dependencies().begin(),result.value().dependencies().end(),expected.begin(),expected.end()));
  CHECK(result.value().transitions().size()==4);
  CHECK(result.value().lifetimes()[0].first_order==0);CHECK(result.value().lifetimes()[0].last_order==3);
  f.passes[0].prerequisites=2;CHECK_ERROR(f.run(scratch),ErrorCode::Cycle);
  Fixture independent;
  for (unsigned i=0;i<3;++i) independent.resource(buffer(Memory::Device,State::CopySource,true));
  independent.pass(Operation::CaptureCopy,{{0,Access::Read,State::CopySource,false}},2);
  independent.pass(Operation::CaptureCopy,{{1,Access::Read,State::CopySource,false}});
  independent.pass(Operation::CaptureCopy,{{2,Access::Read,State::CopySource,false}});
  auto ordered=independent.run(scratch);CHECK(ordered);
  CHECK(ordered.value().steps()[0].pass==1);CHECK(ordered.value().steps()[1].pass==0);CHECK(ordered.value().steps()[2].pass==2);
  // An explicit later producer cannot rewrite declaration-order resource versions.
  independent.uses[0]={0,Access::Read,State::CopySource,false};
  independent.uses[1]={0,Access::Write,State::CopyDestination,true};
  CHECK_ERROR(independent.run(scratch),ErrorCode::Cycle);
}
void rejections() {
  Scratch scratch;auto good=viewport(1,true);auto bad=good;
  bad.passes[0].queue=static_cast<Queue>(2);CHECK_ERROR(bad.run(scratch),ErrorCode::Unsupported);
  bad=good;bad.passes[0].operation=static_cast<Operation>(255);CHECK_ERROR(bad.run(scratch),ErrorCode::Unsupported);
  bad=good;bad.passes[0].prerequisites=1;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidDependency);
  bad=good;bad.passes[0].prerequisites=128;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidDependency);
  bad=good;bad.passes[0].use_count=0;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.passes[0].first_use=UINT16_MAX;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.passes[1].first_use=0;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses.push_back(bad.uses.back());CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[0].resource=64;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[1]=bad.uses[0];CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[0].state=State::CopySource;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[0].access=static_cast<Access>(0);CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[0].initializes_all=true;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[1].initializes_all=false;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[0].memory=static_cast<Memory>(255);CHECK_ERROR(bad.run(scratch),ErrorCode::Unsupported);
  bad=good;bad.resources[0].lifetime=static_cast<Lifetime>(0);CHECK_ERROR(bad.run(scratch),ErrorCode::Unsupported);
  bad=good;bad.resources[0].initial=State::Undefined;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[0].terminal=State::Undefined;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[0].terminal=static_cast<State>(255);CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[0].memory=Memory::Upload;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[1].memory=Memory::Device;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[1].initial=State::CopySource;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[7].terminal=State::CopySource;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources[6].lifetime=Lifetime::Persistent;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.resources.push_back(buffer(Memory::Device,State::CopySource,true));CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=viewport(1,false);bad.resources[0].content_valid=false;CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  bad=good;bad.uses[6]={5,Access::ReadWrite,State::DepthAttachment,false};CHECK_ERROR(bad.run(scratch),ErrorCode::InvalidInput);
  Fixture partial;partial.resource(buffer(Memory::Device,State::CopyDestination,false));
  partial.pass(Operation::Upload,{{0,Access::Write,State::CopyDestination,false}});
  auto admitted=partial.run(scratch);CHECK(admitted);CHECK(!admitted.value().lifetimes()[0].content_valid_after);
  partial.resources[0].exported=true;CHECK_ERROR(partial.run(scratch),ErrorCode::InvalidInput);
  partial.resources[0].exported=false;partial.resources[0].terminal=State::CopySource;
  CHECK_ERROR(partial.run(scratch),ErrorCode::InvalidInput);
  CHECK_ERROR(compile({{},{},{}},scratch),ErrorCode::InvalidInput);
}
void capacities() {
  Scratch scratch;Fixture f;
  for (std::size_t i=0;i<64;++i) f.resource(buffer(Memory::Device,State::CopySource,true));
  for (std::size_t p=0;p<8;++p) {
    std::array<Use,16> uses;
    for (std::size_t i=0;i<16;++i) uses[i]={static_cast<ResourceId>((p%4)*16+i),
        p<4 ? Access::Write : Access::Read,p<4 ? State::CopyDestination : State::CopySource,p<4};
    f.pass(p<4 ? Operation::Upload : Operation::CaptureCopy,uses);
  }
  auto admitted=f.run(scratch);CHECK(admitted);
  CHECK(admitted.value().resources().size()==64);CHECK(admitted.value().steps().size()==8);
  CHECK(admitted.value().uses().size()==128);CHECK(admitted.value().transitions().size()==128);
  auto bad=f;bad.resources.push_back(f.resources[0]);CHECK_ERROR(bad.run(scratch),ErrorCode::Exhausted);
  bad=f;bad.passes.push_back(f.passes[0]);CHECK_ERROR(bad.run(scratch),ErrorCode::Exhausted);
  bad=f;bad.uses.push_back(f.uses[0]);CHECK_ERROR(bad.run(scratch),ErrorCode::Exhausted);
  CHECK(f.run(scratch)); // Failed candidates do not poison reusable scratch.
}
Fixture transient_chain() {
  Fixture f;
  for (unsigned i=0;i<3;++i) f.resource(texture(ResourceFormat::Rgba8Unorm,ColorTarget|CopySource,
      State::Undefined,State::CopySource,false,Lifetime::Transient));
  for (ResourceId i=0;i<3;++i) {
    f.pass(Operation::Opaque,{{i,Access::Write,State::ColorAttachment,true}});
    f.pass(Operation::CaptureCopy,{{i,Access::Read,State::CopySource,false}});
  }
  return f;
}
void aliases() {
  Scratch scratch;auto f=transient_chain();auto admitted=f.run(scratch);CHECK(admitted);const auto& plan=admitted.value();
  std::array<NativeAllocation,3> allocations{{{0,65536,65536,1},{1,65536,65536,1},{2,65536,65536,1}}};
  std::array<AliasHeap,1> heaps{{{65536,1}}};
  std::array<AliasPlacement,3> placements{{{0,0,0},{1,0,0},{2,0,0}}};
  auto valid=validate_alias_plan(plan,allocations,heaps,placements,65536);CHECK(valid);
  CHECK(valid.value().heap_bytes==65536);CHECK(valid.value().allocation_bytes==196608);
  CHECK(valid.value().overlap_pairs==3);CHECK(valid.value().boundary_count==2);
  CHECK(valid.value().boundaries[0].resource==1);CHECK(valid.value().boundaries[0].before_pass==2);
  CHECK(valid.value().boundaries[1].resource==2);CHECK(valid.value().boundaries[1].before_pass==4);
  CHECK_ERROR(validate_alias_plan(plan,allocations,heaps,placements,65535),ErrorCode::Exhausted);
  auto a=allocations;a[1].resource=0;CHECK_ERROR(validate_alias_plan(plan,a,heaps,placements,65536),ErrorCode::InvalidInput);
  a=allocations;a[0].alignment=3;CHECK_ERROR(validate_alias_plan(plan,a,heaps,placements,65536),ErrorCode::InvalidInput);
  a=allocations;a[0].bytes=63;CHECK_ERROR(validate_alias_plan(plan,a,heaps,placements,65536),ErrorCode::InvalidInput);
  a=allocations;a[0].heap_class=2;CHECK_ERROR(validate_alias_plan(plan,a,heaps,placements,65536),ErrorCode::InvalidInput);
  auto p=placements;p[1].resource=0;CHECK_ERROR(validate_alias_plan(plan,allocations,heaps,p,65536),ErrorCode::InvalidInput);
  p=placements;p[0].offset=1;CHECK_ERROR(validate_alias_plan(plan,allocations,heaps,p,65536),ErrorCode::InvalidInput);
  p=placements;p[0].heap=1;CHECK_ERROR(validate_alias_plan(plan,allocations,heaps,p,65536),ErrorCode::InvalidInput);
  p=placements;p[0].offset=65536;CHECK_ERROR(validate_alias_plan(plan,allocations,heaps,p,65536),ErrorCode::Exhausted);
  CHECK_ERROR(validate_alias_plan(plan,std::span(allocations).first(2),heaps,placements,65536),ErrorCode::InvalidInput);
  const std::array<AliasHeap,2> extra{{{65536,1},{65536,1}}};
  CHECK_ERROR(validate_alias_plan(plan,allocations,extra,placements,131072),ErrorCode::InvalidInput);
  const auto maximum=std::numeric_limits<std::uint64_t>::max();
  const std::array<AliasHeap,2> huge{{{maximum,1},{1,1}}};
  CHECK_ERROR(validate_alias_plan(plan,allocations,huge,placements,maximum),ErrorCode::Overflow);
  a=allocations;a[0].bytes=maximum;const std::array<AliasHeap,1> huge_one{{{maximum,1}}};
  CHECK_ERROR(validate_alias_plan(plan,a,huge_one,placements,maximum),ErrorCode::Overflow);
  Fixture one;one.resource(f.resources[0]);one.pass(Operation::Opaque,{{0,Access::Write,State::ColorAttachment,true}});
  auto one_plan=one.run(scratch);CHECK(one_plan);
  const std::array<NativeAllocation,1> one_allocation{{{0,maximum-65535,65536,1}}};
  const std::array<AliasPlacement,1> overflowing{{{0,0,65536}}};
  CHECK_ERROR(validate_alias_plan(one_plan.value(),one_allocation,huge_one,overflowing,maximum),ErrorCode::Overflow);
  Fixture overlap;
  overlap.resource(f.resources[0]);overlap.resource(f.resources[1]);
  overlap.pass(Operation::Opaque,{{0,Access::Write,State::ColorAttachment,true}});
  overlap.pass(Operation::Opaque,{{0,Access::Read,State::CopySource,false},{1,Access::Write,State::ColorAttachment,true}});
  overlap.pass(Operation::CaptureCopy,{{1,Access::Read,State::CopySource,false}});
  auto overlap_plan=overlap.run(scratch);CHECK(overlap_plan);
  CHECK_ERROR(validate_alias_plan(overlap_plan.value(),std::span(allocations).first(2),heaps,std::span(placements).first(2),65536),ErrorCode::InvalidInput);
  // Touching memory ranges do not overlap, even when live intervals share a pass.
  std::array<AliasPlacement,2> touching{{{0,0,0},{1,0,65536}}};const std::array<AliasHeap,1> larger{{{131072,1}}};
  auto disjoint=validate_alias_plan(overlap_plan.value(),std::span(allocations).first(2),larger,touching,131072);CHECK(disjoint);
  CHECK(disjoint.value().overlap_pairs==0);CHECK(disjoint.value().boundary_count==0);
  auto excluded=f;excluded.resources[0].lifetime=Lifetime::Imported;excluded.resources[0].initial=State::CopySource;
  auto imported=excluded.run(scratch);CHECK(imported);
  CHECK_ERROR(validate_alias_plan(imported.value(),std::span(allocations).first(2),heaps,std::span(placements).first(2),65536),ErrorCode::InvalidInput);
  excluded=f;excluded.resources[0].exported=true;auto exported=excluded.run(scratch);CHECK(exported);
  CHECK_ERROR(validate_alias_plan(exported.value(),std::span(allocations).first(2),heaps,std::span(placements).first(2),65536),ErrorCode::InvalidInput);
  excluded=f;excluded.resources[0].content_valid=true;CHECK_ERROR(excluded.run(scratch),ErrorCode::InvalidInput);
  // Later initialization makes a graph read valid, but does not satisfy the
  // stricter alias-placement contract requiring a complete first write.
  Fixture delayed;delayed.resource(f.resources[0]);
  delayed.pass(Operation::Opaque,{{0,Access::Write,State::ColorAttachment,false}});
  delayed.pass(Operation::Opaque,{{0,Access::Write,State::ColorAttachment,true}});
  delayed.pass(Operation::CaptureCopy,{{0,Access::Read,State::CopySource,false}});
  auto delayed_plan=delayed.run(scratch);CHECK(delayed_plan);
  CHECK_ERROR(validate_alias_plan(delayed_plan.value(),std::span(allocations).first(1),heaps,std::span(placements).first(1),65536),ErrorCode::InvalidInput);
}
} // namespace
int main() { viewport_contracts();hazards();rejections();capacities();aliases(); }
