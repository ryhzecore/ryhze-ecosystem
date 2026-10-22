#include "race/render/resource_contract.hpp"
#include <algorithm>
#include <cstring>
#include <string_view>

namespace race::render {
namespace {
std::uint32_t u32(std::span<const std::byte> bytes,std::size_t at) noexcept {
  std::uint32_t value{};
  for (unsigned i=0;i<4;++i) value|=std::uint32_t(std::to_integer<unsigned char>(bytes[at+i]))<<(8*i);
  return value;
}
std::uint64_t u64(std::span<const std::byte> bytes,std::size_t at) noexcept {
  return u32(bytes,at)|(std::uint64_t(u32(bytes,at+4))<<32);
}
Digest256 digest_at(std::span<const std::byte> bytes,std::size_t at) noexcept {
  Digest256 value;std::copy_n(bytes.begin()+static_cast<std::ptrdiff_t>(at),value.size(),value.begin());return value;
}
bool zero(std::span<const std::byte> bytes) noexcept {
  return std::all_of(bytes.begin(),bytes.end(),[](std::byte b){return b==std::byte{};});
}
bool text(std::span<const std::byte> bytes,std::string_view expected) noexcept {
  return expected.size()<bytes.size() && !std::memcmp(bytes.data(),expected.data(),expected.size()) && zero(bytes.subspan(expected.size()));
}
bool present(const ViewportShaderIdentity& value) noexcept {
  return value.compiler_executable!=Digest256{} && value.compiler_library!=Digest256{} &&
      value.compiler_validator!=Digest256{} && value.source!=Digest256{} &&
      value.vertex!=Digest256{} && value.pixel!=Digest256{};
}
} // namespace
Result<std::uint64_t> validate_resource(const ResourceDescription& d,const ResourceLimits& limits) noexcept {
  if (!limits.payload_bytes || limits.payload_bytes>64*1024*1024 || !limits.max_dimension ||
      limits.max_dimension>4096 || !limits.max_mip_levels || limits.max_mip_levels>13)
    return fail(ErrorCode::InvalidInput,"invalid bounded render resource limits");
  constexpr std::uint32_t all=CopySource|CopyDestination|VertexInput|ConstantInput|ShaderSample|ColorTarget|DepthTarget;
  if (!d.uses || (d.uses&~all)) return fail(ErrorCode::Unsupported,"unsupported render resource use");
  if (d.samples!=1) return fail(ErrorCode::Unsupported,"resource profile requires one sample");
  if (d.kind==ResourceKind::Buffer) {
    if (d.format!=ResourceFormat::Bytes || (d.uses&~std::uint32_t(CopySource|CopyDestination|VertexInput|ConstantInput)) ||
        d.width!=1 || d.height!=1 || d.mip_levels!=1 || !d.buffer_bytes)
      return fail(ErrorCode::InvalidInput,"invalid byte-buffer description");
    if (d.buffer_bytes>limits.payload_bytes) return fail(ErrorCode::Exhausted,"buffer payload exceeds resource budget");
    if ((d.uses&ConstantInput) && d.buffer_bytes%16)
      return fail(ErrorCode::InvalidInput,"constant buffer payload requires 16-byte units");
    return d.buffer_bytes;
  }
  if (d.kind!=ResourceKind::Texture2D) return fail(ErrorCode::Unsupported,"unsupported render resource kind");
  if (d.buffer_bytes || !d.width || !d.height || !d.mip_levels || (d.uses&(VertexInput|ConstantInput)))
    return fail(ErrorCode::InvalidInput,"invalid texture description");
  if (d.width>limits.max_dimension || d.height>limits.max_dimension || d.mip_levels>limits.max_mip_levels)
    return fail(ErrorCode::Exhausted,"texture dimensions exceed resource bounds");
  std::uint64_t texel_bytes=4;
  switch (d.format) {
  case ResourceFormat::Rgba8Unorm: case ResourceFormat::Rgba8Srgb: break;
  case ResourceFormat::Rgba16Float:texel_bytes=8;break;
  case ResourceFormat::Depth32Float:
    if (d.uses&(ColorTarget|ShaderSample)) return fail(ErrorCode::Unsupported,"depth sampling is outside resource profile");
    break;
  default:return fail(ErrorCode::Unsupported,"unsupported texture format");
  }
  if ((d.uses&DepthTarget) && d.format!=ResourceFormat::Depth32Float)
    return fail(ErrorCode::InvalidInput,"depth target requires depth format");
  if ((d.uses&ColorTarget) && d.format==ResourceFormat::Depth32Float)
    return fail(ErrorCode::InvalidInput,"color target requires color format");
  std::uint64_t total{};auto width=d.width,height=d.height;
  for (std::uint32_t i=0;i<d.mip_levels;++i) {
    const auto bytes=std::uint64_t(width)*height*texel_bytes;
    if (bytes>limits.payload_bytes-total) return fail(ErrorCode::Exhausted,"texture payload exceeds resource budget");
    total+=bytes;
    if (i+1<d.mip_levels && width==1 && height==1)
      return fail(ErrorCode::InvalidInput,"texture mip count exceeds complete chain");
    width=std::max(1u,width/2);height=std::max(1u,height/2);
  }
  return total;
}
Result<std::uint64_t> validate_output_resource(const ResourceDescription& d,OutputResourceClass output_class) noexcept {
  switch (output_class) {
  case OutputResourceClass::General:return validate_resource(d);
  case OutputResourceClass::HdrAttachment: {
    constexpr std::uint32_t allowed=ColorTarget|ShaderSample|CopySource;
    if (d.kind!=ResourceKind::Texture2D || d.format!=ResourceFormat::Rgba16Float || d.buffer_bytes ||
        d.mip_levels!=1 || d.samples!=1 || !(d.uses&ColorTarget) || (d.uses&~allowed))
      return fail(ErrorCode::InvalidInput,"HDR attachment requires a mip-1 RGBA16Float color target");
    if (!d.width || !d.height || d.width>4096 || d.height>4096)
      return fail(ErrorCode::Exhausted,"HDR attachment dimensions exceed the viewport bounds");
    const auto payload=std::uint64_t(d.width)*d.height*8;
    if (payload>output_resource_payload_bytes) return fail(ErrorCode::Exhausted,"HDR attachment payload exceeds its 128 MiB class ceiling");
    return payload;
  }
  case OutputResourceClass::HdrReadback:
    if (d.kind!=ResourceKind::Buffer || d.format!=ResourceFormat::Bytes || d.uses!=CopyDestination ||
        d.width!=1 || d.height!=1 || d.mip_levels!=1 || d.samples!=1 || !d.buffer_bytes)
      return fail(ErrorCode::InvalidInput,"HDR readback requires a copy-destination byte buffer");
    if (d.buffer_bytes>output_resource_payload_bytes) return fail(ErrorCode::Exhausted,"HDR readback exceeds its 128 MiB class ceiling");
    return d.buffer_bytes;
  default:return fail(ErrorCode::Unsupported,"unknown output resource class");
  }
}
Result<void> validate_viewport_bindings(std::span<const ResourceBinding> bindings) noexcept {
  if (bindings.size()!=viewport_resource_bindings.size() || !std::equal(bindings.begin(),bindings.end(),viewport_resource_bindings.begin()))
    return fail(ErrorCode::IncompatibleVersion,"viewport binding layout requires exact b0/t0/s0 ABI");
  return {};
}
Result<void> validate_viewport_sampled_format(ResourceFormat format) noexcept {
  if (format!=ResourceFormat::Rgba8Unorm && format!=ResourceFormat::Rgba8Srgb)
    return fail(ErrorCode::Unsupported,"viewport samples only explicit RGBA8 linear or sRGB views");
  return {};
}
AdmittedViewportProgram::AdmittedViewportProgram(ViewportShaderIdentity identity,Digest256 vertex,Digest256 pixel,Digest256 manifest) noexcept
    : identity_(identity),vertex_(vertex),pixel_(pixel),manifest_(manifest) {}
Result<AdmittedViewportProgram> admit_viewport_program(
    std::span<const std::byte> manifest,std::span<const std::byte> vertex,
    std::span<const std::byte> pixel,const ViewportShaderIdentity& expected) noexcept {
  if (manifest.size()!=viewport_shader_manifest_bytes)
    return fail(ErrorCode::InvalidInput,"viewport shader manifest requires exact bounded size");
  if (std::memcmp(manifest.data(),"RACESHD1",8) || u32(manifest,8)!=1 || u32(manifest,12)!=64 ||
      u32(manifest,16)!=640 || u32(manifest,20)!=1 || u32(manifest,24)!=3 || u32(manifest,28)!=2 ||
      u32(manifest,32)!=2021 || u32(manifest,36)!=1 || u32(manifest,40)!=viewport_shader_max_bytes ||
      u32(manifest,44)!=3 || u32(manifest,48)!=176 || !zero(manifest.subspan(52,12)))
    return fail(ErrorCode::IncompatibleVersion,"unsupported viewport shader manifest profile");
  if (!present(expected)) return fail(ErrorCode::InvalidInput,"compiled shader identity is incomplete");
  const ViewportShaderIdentity identity{digest_at(manifest,64),digest_at(manifest,96),digest_at(manifest,128),
      digest_at(manifest,160),digest_at(manifest,208),digest_at(manifest,336)};
  if (identity!=expected) return fail(ErrorCode::IncompatibleVersion,"shader compiler, source or program differs from compiled identity");
  if (!text(manifest.subspan(544,64),"-HV 2021 -WX -Ges -O3"))
    return fail(ErrorCode::IncompatibleVersion,"unsupported viewport shader compilation policy");
  std::array<ResourceBinding,3> bindings;
  for (std::size_t i=0;i<bindings.size();++i) {
    const auto p=448+i*32;
    bindings[i]={static_cast<BindingKind>(u32(manifest,p)),u32(manifest,p+4),u32(manifest,p+8),u32(manifest,p+12),
        u32(manifest,p+16),static_cast<BindingAccess>(u32(manifest,p+20)),u32(manifest,p+24),static_cast<BindingLifetime>(u32(manifest,p+28))};
  }
  auto layout=validate_viewport_bindings(bindings);if (!layout) return layout.error();
  const std::array shaders{vertex,pixel};
  std::array<Digest256,2> revisions;
  constexpr std::array<std::uint32_t,5> offsets{0,64,128,144,160};
  for (std::size_t i=0;i<shaders.size();++i) {
    const auto p=192+i*128;const auto bytes=shaders[i];
    if (bytes.size()<32 || bytes.size()>viewport_shader_max_bytes || u64(manifest,p+8)!=bytes.size())
      return fail(ErrorCode::InvalidInput,"viewport shader byte count differs from admitted bounds");
    if (u32(manifest,p)!=(i==0 ? VertexStage : PixelStage) || u32(manifest,p+4)!=1 ||
        !text(manifest.subspan(p+48,16),i==0 ? "VSMain" : "PSMain") ||
        !text(manifest.subspan(p+64,16),i==0 ? "vs_6_0" : "ps_6_0") ||
        u32(manifest,p+80)!=176 || u32(manifest,p+84)!=2 || !zero(manifest.subspan(p+108,20)))
      return fail(ErrorCode::IncompatibleVersion,"viewport shader stage or reflected interface mismatch");
    for (std::size_t field=0;field<offsets.size();++field)
      if (u32(manifest,p+88+field*4)!=offsets[field])
        return fail(ErrorCode::IncompatibleVersion,"viewport shader constant offset mismatch");
    revisions[i]=digest_at(manifest,p+16);
    if (revisions[i]==Digest256{}) return fail(ErrorCode::InvalidInput,"viewport shader revision is missing");
    if (std::memcmp(bytes.data(),"DXBC",4)) return fail(ErrorCode::InvalidInput,"viewport shader requires a DXIL container");
  }
  if (sha256(manifest.first(608))!=digest_at(manifest,608))
    return fail(ErrorCode::InvalidInput,"viewport shader manifest checksum mismatch");
  if (sha256(vertex)!=revisions[0] || sha256(pixel)!=revisions[1])
    return fail(ErrorCode::InvalidInput,"viewport shader revision differs from reflected manifest");
  return AdmittedViewportProgram(identity,revisions[0],revisions[1],sha256(manifest));
}

namespace {
bool present(const LightOutputShaderIdentity& v) noexcept {
  for (const auto* d:{&v.compiler_executable,&v.compiler_library,&v.compiler_validator,&v.physical_source,&v.output_source,
      &v.legacy_vertex,&v.physical_pixel,&v.output_vertex,&v.output_pixel})
    if (*d==Digest256{}) return false;
  return true;
}
// One 128-byte RACESHD2 stage record: all layout checks precede any shader hashing.
Result<void> stage_record(std::span<const std::byte> manifest,std::size_t p,std::uint32_t stage,std::uint32_t program,
    std::string_view entry,std::string_view target,std::uint32_t constants,std::uint32_t bindings,
    const std::array<std::uint32_t,5>& offsets,std::span<const std::byte> bytes,Digest256& revision) noexcept {
  if (bytes.size()<32 || bytes.size()>viewport_shader_max_bytes || u64(manifest,p+8)!=bytes.size())
    return fail(ErrorCode::InvalidInput,"light/output shader byte count differs from admitted bounds");
  if (u32(manifest,p)!=stage || u32(manifest,p+4)!=program || !text(manifest.subspan(p+48,16),entry) ||
      !text(manifest.subspan(p+64,16),target) || u32(manifest,p+80)!=constants || u32(manifest,p+84)!=bindings ||
      !zero(manifest.subspan(p+108,20)))
    return fail(ErrorCode::IncompatibleVersion,"light/output shader stage or reflected interface mismatch");
  for (std::size_t field=0;field<offsets.size();++field)
    if (u32(manifest,p+88+field*4)!=offsets[field])
      return fail(ErrorCode::IncompatibleVersion,"light/output shader constant offset mismatch");
  revision=digest_at(manifest,p+16);
  if (revision==Digest256{}) return fail(ErrorCode::InvalidInput,"light/output shader revision is missing");
  if (std::memcmp(bytes.data(),"DXBC",4)) return fail(ErrorCode::InvalidInput,"light/output shader requires a DXIL container");
  return {};
}
} // namespace
AdmittedLightOutputPrograms::AdmittedLightOutputPrograms(const LightOutputShaderIdentity& identity,const Digest256& manifest) noexcept
    : identity_(identity),manifest_(manifest) {}
Result<AdmittedLightOutputPrograms> admit_light_output_programs(
    std::span<const std::byte> manifest,std::span<const std::byte> physical_pixel,
    std::span<const std::byte> output_vertex,std::span<const std::byte> output_pixel,
    const LightOutputShaderIdentity& expected,const Digest256& admitted_legacy_vertex) noexcept {
  if (manifest.size()!=light_output_shader_manifest_bytes)
    return fail(ErrorCode::InvalidInput,"light/output shader manifest requires exact bounded size");
  if (std::memcmp(manifest.data(),"RACESHD2",8) || u32(manifest,8)!=2 || u32(manifest,12)!=64 ||
      u32(manifest,16)!=light_output_shader_manifest_bytes || u32(manifest,20)!=2 || u32(manifest,24)!=5 || u32(manifest,28)!=3 ||
      u32(manifest,32)!=2021 || u32(manifest,36)!=1 || u32(manifest,40)!=viewport_shader_max_bytes ||
      u32(manifest,44)!=6 || u32(manifest,48)!=176 || !zero(manifest.subspan(52,12)))
    return fail(ErrorCode::IncompatibleVersion,"unsupported light/output shader manifest profile");
  if (!present(expected)) return fail(ErrorCode::InvalidInput,"compiled light/output shader identity is incomplete");
  LightOutputShaderIdentity identity{digest_at(manifest,64),digest_at(manifest,96),digest_at(manifest,128),
      digest_at(manifest,160),digest_at(manifest,192),digest_at(manifest,224)};
  if (identity.compiler_executable!=expected.compiler_executable || identity.compiler_library!=expected.compiler_library ||
      identity.compiler_validator!=expected.compiler_validator || identity.physical_source!=expected.physical_source ||
      identity.output_source!=expected.output_source || identity.legacy_vertex!=expected.legacy_vertex)
    return fail(ErrorCode::IncompatibleVersion,"light/output shader compiler, sources or vertex link differ from compiled identity");
  if (identity.legacy_vertex!=admitted_legacy_vertex)
    return fail(ErrorCode::IncompatibleVersion,"light/output programs are linked to a different admitted viewport vertex stage");
  if (!text(manifest.subspan(800,64),"-HV 2021 -WX -Ges -O3"))
    return fail(ErrorCode::IncompatibleVersion,"unsupported light/output shader compilation policy");
  std::array<ResourceBinding,5> bindings;
  for (std::size_t i=0;i<bindings.size();++i) {
    const auto p=640+i*32;
    bindings[i]={static_cast<BindingKind>(u32(manifest,p)),u32(manifest,p+4),u32(manifest,p+8),u32(manifest,p+12),
        u32(manifest,p+16),static_cast<BindingAccess>(u32(manifest,p+20)),u32(manifest,p+24),static_cast<BindingLifetime>(u32(manifest,p+28))};
  }
  if (!std::equal(bindings.begin(),bindings.begin()+3,viewport_resource_bindings.begin(),viewport_resource_bindings.end()) ||
      !std::equal(bindings.begin()+3,bindings.end(),output_resource_bindings.begin(),output_resource_bindings.end()))
    return fail(ErrorCode::IncompatibleVersion,"light/output binding layout requires the exact physical b0/t0/s0 and output b0/t0 ABI");
  Digest256 physical,vertex,pixel;
  auto a=stage_record(manifest,256,PixelStage,0,"PSPhysical","ps_6_0",176,3,{0,64,128,144,160},physical_pixel,physical);if (!a) return a.error();
  auto b=stage_record(manifest,384,VertexStage,1,"VSOutput","vs_6_0",0,0,{0,0,0,0,0},output_vertex,vertex);if (!b) return b.error();
  auto c=stage_record(manifest,512,PixelStage,1,"PSOutput","ps_6_0",32,2,{0,16,0,0,0},output_pixel,pixel);if (!c) return c.error();
  if (sha256(manifest.first(864))!=digest_at(manifest,864))
    return fail(ErrorCode::InvalidInput,"light/output shader manifest checksum mismatch");
  if (sha256(physical_pixel)!=physical || sha256(output_vertex)!=vertex || sha256(output_pixel)!=pixel)
    return fail(ErrorCode::InvalidInput,"light/output shader revision differs from reflected manifest");
  identity.physical_pixel=physical;identity.output_vertex=vertex;identity.output_pixel=pixel;
  if (identity!=expected) return fail(ErrorCode::IncompatibleVersion,"light/output shader programs differ from compiled identity");
  return AdmittedLightOutputPrograms(identity,sha256(manifest));
}
} // namespace race::render
