#include "race/render/resource_contract.hpp"
#include "test.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

using namespace race;
using namespace race::render;
namespace {
std::span<const std::byte> bytes(std::string_view s) { return std::as_bytes(std::span(s)); }
Digest256 unhex(std::string_view s) {
  CHECK(s.size()==64);Digest256 d;
  for (std::size_t i=0;i<d.size();++i) {
    auto digit=[](char c){return static_cast<unsigned>(c<='9' ? c-'0' : c-'a'+10);};
    d[i]=std::byte((digit(s[2*i])<<4)|digit(s[2*i+1]));
  }
  return d;
}
void hashes() {
  CHECK(sha256({})==unhex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
  // Independent Python hashlib vectors at SHA padding and block boundaries.
  CHECK(sha256(bytes(std::string(1, 'a')))==unhex("ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb"));
  CHECK(sha256(bytes(std::string(55, 'a')))==unhex("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
  CHECK(sha256(bytes(std::string(56, 'a')))==unhex("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
  CHECK(sha256(bytes(std::string(63, 'a')))==unhex("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"));
  CHECK(sha256(bytes(std::string(64, 'a')))==unhex("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
  CHECK(sha256(bytes(std::string(65, 'a')))==unhex("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"));
  CHECK(sha256(bytes(std::string(119, 'a')))==unhex("31eba51c313a5c08226adf18d4a359cfdfd8d2e816b13f4af952f7ea6584dcfb"));
  CHECK(sha256(bytes(std::string(120, 'a')))==unhex("2f3d335432c70b580af0e8e1b3674a7c020d683aa5f73aaaedfdc55af904c21c"));
  CHECK(sha256(bytes(std::string(127, 'a')))==unhex("c57e9278af78fa3cab38667bef4ce29d783787a2f731d4e12200270f0c32320a"));
  CHECK(sha256(bytes(std::string(128, 'a')))==unhex("6836cf13bac400e9105071cd6af47084dfacad4e5e302c94bfed24e013afb73e"));
  CHECK(sha256(bytes(std::string(129, 'a')))==unhex("c12cb024a2e5551cca0e08fce8f1c5e314555cc3fef6329ee994a3db752166ae"));
  CHECK(sha256(bytes("abc"))==unhex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
  CHECK(sha256(bytes("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))==
      unhex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
  const std::string million(1000000,'a');
  CHECK(sha256(bytes(million))==unhex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}
void resources() {
  ResourceDescription buffer;buffer.buffer_bytes=176;buffer.uses=ConstantInput|CopyDestination;
  CHECK(validate_resource(buffer).value()==176);
  auto bad=buffer;bad.buffer_bytes=175;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=buffer;bad.buffer_bytes=std::numeric_limits<std::uint64_t>::max();CHECK_ERROR(validate_resource(bad),ErrorCode::Exhausted);
  bad=buffer;bad.uses=ShaderSample;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=buffer;bad.uses=0;CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=buffer;bad.uses=1u<<31;CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=buffer;bad.kind=static_cast<ResourceKind>(0);CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=buffer;bad.samples=0;CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=buffer;bad.width=2;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  ResourceDescription texture{ResourceKind::Texture2D,ResourceFormat::Rgba8Srgb,ShaderSample|CopyDestination,0,3,5,3,1};
  CHECK(validate_resource(texture).value()==72); // 3x5 + 1x2 + 1x1, independent NPOT expectation.
  texture.format=ResourceFormat::Rgba8Unorm;CHECK(validate_resource(texture).value()==72);
  texture.format=ResourceFormat::Rgba16Float;CHECK(validate_resource(texture).value()==144);
  texture.format=ResourceFormat::Depth32Float;texture.uses=DepthTarget;CHECK(validate_resource(texture).value()==72);
  bad=texture;bad.uses|=ShaderSample;CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=texture;bad.format=ResourceFormat::Rgba8Unorm;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=texture;bad.format=static_cast<ResourceFormat>(UINT32_MAX);CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  bad=texture;bad.mip_levels=4;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=texture;bad.mip_levels=14;CHECK_ERROR(validate_resource(bad),ErrorCode::Exhausted);
  bad=texture;bad.width=UINT32_MAX;CHECK_ERROR(validate_resource(bad),ErrorCode::Exhausted);
  bad=texture;bad.height=0;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=texture;bad.buffer_bytes=1;CHECK_ERROR(validate_resource(bad),ErrorCode::InvalidInput);
  bad=texture;bad.samples=2;CHECK_ERROR(validate_resource(bad),ErrorCode::Unsupported);
  CHECK_ERROR(validate_resource(texture,{71,4096,13}),ErrorCode::Exhausted);
  CHECK_ERROR(validate_resource(texture,{64*1024*1024+1,4096,13}),ErrorCode::InvalidInput);
  CHECK_ERROR(validate_resource(texture,{72,4097,13}),ErrorCode::InvalidInput);
  CHECK_ERROR(validate_resource(texture,{72,4096,0}),ErrorCode::InvalidInput);
  texture={ResourceKind::Texture2D,ResourceFormat::Rgba8Unorm,ColorTarget,0,4096,4096,1,1};
  CHECK(validate_resource(texture).value()==67108864);
  texture.format=ResourceFormat::Rgba16Float;CHECK_ERROR(validate_resource(texture),ErrorCode::Exhausted);
  CHECK(validate_viewport_sampled_format(ResourceFormat::Rgba8Srgb));
  CHECK(validate_viewport_sampled_format(ResourceFormat::Rgba8Unorm));
  CHECK_ERROR(validate_viewport_sampled_format(ResourceFormat::Rgba16Float),ErrorCode::Unsupported);
  CHECK_ERROR(validate_viewport_sampled_format(static_cast<ResourceFormat>(UINT32_MAX)),ErrorCode::Unsupported);
  CHECK(validate_viewport_bindings(viewport_resource_bindings));
  auto bindings=viewport_resource_bindings;bindings[1].count=2;
  CHECK_ERROR(validate_viewport_bindings(bindings),ErrorCode::IncompatibleVersion);
  bindings=viewport_resource_bindings;bindings[1].space=1;
  CHECK_ERROR(validate_viewport_bindings(bindings),ErrorCode::IncompatibleVersion);
  bindings=viewport_resource_bindings;std::swap(bindings[1],bindings[2]);
  CHECK_ERROR(validate_viewport_bindings(bindings),ErrorCode::IncompatibleVersion);
  CHECK_ERROR(validate_viewport_bindings(std::span(bindings).first(2)),ErrorCode::IncompatibleVersion);
}
// This independent wire fixture deliberately does not use a production encoder.
struct Fixture {
  std::array<std::byte,640> manifest{};
  std::array<std::byte,64> vertex{},pixel{};
  ViewportShaderIdentity identity;
  void put(std::size_t p,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) manifest[p+i]=std::byte((value>>(8*i))&255);
  }
  void copy(std::size_t p,std::span<const std::byte> value) { std::copy(value.begin(),value.end(),manifest.begin()+static_cast<std::ptrdiff_t>(p)); }
  void seal() { copy(608,sha256(std::span(manifest).first(608))); }
  Fixture() {
    std::memcpy(vertex.data(),"DXBC",4);std::memcpy(pixel.data(),"DXBC",4);
    for (unsigned i=4;i<64;++i) { vertex[i]=std::byte(i);pixel[i]=std::byte(i+60); }
    identity={sha256(bytes("compiler executable")),sha256(bytes("compiler library")),sha256(bytes("compiler validator")),sha256(bytes("shader source")),sha256(vertex),sha256(pixel)};
    copy(0,bytes("RACESHD1"));
    const std::array<std::uint32_t,11> header{1,64,640,1,3,2,2021,1,4194304,3,176};
    for (std::size_t i=0;i<header.size();++i) put(8+i*4,header[i]);
    copy(64,identity.compiler_executable);copy(96,identity.compiler_library);copy(128,identity.compiler_validator);copy(160,identity.source);
    for (unsigned i=0;i<2;++i) {
      const auto at=192+i*128;put(at,i+1);put(at+4,1);put(at+8,64);
      copy(at+16,sha256(i==0 ? std::span(vertex) : std::span(pixel)));
      copy(at+48,bytes(i==0 ? "VSMain" : "PSMain"));copy(at+64,bytes(i==0 ? "vs_6_0" : "ps_6_0"));
      const std::array<std::uint32_t,7> layout{176,2,0,64,128,144,160};
      for (std::size_t j=0;j<layout.size();++j) put(at+80+j*4,layout[j]);
    }
    const std::array<std::array<std::uint32_t,8>,3> bindings{{{1,0,0,1,3,1,176,1},{2,0,0,1,2,1,0,1},{3,0,0,1,2,1,0,1}}};
    for (std::size_t i=0;i<3;++i) for (std::size_t j=0;j<8;++j) put(448+i*32+j*4,bindings[i][j]);
    copy(544,bytes("-HV 2021 -WX -Ges -O3"));seal();
  }
  Result<AdmittedViewportProgram> admit() const { return admit_viewport_program(manifest,vertex,pixel,identity); }
};
void manifests() {
  Fixture f;auto accepted=f.admit();CHECK(accepted);
  // Python hashlib + struct oracle, not the production SHA implementation.
  CHECK(sha256(f.manifest)==unhex("e2608d44f67ec871d4236f8664c7322f6aaa726d25d815cc67603390550d282f"));
  CHECK(accepted.value().identity()==f.identity);
  CHECK(accepted.value().vertex_digest()==sha256(f.vertex));
  CHECK(accepted.value().pixel_digest()==sha256(f.pixel));
  CHECK(validate_viewport_bindings(accepted.value().bindings()));
  const auto saved=accepted.value().manifest_digest();
  for (std::size_t n=0;n<f.manifest.size();++n)
    CHECK_ERROR(admit_viewport_program(std::span(f.manifest).first(n),f.vertex,f.pixel,f.identity),ErrorCode::InvalidInput);
  std::array<std::byte,641> over{};std::copy(f.manifest.begin(),f.manifest.end(),over.begin());
  CHECK_ERROR(admit_viewport_program(over,f.vertex,f.pixel,f.identity),ErrorCode::InvalidInput);
  for (std::size_t at=0;at<f.manifest.size();++at) {
    auto damaged=f;damaged.manifest[at]^=std::byte{1};CHECK(!damaged.admit());
  }
  // Recomputed checksums must not admit malformed semantic fields or padding.
  for (const auto at:{8u,12u,16u,20u,24u,28u,32u,36u,40u,44u,48u,52u,60u,
      192u,196u,204u,240u,255u,256u,271u,272u,276u,280u,284u,288u,292u,296u,300u,316u,
      320u,324u,332u,368u,383u,384u,399u,400u,404u,408u,412u,416u,420u,424u,428u,444u,
      448u,452u,456u,460u,464u,468u,472u,476u,480u,484u,488u,492u,496u,500u,504u,508u,
      512u,516u,520u,524u,528u,532u,536u,540u,544u,607u}) {
    auto bad=f;bad.manifest[at]^=std::byte{1};bad.seal();CHECK(!bad.admit());
  }
  auto bad=f;bad.identity.source[0]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::IncompatibleVersion);
  bad=f;bad.identity.compiler_library={};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.vertex[12]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.pixel[63]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.pixel[63]^=std::byte{1};bad.copy(336,sha256(bad.pixel));bad.seal();
  CHECK_ERROR(bad.admit(),ErrorCode::IncompatibleVersion); // A self-consistent forged variant cannot change the compiled pin.
  bad=f;bad.vertex[0]=std::byte{};bad.identity.vertex=sha256(bad.vertex);bad.copy(208,bad.identity.vertex);bad.seal();CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  CHECK_ERROR(admit_viewport_program(f.manifest,std::span(f.vertex).first(31),f.pixel,f.identity),ErrorCode::InvalidInput);
  const std::vector<std::byte> oversized(viewport_shader_max_bytes+1);
  CHECK_ERROR(admit_viewport_program(f.manifest,oversized,f.pixel,f.identity),ErrorCode::InvalidInput);
  std::fill(f.manifest.begin(),f.manifest.end(),std::byte{});std::fill(f.vertex.begin(),f.vertex.end(),std::byte{});
  CHECK(accepted.value().manifest_digest()==saved); // Candidate retained values, no borrowed inputs.
}
// P03.04 RACESHD2 wire fixture, again independent of the production encoder.
struct LightFixture {
  std::array<std::byte,896> manifest{};
  std::array<std::byte,64> legacy_vertex{},physical{},vertex{},pixel{};
  LightOutputShaderIdentity identity;
  void put(std::size_t p,std::uint32_t value) {
    for (unsigned i=0;i<4;++i) manifest[p+i]=std::byte((value>>(8*i))&255);
  }
  void copy(std::size_t p,std::span<const std::byte> value) { std::copy(value.begin(),value.end(),manifest.begin()+static_cast<std::ptrdiff_t>(p)); }
  void seal() { copy(864,sha256(std::span(manifest).first(864))); }
  LightFixture() {
    for (auto* shader:{&legacy_vertex,&physical,&vertex,&pixel}) std::memcpy(shader->data(),"DXBC",4);
    for (unsigned i=4;i<64;++i) { legacy_vertex[i]=std::byte(i);physical[i]=std::byte(i+10);vertex[i]=std::byte(i+20);pixel[i]=std::byte(i+30); }
    identity={sha256(bytes("compiler executable")),sha256(bytes("compiler library")),sha256(bytes("compiler validator")),
        sha256(bytes("physical shader source")),sha256(bytes("output shader source")),sha256(legacy_vertex),
        sha256(physical),sha256(vertex),sha256(pixel)};
    copy(0,bytes("RACESHD2"));
    const std::array<std::uint32_t,11> header{2,64,896,2,5,3,2021,1,4194304,6,176};
    for (std::size_t i=0;i<header.size();++i) put(8+i*4,header[i]);
    copy(64,identity.compiler_executable);copy(96,identity.compiler_library);copy(128,identity.compiler_validator);
    copy(160,identity.physical_source);copy(192,identity.output_source);copy(224,identity.legacy_vertex);
    struct Stage { std::size_t at;std::uint32_t kind,program;const std::array<std::byte,64>* shader;const char* entry;const char* target;std::array<std::uint32_t,7> layout; };
    const std::array<Stage,3> stages{{{256,2,0,&physical,"PSPhysical","ps_6_0",{176,3,0,64,128,144,160}},
        {384,1,1,&vertex,"VSOutput","vs_6_0",{0,0,0,0,0,0,0}},{512,2,1,&pixel,"PSOutput","ps_6_0",{32,2,0,16,0,0,0}}}};
    for (const auto& s:stages) {
      put(s.at,s.kind);put(s.at+4,s.program);put(s.at+8,64);copy(s.at+16,sha256(*s.shader));
      copy(s.at+48,bytes(s.entry));copy(s.at+64,bytes(s.target));
      for (std::size_t j=0;j<s.layout.size();++j) put(s.at+80+j*4,s.layout[j]);
    }
    const std::array<std::array<std::uint32_t,8>,5> bindings{{{1,0,0,1,3,1,176,1},{2,0,0,1,2,1,0,1},{3,0,0,1,2,1,0,1},{1,0,0,1,2,1,32,1},{2,0,0,1,2,1,0,1}}};
    for (std::size_t i=0;i<bindings.size();++i) for (std::size_t j=0;j<8;++j) put(640+i*32+j*4,bindings[i][j]);
    copy(800,bytes("-HV 2021 -WX -Ges -O3"));seal();
  }
  Result<AdmittedLightOutputPrograms> admit() const { return admit_light_output_programs(manifest,physical,vertex,pixel,identity,sha256(legacy_vertex)); }
};
void light_output_manifests() {
  LightFixture f;auto accepted=f.admit();CHECK(accepted);
  // Python hashlib + struct oracle of the same wire layout, not the production SHA implementation.
  CHECK(sha256(f.manifest)==unhex("6fa2805ac6fb9c7825bb507cb380f4f69f5b7eb8cd9dfadbc55654428ca8abd8"));
  CHECK(accepted.value().identity()==f.identity);
  CHECK(accepted.value().physical_pixel_digest()==sha256(f.physical) && accepted.value().output_vertex_digest()==sha256(f.vertex) &&
      accepted.value().output_pixel_digest()==sha256(f.pixel));
  CHECK(validate_viewport_bindings(accepted.value().physical_bindings()));
  const auto output=accepted.value().output_bindings();
  CHECK(output.size()==2 && output[0].kind==BindingKind::Constants && output[0].constant_bytes==32 && output[0].visibility==PixelStage &&
      output[1].kind==BindingKind::SampledTexture && output[1].register_index==0 && output[1].visibility==PixelStage);
  const auto saved=accepted.value().manifest_digest();
  // The legacy admission is untouched by the second profile: RACESHD1 bytes do not admit as RACESHD2 and vice versa.
  Fixture legacy;CHECK(legacy.admit());
  CHECK_ERROR(admit_light_output_programs(legacy.manifest,f.physical,f.vertex,f.pixel,f.identity,sha256(f.legacy_vertex)),ErrorCode::InvalidInput);
  CHECK_ERROR(admit_viewport_program(std::span(f.manifest).first(640),f.vertex,f.pixel,legacy.identity),ErrorCode::IncompatibleVersion);
  for (std::size_t n=0;n<f.manifest.size();++n)
    CHECK_ERROR(admit_light_output_programs(std::span(f.manifest).first(n),f.physical,f.vertex,f.pixel,f.identity,sha256(f.legacy_vertex)),ErrorCode::InvalidInput);
  std::array<std::byte,897> over{};std::copy(f.manifest.begin(),f.manifest.end(),over.begin());
  CHECK_ERROR(admit_light_output_programs(over,f.physical,f.vertex,f.pixel,f.identity,sha256(f.legacy_vertex)),ErrorCode::InvalidInput);
  for (std::size_t at=0;at<f.manifest.size();++at) {
    auto damaged=f;damaged.manifest[at]^=std::byte{1};CHECK(!damaged.admit());
  }
  // Recomputed checksums must not admit malformed semantic fields or padding.
  std::vector<std::size_t> resealed{8,12,16,20,24,28,32,36,40,44,48,52,60,64,100,160,200,224,255,800,863};
  for (const std::size_t base:{256u,384u,512u}) for (const std::size_t field:{0u,4u,8u,16u,48u,63u,64u,79u,80u,84u,88u,92u,96u,100u,104u,108u,127u}) resealed.push_back(base+field);
  for (std::size_t at=640;at<800;at+=4) resealed.push_back(at);
  for (const auto at:resealed) {
    auto bad=f;bad.manifest[at]^=std::byte{1};bad.seal();CHECK(!bad.admit());
  }
  // The physical pixel stage is bound to one admitted vertex interface.
  CHECK_ERROR(admit_light_output_programs(f.manifest,f.physical,f.vertex,f.pixel,f.identity,sha256(f.physical)),ErrorCode::IncompatibleVersion);
  auto bad=f;bad.identity.output_source[0]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::IncompatibleVersion);
  bad=f;bad.identity.legacy_vertex={};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.identity.output_pixel[5]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::IncompatibleVersion);
  bad=f;bad.physical[12]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.vertex[40]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.pixel[63]^=std::byte{1};CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  bad=f;bad.pixel[63]^=std::byte{1};bad.copy(528,sha256(bad.pixel));bad.seal();
  CHECK_ERROR(bad.admit(),ErrorCode::IncompatibleVersion); // Self-consistent forgery cannot change the compiled pin.
  bad=f;bad.vertex[0]=std::byte{};bad.identity.output_vertex=sha256(bad.vertex);bad.copy(400,bad.identity.output_vertex);bad.seal();
  CHECK_ERROR(bad.admit(),ErrorCode::InvalidInput);
  CHECK_ERROR(admit_light_output_programs(f.manifest,std::span(f.physical).first(31),f.vertex,f.pixel,f.identity,sha256(f.legacy_vertex)),ErrorCode::InvalidInput);
  const std::vector<std::byte> oversized(viewport_shader_max_bytes+1);
  CHECK_ERROR(admit_light_output_programs(f.manifest,f.physical,oversized,f.pixel,f.identity,sha256(f.legacy_vertex)),ErrorCode::InvalidInput);
  std::fill(f.manifest.begin(),f.manifest.end(),std::byte{});std::fill(f.pixel.begin(),f.pixel.end(),std::byte{});
  CHECK(accepted.value().manifest_digest()==saved); // Candidate retained values, no borrowed inputs.
}
} // namespace
int main() { hashes();resources();manifests();light_output_manifests();std::puts("render resource and shader admission contracts: passed"); }
