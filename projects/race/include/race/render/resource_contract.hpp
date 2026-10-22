#pragma once
#include "race/foundation/digest.hpp"
#include "race/foundation/result.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace race::render {
// Portable descriptions of the resources used by the bounded raster viewport.
// Validation describes intended use; device format support is separately admitted.
enum class ResourceKind : std::uint32_t { Buffer=1, Texture2D=2 };
enum class ResourceFormat : std::uint32_t {
  Bytes=1, Rgba8Unorm=2, Rgba8Srgb=3, Rgba16Float=4, Depth32Float=5
};
enum ResourceUse : std::uint32_t {
  CopySource=1, CopyDestination=2, VertexInput=4, ConstantInput=8,
  ShaderSample=16, ColorTarget=32, DepthTarget=64
};
struct ResourceDescription {
  ResourceKind kind{ResourceKind::Buffer};
  ResourceFormat format{ResourceFormat::Bytes};
  std::uint32_t uses{CopySource};
  std::uint64_t buffer_bytes{}; // Must be zero for a texture.
  std::uint32_t width{1},height{1},mip_levels{1},samples{1};
};
struct ResourceLimits {
  std::uint64_t payload_bytes{64*1024*1024};
  std::uint32_t max_dimension{4096},max_mip_levels{13};
};
// Returns tightly packed logical payload bytes, not GPU allocation/row padding.
Result<std::uint64_t> validate_resource(const ResourceDescription&,const ResourceLimits& = {}) noexcept;
// Separately tagged admission for the PhysicalDiffuseV1 output set only. General keeps the
// 64 MiB ceiling above. HdrAttachment is a mip-1, single-sample RGBA16Float texture used as a
// color target (optionally sampled/copied); HdrReadback is a copy-destination readback byte
// buffer. Each has a 128 MiB logical ceiling; neither can substitute for a material texture,
// vertex, constant or upload resource.
enum class OutputResourceClass : std::uint32_t { General=0, HdrAttachment=1, HdrReadback=2 };
inline constexpr std::uint64_t output_resource_payload_bytes=128ull*1024*1024;
Result<std::uint64_t> validate_output_resource(const ResourceDescription&,OutputResourceClass) noexcept;

enum class BindingKind : std::uint32_t { Constants=1, SampledTexture=2, Sampler=3 };
enum ShaderVisibility : std::uint32_t { VertexStage=1, PixelStage=2 };
enum class BindingAccess : std::uint32_t { ReadOnly=1 };
enum class BindingLifetime : std::uint32_t { Submission=1 };
struct ResourceBinding {
  BindingKind kind{};
  std::uint32_t register_index{},space{},count{1},visibility{};
  BindingAccess access{BindingAccess::ReadOnly};
  std::uint32_t constant_bytes{};
  BindingLifetime lifetime{BindingLifetime::Submission};
  friend bool operator==(const ResourceBinding&,const ResourceBinding&) = default;
};
inline constexpr std::array<ResourceBinding,3> viewport_resource_bindings{{
  {BindingKind::Constants,0,0,1,VertexStage|PixelStage,BindingAccess::ReadOnly,176,BindingLifetime::Submission},
  {BindingKind::SampledTexture,0,0,1,PixelStage,BindingAccess::ReadOnly,0,BindingLifetime::Submission},
  {BindingKind::Sampler,0,0,1,PixelStage,BindingAccess::ReadOnly,0,BindingLifetime::Submission}
}};
Result<void> validate_viewport_bindings(std::span<const ResourceBinding>) noexcept;
// SRV format determines decoding. Linear scalar bytes must never take the sRGB path.
Result<void> validate_viewport_sampled_format(ResourceFormat) noexcept;

inline constexpr std::size_t viewport_shader_manifest_bytes=640;
inline constexpr std::size_t viewport_shader_max_bytes=4*1024*1024;
struct ViewportShaderIdentity {
  Digest256 compiler_executable{},compiler_library{},compiler_validator{},source{},vertex{},pixel{};
  friend bool operator==(const ViewportShaderIdentity&,const ViewportShaderIdentity&) = default;
};
// Value-only candidate: no file paths, borrowed shader/manifest bytes or native handles.
// A backend publishes its pipeline only after this admission AND native creation pass.
class AdmittedViewportProgram {
  ViewportShaderIdentity identity_;
  Digest256 vertex_{},pixel_{},manifest_{};
  AdmittedViewportProgram(ViewportShaderIdentity,Digest256,Digest256,Digest256) noexcept;
  friend Result<AdmittedViewportProgram> admit_viewport_program(
      std::span<const std::byte>,std::span<const std::byte>,std::span<const std::byte>,
      const ViewportShaderIdentity&) noexcept;
public:
  const ViewportShaderIdentity& identity() const noexcept { return identity_; }
  const Digest256& vertex_digest() const noexcept { return vertex_; }
  const Digest256& pixel_digest() const noexcept { return pixel_; }
  const Digest256& manifest_digest() const noexcept { return manifest_; }
  std::span<const ResourceBinding> bindings() const noexcept { return viewport_resource_bindings; }
};
// RACESHD1: 64-byte header, four identities, two 128-byte stages, three
// 32-byte bindings, 64-byte compile-policy string, SHA-256 of the first 608 bytes.
// Fixed little-endian wire. Unknown versions, flags, layouts and reserved bytes
// fail closed. The expected identity comes from the compiled build, not this file.
// No allocations or filesystem I/O; all size/layout checks precede shader hashing.
Result<AdmittedViewportProgram> admit_viewport_program(
    std::span<const std::byte> manifest,std::span<const std::byte> vertex,
    std::span<const std::byte> pixel,const ViewportShaderIdentity& expected) noexcept;

// P03.04 light/output programs (docs/progress/p03-light-output-contracts-20261013.md).
// The physical pixel stage keeps the legacy 176-byte b0/t0/s0 ABI and is linked by
// digest to the separately admitted legacy vertex stage; the fullscreen output
// program has its own reflected ABI: b0 32 bytes and the FP16 scene-linear texture
// at t0, pixel stage only, read by exact pixel Load.
inline constexpr std::array<ResourceBinding,2> output_resource_bindings{{
  {BindingKind::Constants,0,0,1,PixelStage,BindingAccess::ReadOnly,32,BindingLifetime::Submission},
  {BindingKind::SampledTexture,0,0,1,PixelStage,BindingAccess::ReadOnly,0,BindingLifetime::Submission}
}};
inline constexpr std::size_t light_output_shader_manifest_bytes=896;
struct LightOutputShaderIdentity {
  Digest256 compiler_executable{},compiler_library{},compiler_validator{},physical_source{},output_source{},
      legacy_vertex{},physical_pixel{},output_vertex{},output_pixel{};
  friend bool operator==(const LightOutputShaderIdentity&,const LightOutputShaderIdentity&) = default;
};
class AdmittedLightOutputPrograms {
  LightOutputShaderIdentity identity_;
  Digest256 manifest_{};
  AdmittedLightOutputPrograms(const LightOutputShaderIdentity&,const Digest256&) noexcept;
  friend Result<AdmittedLightOutputPrograms> admit_light_output_programs(
      std::span<const std::byte>,std::span<const std::byte>,std::span<const std::byte>,std::span<const std::byte>,
      const LightOutputShaderIdentity&,const Digest256&) noexcept;
public:
  const LightOutputShaderIdentity& identity() const noexcept { return identity_; }
  const Digest256& physical_pixel_digest() const noexcept { return identity_.physical_pixel; }
  const Digest256& output_vertex_digest() const noexcept { return identity_.output_vertex; }
  const Digest256& output_pixel_digest() const noexcept { return identity_.output_pixel; }
  const Digest256& manifest_digest() const noexcept { return manifest_; }
  std::span<const ResourceBinding> physical_bindings() const noexcept { return viewport_resource_bindings; }
  std::span<const ResourceBinding> output_bindings() const noexcept { return output_resource_bindings; }
};
// RACESHD2: 64-byte header, six identities (compiler x3, physical and output sources,
// legacy vertex stage), three 128-byte stages (physical pixel, output vertex, output
// pixel), five 32-byte bindings, 64-byte compile-policy string, SHA-256 of the first
// 864 bytes. Same fixed little-endian, fail-closed discipline as RACESHD1.
// `admitted_legacy_vertex` is the digest of the already admitted viewport vertex stage;
// the manifest and the compiled identity must both name it, so the physical pixel
// stage can never be paired with a different vertex interface.
Result<AdmittedLightOutputPrograms> admit_light_output_programs(
    std::span<const std::byte> manifest,std::span<const std::byte> physical_pixel,
    std::span<const std::byte> output_vertex,std::span<const std::byte> output_pixel,
    const LightOutputShaderIdentity& expected,const Digest256& admitted_legacy_vertex) noexcept;
} // namespace race::render
