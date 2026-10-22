#include "race/render/light_output.hpp"
#include "race/render/graph.hpp"
#include "test.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <numbers>
#include <vector>

using namespace race;
using namespace race::render;
namespace {
constexpr double not_a_number=std::numeric_limits<double>::quiet_NaN(),infinite=std::numeric_limits<double>::infinity();
bool near(double a,double b,double tolerance) { return std::fabs(a-b)<=tolerance; }

// Oracle group 1: units and energy. The shader is never the oracle; these are the
// closed-form expectations of the normalised Lambertian equation.
void units_and_energy() {
  CHECK(near(lambert_outgoing(1,0,std::numbers::pi,1),1.0,1e-12));
  CHECK(near(lambert_outgoing(1,0,std::numbers::pi,.5),.5,1e-12));
  CHECK(near(lambert_outgoing(.5,0,std::numbers::pi,1),.5,1e-12));
  CHECK(near(lambert_outgoing(.25,2,0,1),.5,1e-12));
  CHECK(lambert_outgoing(1,0,std::numbers::pi,-.3)==0.0); // Back-facing: no negative light.
  // Hemispherical integral of (rho/pi) cos over the hemisphere returns rho exactly.
  for (const double rho:{1.0,.5,.125}) {
    const std::size_t steps=65536;double integral{};
    for (std::size_t i=0;i<steps;++i) {
      const double theta=(i+.5)*(std::numbers::pi/2)/steps;
      integral+=(rho/std::numbers::pi)*std::cos(theta)*std::sin(theta)*((std::numbers::pi/2)/steps);
    }
    CHECK(near(integral*2*std::numbers::pi,rho,1e-7));
  }
  // Reflected flux never exceeds incident flux for rho <= 1: pi*Lo <= E*cos + pi*Lenv.
  for (const double rho:{0.0,.3,1.0}) for (const double e:{0.0,1000.0,100000.0}) for (const double env:{0.0,2.5,1000.0}) for (const double c:{0.0,.2,1.0})
    CHECK(std::numbers::pi*lambert_outgoing(rho,env,e,c)<=e*c+std::numbers::pi*env+1e-6);
  // Resolution: E/pi, exposure 2^stops/R, normalised direction, SI values preserved.
  PhysicalLighting p;p.direction_to_light={0,0,2};p.illuminance_lux=std::numbers::pi*100;p.environment_cd_m2=3;
  p.background_cd_m2={1,2,3};p.exposure_stops=1;p.reference_luminance_cd_m2=50;
  auto r=resolve_physical_lighting(p);CHECK(r);
  CHECK(near(r.value().irradiance_over_pi,100,1e-4) && r.value().environment==3.f && r.value().direction_to_light[2]==1.f);
  CHECK(near(r.value().exposure_scale,2.0/50,1e-7) && r.value().background_radiance[2]==3.f && r.value().target==OutputTarget::DisplaySdr);
}

void bounds_and_profiles() {
  PhysicalLighting p;p.illuminance_lux=100;
  CHECK(resolve_physical_lighting(p));
  auto bad=p;bad.direction_to_light={0,0,0};CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.direction_to_light={not_a_number,1,0};CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.illuminance_lux=-1;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.illuminance_lux=physical_illuminance_max_lux+1;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.illuminance_lux=infinite;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.environment_cd_m2=physical_environment_max_cd_m2+.5;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.background_cd_m2={0,physical_background_max_cd_m2+1,0};CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.exposure_stops=-physical_exposure_max_stops-1;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.reference_luminance_cd_m2=0;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::InvalidInput);
  bad=p;bad.target=static_cast<OutputTarget>(9);CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::Unsupported);
  bad=p;bad.target=OutputTarget::DisplayScrgb;CHECK_ERROR(resolve_physical_lighting(bad),ErrorCode::Unsupported);
  // Exactly one profile per frame: legacy frames carry default physical fields only.
  CHECK(validate_frame_lighting(LightingProfile::LegacyViewportV1,PhysicalLighting{}));
  CHECK_ERROR(validate_frame_lighting(LightingProfile::LegacyViewportV1,p),ErrorCode::InvalidInput);
  CHECK(validate_frame_lighting(LightingProfile::PhysicalDiffuseV1,p));
  CHECK_ERROR(validate_frame_lighting(LightingProfile::PhysicalDiffuseV1,bad),ErrorCode::Unsupported);
  CHECK_ERROR(validate_frame_lighting(static_cast<LightingProfile>(7),PhysicalLighting{}),ErrorCode::Unsupported);
  // Reflectance: factor*tint within [0,1]; excess is rejected, not clamped or treated as emission.
  CHECK(validate_physical_reflectance({.28f,.52f,.78f,1},{1,1,1,1}));
  CHECK_ERROR(validate_physical_reflectance({1.2f,.5f,.5f,1},{1,1,1,1}),ErrorCode::InvalidInput);
  CHECK_ERROR(validate_physical_reflectance({1,1,1,1},{1,1,1.0001f,1}),ErrorCode::InvalidInput);
  CHECK_ERROR(validate_physical_reflectance({1,1,1,.5f},{1,1,1,1}),ErrorCode::InvalidInput);
  CHECK_ERROR(validate_physical_reflectance({-.1f,1,1,1},{1,1,1,1}),ErrorCode::InvalidInput);
}

// Oracle group 3: exactly one output transform. Double references on both sides of the
// sRGB breakpoint, exposure doubling, rounding, and the HDR reference curve limits.
void transforms() {
  CHECK(display_sdr_encoded(0,1)==0.0);
  const double breakpoint_x=0.0031308/(1-0.0031308); // Reinhard inverse of the sRGB breakpoint.
  // IEC 61966-2-1's two segments meet within about 5e-6 at the breakpoint; the encoding stays monotonic across it.
  const double below=display_sdr_encoded(breakpoint_x*(1-1e-6),1),above=display_sdr_encoded(breakpoint_x*(1+1e-6),1);
  CHECK(near(below,12.92*0.0031308,2e-5) && near(above,12.92*0.0031308,2e-5) && above>=below);
  CHECK(near(srgb_decode(srgb_encode(.2)),.2,1e-12) && near(srgb_decode(srgb_encode(.001)),.001,1e-12));
  // One stop doubles the exposed value before the tone transform; stored scene values are untouched.
  CHECK(near(reinhard_rgb_v1(.6),.6/1.6,1e-12));
  CHECK(near(display_sdr_encoded(.3,std::exp2(1)/1.0),srgb_encode(.6/1.6),1e-12));
  CHECK(unorm8(0)==0 && unorm8(1)==255 && unorm8(.5)==128 && unorm8(-.2)==0 && unorm8(1.7)==255);
  CHECK(unorm8(srgb_encode(reinhard_rgb_v1(1.0)))==188); // sRGB(0.5)=0.7354 -> 187.5 rounds up.
  // Reference HDR curve: approaches the peak, low-level slope equals paper white.
  CHECK(near(reference_hdr_display_nits(1e9,1000,200).value(),1000,1e-3));
  CHECK(near(reference_hdr_display_nits(1e-6,1000,200).value()/1e-6,200,1e-2));
  CHECK(near(scrgb_from_nits(80),1,1e-12) && near(scrgb_from_nits(1000),12.5,1e-12));
  CHECK_ERROR(reference_hdr_display_nits(1,100,200),ErrorCode::InvalidInput);
  CHECK_ERROR(reference_hdr_display_nits(-1,1000,200),ErrorCode::InvalidInput);
  CHECK_ERROR(reference_hdr_display_nits(1,20000,200),ErrorCode::InvalidInput);
}

// Oracle group 6 (portable part): output resource class admission and the declared budget.
void output_resources_and_budget() {
  const ResourceDescription hdr{ResourceKind::Texture2D,ResourceFormat::Rgba16Float,ColorTarget|ShaderSample|CopySource,0,4096,4096,1,1};
  CHECK_ERROR(validate_resource(hdr),ErrorCode::Exhausted); // 128 MiB exceeds the general 64 MiB ceiling.
  auto admitted=validate_output_resource(hdr,OutputResourceClass::HdrAttachment);CHECK(admitted && admitted.value()==128ull*1024*1024);
  auto larger=hdr;larger.width=4097;CHECK_ERROR(validate_output_resource(larger,OutputResourceClass::HdrAttachment),ErrorCode::Exhausted);
  auto wrong=hdr;wrong.format=ResourceFormat::Rgba8Unorm;CHECK_ERROR(validate_output_resource(wrong,OutputResourceClass::HdrAttachment),ErrorCode::InvalidInput);
  wrong=hdr;wrong.mip_levels=2;CHECK_ERROR(validate_output_resource(wrong,OutputResourceClass::HdrAttachment),ErrorCode::InvalidInput);
  wrong=hdr;wrong.uses=ShaderSample;CHECK_ERROR(validate_output_resource(wrong,OutputResourceClass::HdrAttachment),ErrorCode::InvalidInput);
  wrong=hdr;wrong.uses=ColorTarget|DepthTarget;CHECK_ERROR(validate_output_resource(wrong,OutputResourceClass::HdrAttachment),ErrorCode::InvalidInput);
  // The 128 MiB privilege never leaks into texture/upload/vertex admission.
  const ResourceDescription material{ResourceKind::Texture2D,ResourceFormat::Rgba8Srgb,CopyDestination|ShaderSample,0,4096,4096,1,1};
  CHECK_ERROR(validate_output_resource(material,OutputResourceClass::HdrAttachment),ErrorCode::InvalidInput);
  const ResourceDescription readback{ResourceKind::Buffer,ResourceFormat::Bytes,CopyDestination,100ull*1024*1024,1,1,1,1};
  CHECK_ERROR(validate_resource(readback),ErrorCode::Exhausted);
  CHECK(validate_output_resource(readback,OutputResourceClass::HdrReadback));
  auto vertex=readback;vertex.uses=CopyDestination|VertexInput;CHECK_ERROR(validate_output_resource(vertex,OutputResourceClass::HdrReadback),ErrorCode::InvalidInput);
  auto huge=readback;huge.buffer_bytes=output_resource_payload_bytes+1;CHECK_ERROR(validate_output_resource(huge,OutputResourceClass::HdrReadback),ErrorCode::Exhausted);
  CHECK(validate_output_resource(material,OutputResourceClass::General)); // General stays the existing path.
  CHECK_ERROR(validate_output_resource(hdr,static_cast<OutputResourceClass>(5)),ErrorCode::Unsupported);
  // Budget: checked sum against the declared ceilings; slack is explicit, never implicit.
  const std::array<std::uint64_t,5> sdr{256ull<<20,128ull<<20,64ull<<20,64ull<<20,8ull<<20};
  auto total=admit_output_allocations(sdr,OutputTarget::DisplaySdr);CHECK(total && total.value()==520ull<<20);
  const std::array<std::uint64_t,2> over{520ull<<20,1};CHECK_ERROR(admit_output_allocations(over,OutputTarget::DisplaySdr),ErrorCode::Exhausted);
  CHECK(admit_output_allocations(over,OutputTarget::DisplayScrgb));
  const std::array<std::uint64_t,2> overflow{std::numeric_limits<std::uint64_t>::max(),1};CHECK_ERROR(admit_output_allocations(overflow,OutputTarget::DisplaySdr),ErrorCode::Overflow);
  const std::array<std::uint64_t,1> zero{0};CHECK_ERROR(admit_output_allocations(zero,OutputTarget::DisplaySdr),ErrorCode::InvalidInput);
  CHECK(output_budget_bytes(OutputTarget::DisplaySdr)==output_budget_physical_sdr_bytes && output_budget_bytes(OutputTarget::DisplayScrgb)==output_budget_physical_scrgb_bytes);
}

// Graph: the physical pass set with operation 8 compiles into the fixed storage; the
// HDR classes are admitted only as retained resources; operation 9 is rejected.
void physical_graph() {
  using namespace race::render::graph;
  Scratch scratch;std::vector<Resource> resources;std::vector<Pass> passes;std::vector<Use> uses;
  auto add=[&](Resource r){resources.push_back(r);return static_cast<ResourceId>(resources.size()-1);};
  auto pass=[&](Operation op,std::initializer_list<Use> next,PassMask prerequisites=0){
    passes.push_back({op,Queue::Direct,prerequisites,static_cast<std::uint16_t>(uses.size()),static_cast<std::uint16_t>(next.size())});
    uses.insert(uses.end(),next.begin(),next.end());};
  Resource hdr{{ResourceKind::Texture2D,ResourceFormat::Rgba16Float,ColorTarget|ShaderSample|CopySource,0,1280,720,1,1},
      Lifetime::Persistent,Memory::Device,State::CopySource,State::CopySource,false,false,OutputResourceClass::HdrAttachment};
  Resource display{{ResourceKind::Texture2D,ResourceFormat::Rgba8Unorm,ColorTarget|CopySource,0,1280,720,1,1},
      Lifetime::Persistent,Memory::Device,State::CopySource,State::CopySource,false,false};
  display.exported=true;
  Resource depth{{ResourceKind::Texture2D,ResourceFormat::Depth32Float,DepthTarget,0,1280,720,1,1},
      Lifetime::Persistent,Memory::Device,State::DepthAttachment,State::DepthAttachment,false,false};
  Resource back{{ResourceKind::Texture2D,ResourceFormat::Rgba8Unorm,CopyDestination,0,1280,720,1,1},
      Lifetime::Imported,Memory::Device,State::Present,State::Present,false,false};
  const auto h=add(hdr),c=add(display),d=add(depth),b=add(back);
  pass(Operation::Opaque,{{h,Access::Write,State::ColorAttachment,true},{d,Access::Write,State::DepthAttachment,true}});
  pass(Operation::OutputTransform,{{h,Access::Read,State::SampledRead,false},{c,Access::Write,State::ColorAttachment,true}});
  pass(Operation::GridSelection,{{d,Access::Read,State::DepthAttachment,false},{c,Access::ReadWrite,State::ColorAttachment,false}});
  pass(Operation::PresentCopy,{{c,Access::Read,State::CopySource,false},{b,Access::Write,State::CopyDestination,true}});
  auto plan=compile({resources,passes,uses},scratch);CHECK(plan);
  CHECK(plan.value().resources().size()==4 && plan.value().passes().size()==4 && plan.value().uses().size()==8);
  // HDR: CopySource->ColorAttachment, ->SampledRead, terminal ->CopySource; display: CopySource->ColorAttachment, ->CopySource;
  // back: Present->CopyDestination->Present; depth stays. Seven transitions in total.
  CHECK(plan.value().transitions().size()==7);
  const auto order=plan.value().steps();CHECK(order[0].pass==0 && order[1].pass==1 && order[2].pass==2 && order[3].pass==3);
  CHECK(plan.value().lifetimes()[h].content_valid_after && plan.value().lifetimes()[c].content_valid_after);
  // Operation 9 does not exist; an HDR class resource cannot be transient; a general
  // 4096x4096 FP16 target without the class is over the 64 MiB ceiling.
  auto bad_passes=passes;bad_passes[1].operation=static_cast<Operation>(9);
  CHECK_ERROR(compile({resources,bad_passes,uses},scratch),ErrorCode::Unsupported);
  auto bad_resources=resources;bad_resources[h].lifetime=Lifetime::Transient;bad_resources[h].initial=State::Undefined;
  CHECK_ERROR(compile({bad_resources,passes,uses},scratch),ErrorCode::Unsupported);
  bad_resources=resources;bad_resources[h].output_class=OutputResourceClass::General;bad_resources[h].description.width=4096;bad_resources[h].description.height=4096;
  CHECK_ERROR(compile({bad_resources,passes,uses},scratch),ErrorCode::Exhausted);
  bad_resources=resources;bad_resources[h].output_class=OutputResourceClass::HdrReadback;
  CHECK_ERROR(compile({bad_resources,passes,uses},scratch),ErrorCode::InvalidInput);
  // The same description compiles again into the reusable scratch after the rejections.
  CHECK(compile({resources,passes,uses},scratch));
}
} // namespace
int main() { units_and_energy();bounds_and_profiles();transforms();output_resources_and_budget();physical_graph(); }
