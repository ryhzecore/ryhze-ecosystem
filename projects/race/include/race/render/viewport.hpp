#pragma once
#include "race/foundation/digest.hpp"
#include "race/foundation/identity.hpp"
#include "race/render/light_output.hpp"
#include "race/render/viewport_geometry.hpp"
#include "race/render/viewport_material.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <optional>
#include <vector>

namespace race::render {
inline constexpr std::uint32_t viewport_max_dimension = 4096;
inline constexpr std::size_t viewport_max_draws = 1024;
inline constexpr std::size_t viewport_max_overlay_draws = 8;
inline constexpr std::size_t viewport_max_overlay_vertices = 4096;
struct ViewportVec3 { double x{}, y{}, z{}; };
// Absolute double-precision world metres; Render alone converts to camera-relative floats.
struct ViewportCamera {
  ViewportVec3 eye{6, 5, 8}, target{0, 0, 0}, up{0, 1, 0};
  double vertical_fov_radians{0.7853981633974483}, near_metres{0.05}, far_metres{2000};
};
struct DrawPrimitive {
  PersistentId id;
  // Local geometry (unit cube [-0.5,0.5] if absent), absolute double world metres. Row-major affine, column-vector convention:
  // x'=m[0]x+m[1]y+m[2]z+m[3], matching the Scene-owned copied Affine.
  std::array<double, 12> world{1,0,0,0, 0,1,0,0, 0,0,1,0};
  std::array<float, 4> color{0.35f,0.55f,0.8f,1}; // Linear RGB, opaque only.
  bool selected{};
  std::shared_ptr<const ViewportGeometry> geometry;
  std::shared_ptr<const ViewportMaterial> material;
};
// Opaque, unlit triangles in normalized device coordinates, drawn in order on
// top of Scene geometry. Each draw owns complete nonempty triangles; finite x/y
// are bounded to [-16,16] and clipped by the viewport. Colors use linear RGB.
struct OverlayDraw {
  std::array<float,4> color{1,1,1,1};
  std::vector<std::array<float,2>> vertices;
};
struct RenderFrame {
  // Scene origin revision is provenance only: do not pre-subtract a Scene origin
  // from world transforms or camera positions. GPU conversion is backend-owned.
  std::uint64_t scene_generation{}, origin_revision{1}, simulation_tick{};
  ViewportCamera camera;
  std::vector<DrawPrimitive> draws; // Owned copies; no Scene pointer or ReadView.
  ViewportVec3 direction_to_light{0.4,0.8,0.3};
  float directional_intensity{0.9f}, ambient_intensity{0.22f};
  std::array<float,4> clear_color{0.015f,0.02f,0.032f,1}; // Linear RGB.
  bool grid{true};
  std::vector<OverlayDraw> overlays; // Owned screen-space triangle lists, no UI pointers.
  // One lighting profile per frame. LegacyViewportV1 uses the unitless fields above and
  // requires `physical` to stay default; PhysicalDiffuseV1 uses `physical` (SI units,
  // light_output.hpp) and ignores direction_to_light, the two intensities and clear_color.
  LightingProfile lighting_profile{LightingProfile::LegacyViewportV1};
  PhysicalLighting physical{};
};
// Epoch/frame identities are scoped to one stable viewport owner, not globally unique.
struct GpuTimingSample { std::uint64_t frame{}; double milliseconds{}; std::uint64_t device_epoch{}; };
enum class ViewportState : std::uint8_t { Unavailable, Ready, SurfaceRetired, Removed };
enum class ViewportRecreation : std::uint8_t { Initial, Controlled, SurfaceRestore, DeviceRemoval };
struct ViewportRetirement {
  std::uint64_t device_epoch{}, submitted_frames{}, completed_frames{};
  std::int64_t operation_error{}, removal_reason{};
  bool removed{}, removal_from_fence{};
  std::uint64_t debug_errors{},debug_warnings{},debug_messages_discarded{};
  bool diagnostics_incomplete{};
  std::array<char,512> last_debug_message{};
};
// Successful admission and actual command-list submissions are separate: native
// preparation may fail after compile, and Signal may fail after submission.
// Counts, operation mask and compile time describe the most recent submission.
// Operation bits use the portable graph::Operation numeric values (1..8).
struct ViewportGraphStats {
  std::uint64_t compiles{}, submissions{};
  std::uint32_t resources{}, passes{}, uses{}, transitions{}, operation_mask{};
  double last_compile_cpu_ms{};
};
struct ViewportStats {
  // Lifecycle survives native-device replacement. Old images/timestamps are never current.
  std::uint64_t device_epoch{}, recreation_attempts{};
  std::uint32_t removal_recovery_attempts{}; // At most one automatic attempt per owner.
  ViewportState state{ViewportState::Unavailable};
  ViewportRecreation recreation{ViewportRecreation::Initial};
  ViewportRetirement retired;
  std::optional<Error> lifecycle_error;
  std::int64_t device_removal_reason{}; // Actual GetDeviceRemovedReason result; distinct from operation HRESULT.
  bool removal_from_fence{}, has_image{};
  std::string adapter;
  std::uint32_t vendor_id{}, device_id{}, width{}, height{};
  std::uint64_t dedicated_video_bytes{}, submitted_frames{}, completed_frames{};
  std::uint64_t scene_generation{}, origin_revision{}, simulation_tick{};
  std::uint64_t last_draw_count{}, debug_warnings{}, debug_errors{}; // Cumulative owner observations across epochs.
  std::uint64_t debug_messages_discarded{};
  bool diagnostics_incomplete{}; // Shared queue reset/read failure/discard; never a clean-debug claim.
  std::uint64_t overlay_draws{}, overlay_vertices{}; // Most recent submitted frame.
  std::uint64_t texture_uploads{}, texture_upload_bytes{}; // Cumulative submitted copies, padded footprints.
  std::uint64_t texture_cache_hits{}; // Submitted slot-resident unique packet uses without a copy.
  std::uint64_t frame_textures{}, frame_texture_bytes{}; // Unique packets in most recent submitted frame.
  ViewportGraphStats draw_graph, capture_graph;
  // Fixed adapter graph/texture-preflight storage and its included Plan capacity.
  // Excludes native allocations, allocator metadata and compile call-stack storage.
  std::uint64_t graph_retained_bytes{}, graph_plan_bytes{};
  std::int64_t last_native_error{};
  std::array<char,512> last_debug_message{};
  // Latest completed timestamp pair, identified independently of CPU submission.
  // GPU scope: texture uploads, clears, raster draws and color copy;
  // excludes Present/fence wait.
  double gpu_milliseconds{};
  std::uint64_t last_timed_frame{};
  // Oldest-to-newest last four completed samples; poll every draw and after drain
  // to collect an entire run. Frame=0 is not a valid submitted frame.
  std::array<GpuTimingSample,4> gpu_samples{};
  std::size_t gpu_sample_count{};
  // CPU scopes for last_cpu_frame only, milliseconds from steady_clock. Validation
  // includes owned-input checks/conversion; record includes texture preparation,
  // mesh upload and allocator/list reset through Close; execute/Present surround those API calls; signal includes its
  // fence identity bookkeeping. Rejected/minimized calls do not advance CPU IDs.
  // Inflight wait includes fence setup and retired timestamp collection; the
  // pending subset measures fence setup/wait only when incomplete on entry.
  // Completion includes nonblocking timestamp collection and debug-message drain.
  // Draw spans API entry through completion, excluding these stats assignments
  // and caller extraction/input. Inflight wait count records pending-on-entry fences.
  std::uint64_t last_cpu_frame{}, inflight_wait_count{};
  double last_validation_cpu_ms{}, last_inflight_wait_cpu_ms{}, last_pending_fence_cpu_ms{};
  double last_record_cpu_ms{}, last_execute_cpu_ms{}, last_present_cpu_ms{}, last_signal_cpu_ms{};
  double last_completion_cpu_ms{}, last_draw_cpu_ms{}, total_inflight_wait_cpu_ms{};
  bool last_inflight_wait_pending{};
  std::int64_t debug_interface_result{}; // Process-once SDK configuration result; failed availability is explicit.
  bool debug_layer_configured{}; // EnableDebugLayer was called before first WindowsViewport device.
  bool hardware{}, debug_layer{}, gpu_timing_available{}, minimized{}, device_lost{}, grid_visible{};
  // P03.04 declared output: the most recent submitted frame's profile and target, and
  // the physical output configuration installed in this device epoch. Allocation bytes
  // are API-reported sizes of the owned output set (FP16 target, display image, depth,
  // both swapchain images, worst-case HDR readback) checked against the accepted
  // ceiling; video memory is a separate adapter observation at install time (0 when
  // unavailable) and never part of that ceiling.
  LightingProfile lighting_profile{LightingProfile::LegacyViewportV1};
  OutputTarget output_target{OutputTarget::DisplaySdr};
  bool physical_output_installed{};
  std::uint64_t output_allocation_bytes{}, output_budget_bytes{}, hdr_target_bytes{};
  std::uint64_t hdr_target_installs{}; // FP16 target creations: first install, resizes and background-radiance changes.
  std::uint64_t video_memory_budget_bytes{}, video_memory_usage_bytes{};
  float exposure_scale{}, reference_luminance{};
};
// Metadata of one scene-linear capture (PhysicalDiffuseV1 only). The adapter fills every
// field from its own submission state; callers add source and shader identities when
// they publish a digest-bearing manifest after the image.
struct SceneLinearCapture {
  std::uint32_t width{}, height{};
  std::uint64_t submitted_frame{}, device_epoch{}, scene_generation{}, origin_revision{}, simulation_tick{};
  Digest256 image_sha256{}, raw_sha256{}; // RGB32F PFM bytes; tightly packed RGBA16F rows.
  float exposure_scale{}, reference_luminance{};
  OutputTarget requested_target{OutputTarget::DisplaySdr}, effective_target{OutputTarget::DisplaySdr};
  std::uint32_t dxgi_format{}; // DXGI_FORMAT numeric value of the stored target (10 = R16G16B16A16_FLOAT).
  bool annotations_included{}; // Grid, selection and overlays never enter the scene-linear target.
};
} // namespace race::render
