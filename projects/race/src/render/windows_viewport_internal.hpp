#pragma once
// Private native implementation shared only by the Windows viewport translation units.
#include "windows_viewport.hpp"
#include "race/render/resource_contract.hpp"
#include "race/render/graph.hpp"
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <thread>

namespace race::render {
using Microsoft::WRL::ComPtr;
namespace windows_viewport_detail {

using ProfileClock = std::chrono::steady_clock;
inline double elapsed_ms(ProfileClock::time_point begin, ProfileClock::time_point end) {
  return std::chrono::duration<double, std::milli>(end - begin).count();
}
constexpr UINT frame_count = 2;
constexpr DXGI_FORMAT color_format = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT;
constexpr DXGI_FORMAT texture_format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
constexpr DXGI_FORMAT scalar_texture_format = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT hdr_format =
    DXGI_FORMAT_R16G16B16A16_FLOAT; // PhysicalDiffuseV1 scene-linear target only.
inline DXGI_FORMAT packet_format(const ViewportTexture &packet) noexcept {
  return packet.encoding() == TextureEncoding::LinearData ? scalar_texture_format : texture_format;
}
constexpr UINT texture_descriptors = static_cast<UINT>(viewport_max_textures) + 1;
struct Vertex {
  float x, y, z, nx, ny, nz, u{}, v{};
};
struct Constants {
  std::array<float, 16> mvp{}, normal{};
  std::array<float, 4> color{}, light{}, parameters{};
};
static_assert(sizeof(Constants) == 44 * sizeof(float));
// OutputTransform b0: values[0] = 2^stops / reference luminance; the other lanes stay zero.
struct OutputConstants {
  std::array<float, 8> values{};
};
static_assert(sizeof(OutputConstants) == 32);
using Matrix = std::array<double, 16>;
constexpr Matrix identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

inline float display(float linear) {
  linear = linear / (1 + linear);
  return linear <= .0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.f / 2.4f) - .055f;
}

inline D3D12_HEAP_PROPERTIES heap(D3D12_HEAP_TYPE type) {
  D3D12_HEAP_PROPERTIES h{};
  h.Type = type;
  h.CreationNodeMask = 1;
  h.VisibleNodeMask = 1;
  return h;
}
inline D3D12_RESOURCE_DESC buffer(UINT64 bytes) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = bytes;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  return d;
}
inline D3D12_RESOURCE_BARRIER transition(ID3D12Resource *resource, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
  return b;
}

inline D3D12_SHADER_VISIBILITY native_visibility(std::uint32_t value) noexcept {
  return value == VertexStage  ? D3D12_SHADER_VISIBILITY_VERTEX
         : value == PixelStage ? D3D12_SHADER_VISIBILITY_PIXEL
                               : D3D12_SHADER_VISIBILITY_ALL;
}

bool finite(ViewportVec3 p);
ViewportVec3 subtract(ViewportVec3 a, ViewportVec3 b);
ViewportVec3 cross(ViewportVec3 a, ViewportVec3 b);
Result<ViewportVec3> normalized(ViewportVec3 p);
Matrix multiply(const Matrix &a, const Matrix &b);
Result<Matrix> view_projection(const ViewportCamera &camera, double aspect);
bool valid_color(const std::array<float, 4> &color);
Result<Constants> constants(const std::array<double, 12> &affine, const Matrix &vp, const RenderFrame &frame,
                            const std::array<float, 4> &color, ViewportVec3 light, bool unlit = false,
                            float expansion = 1, bool camera_relative_geometry = false);
Constants overlay_constants(const OverlayDraw &draw) noexcept;
std::vector<Vertex> geometry();
} // namespace windows_viewport_detail
using namespace windows_viewport_detail;
struct WindowsViewport::Program {
  std::vector<char> vertex, pixel;
  AdmittedViewportProgram admitted;
  // P03.04 programs: physical pixel stage (legacy vertex interface) and the
  // fullscreen output transform, admitted together and linked by digest.
  std::vector<char> physical_pixel, output_vertex, output_pixel;
  AdmittedLightOutputPrograms light_output;
  Program(std::vector<char> vs, std::vector<char> ps, AdmittedViewportProgram contract,
          std::vector<char> physical, std::vector<char> output_vs, std::vector<char> output_ps,
          AdmittedLightOutputPrograms light)
      : vertex(std::move(vs)), pixel(std::move(ps)), admitted(std::move(contract)),
        physical_pixel(std::move(physical)), output_vertex(std::move(output_vs)),
        output_pixel(std::move(output_ps)), light_output(std::move(light)) {}
};
struct WindowsViewport::ImageOwner {};
struct WindowsViewport::Impl {
  struct TextureEntry {
    // The packet reference prevents pointer-identity reuse while cached. Upload
    // and GPU allocations remain alive through every submission using this slot.
    std::shared_ptr<const ViewportTexture> packet;
    ComPtr<ID3D12Resource> resource, upload;
    std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, viewport_texture_max_mips> footprints{};
    UINT64 upload_bytes{};
    bool resident{};
  };
  struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> mesh_vertices;
    D3D12_VERTEX_BUFFER_VIEW mesh_view{};
    std::size_t mesh_capacity{};
    UINT64 fence{}, frame{};
    bool timing{};
    std::array<TextureEntry, viewport_max_textures> textures;
  };
  struct MeshRange {
    UINT first{}, count{}, outline_first{}, outline_count{};
  };
  struct OverlayRange {
    UINT first{}, count{};
  };
  HWND window{};
  ViewportStats information;
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGISwapChain3> swapchain;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12Fence> fence;
  HANDLE fence_event{};
  UINT64 next_fence{1}, timestamp_frequency{};
  UINT64 diagnostic_cursor{}, diagnostic_discarded{};
  ComPtr<ID3D12InfoQueue> diagnostics;
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap, srv_heap;
  UINT rtv_increment{}, srv_increment{};
  std::array<ComPtr<ID3D12Resource>, frame_count> targets;
  std::array<Slot, frame_count> slots;
  ComPtr<ID3D12GraphicsCommandList> commands;
  ComPtr<ID3D12Resource> scene_color, depth, vertices, timestamp_readback, capture_readback;
  // P03.04: the FP16 scene-linear target and its readback exist only after a
  // PhysicalDiffuseV1 frame installed the declared output configuration.
  ComPtr<ID3D12Resource> hdr_color, hdr_readback;
  ComPtr<ID3D12QueryHeap> timestamp_heap;
  ComPtr<ID3D12RootSignature> root, output_root;
  ComPtr<ID3D12PipelineState> triangles, lines, overlay_triangles, physical_triangles, output_transform;
  bool physical_output{};
  std::array<float, 4>
      hdr_clear{}; // Optimized clear value of the installed FP16 target: the background radiance it was created for.
  ResolvedLighting resolved{}; // Most recently submitted physical frame; legacy frames leave it unused.
  OutputConstants output_constants{};
  std::optional<AdmittedViewportProgram> program;
  D3D12_VERTEX_BUFFER_VIEW vertex_view{};
  std::vector<Constants> prepared;
  std::vector<Vertex> mesh_staging;
  std::array<MeshRange, viewport_max_draws> mesh_ranges{};
  std::array<OverlayRange, viewport_max_overlay_draws> overlay_ranges{};
  std::array<Constants, viewport_max_overlay_draws> overlay_prepared{};
  std::array<std::shared_ptr<const ViewportTexture>, viewport_max_textures> texture_packets;
  std::array<UINT, viewport_max_textures> texture_bindings{};
  std::array<UINT, viewport_max_draws> draw_textures{}; // 0 = no texture; otherwise unique packet + 1.
  UINT last_target{};
  bool has_image{}, submitted_without_fence{}, retired{};

  ~Impl();
  void invalidate_image_and_timing() noexcept;
  bool observe_removal(bool fence_removal = false);
  Error native_error(HRESULT result, const char *message);
  bool removed();
  void read_diagnostics();
  void record_timing(std::uint64_t frame, double milliseconds);
  void collect(Slot &slot);
  struct WaitMetrics {
    bool pending{};
    double pending_ms{};
  };
  Result<void> wait(UINT64 value, WaitMetrics *metrics = nullptr);
  Result<void> require_signal_capacity(UINT64 count) const;
  Result<UINT64> signal();
  Result<void> drain();
  D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT index) const;
  D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu(UINT slot, UINT texture) const;
  D3D12_GPU_DESCRIPTOR_HANDLE srv_gpu(UINT slot, UINT texture) const;
  // The scene-linear source descriptor follows the two per-slot texture tables.
  D3D12_CPU_DESCRIPTOR_HANDLE hdr_srv_cpu() const;
  D3D12_GPU_DESCRIPTOR_HANDLE hdr_srv_gpu() const;
  void texture_view(ID3D12Resource *resource, UINT slot, UINT texture, UINT mips = 1,
                    DXGI_FORMAT format = texture_format);
  struct TexturePreparation {
    std::array<D3D12_RESOURCE_DESC, viewport_max_textures> descriptions{};
    std::array<std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, viewport_texture_max_mips>,
               viewport_max_textures>
        footprints{};
    std::array<std::array<UINT, viewport_texture_max_mips>, viewport_max_textures> row_counts{};
    std::array<std::array<UINT64, viewport_texture_max_mips>, viewport_max_textures> row_sizes{};
    std::array<UINT64, viewport_max_textures> upload_sizes{};
  } texture_preparation;
  enum class GraphBinding : std::uint8_t {
    Texture,
    TextureUpload,
    Vertices,
    MeshVertices,
    Color,
    Depth,
    Backbuffer,
    TimingReadback,
    CaptureReadback,
    HdrColor,
    HdrReadback
  };
  struct GraphReference {
    GraphBinding kind{};
    std::uint16_t index{};
  };
  struct GraphWork {
    std::array<graph::Resource, graph::max_resources> resources{};
    std::array<GraphReference, graph::max_resources> references{};
    std::array<graph::Pass, graph::max_passes> passes{};
    std::array<graph::Use, graph::max_uses> uses{};
    std::array<ID3D12Resource *, graph::max_resources> native_resources{};
    std::array<D3D12_RESOURCE_BARRIER, graph::max_transitions> native_transitions{};
    std::array<bool, viewport_max_textures> resident{};
    graph::Scratch scratch;
    std::optional<graph::Plan> plan;
    std::size_t resource_count{}, pass_count{}, use_count{};
    bool overflow{}, capture{};
    double compile_cpu_ms{};
    void reset(bool capturing) noexcept {
      plan.reset(); // Builder arrays and installed plan must always describe the same candidate.
      resource_count = pass_count = use_count = 0;
      overflow = false;
      capture = capturing;
      native_resources.fill(nullptr);
    }
    graph::ResourceId resource(graph::Resource value, GraphReference reference) noexcept {
      if (resource_count == resources.size()) {
        overflow = true;
        return 0;
      }
      const auto id = static_cast<graph::ResourceId>(resource_count++);
      resources[id] = value;
      references[id] = reference;
      return id;
    }
    graph::PassId pass(graph::Operation operation, graph::PassMask prerequisites = 0) noexcept {
      if (pass_count == passes.size()) {
        overflow = true;
        return graph::no_pass;
      }
      const auto id = static_cast<graph::PassId>(pass_count++);
      passes[id] = {operation, graph::Queue::Direct, prerequisites, static_cast<std::uint16_t>(use_count), 0};
      return id;
    }
    void use(graph::ResourceId resource, graph::Access access, graph::State state,
             bool initializes_all = false) noexcept {
      if (!pass_count || use_count == uses.size()) {
        overflow = true;
        return;
      }
      uses[use_count++] = {resource, access, state, initializes_all};
      ++passes[pass_count - 1].use_count;
    }
  } graph_work;
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  enum class GraphFault { None, Description, Binding };
  GraphFault graph_fault{GraphFault::None};
  std::uint64_t output_budget_override{}; // Probe builds only: a lower test ceiling for install_output.
#endif
  static graph::Resource graph_buffer(std::uint64_t bytes, graph::Memory memory, std::uint32_t uses,
                                      bool valid) noexcept;
  static graph::Resource graph_texture(UINT width, UINT height, UINT mips, ResourceFormat format,
                                       std::uint32_t uses, graph::State initial, graph::State terminal,
                                       bool valid,
                                       graph::Lifetime lifetime = graph::Lifetime::Persistent) noexcept;
  // Tagged HDR classes: the 128 MiB ceiling applies only to these two roles.
  static graph::Resource graph_hdr(UINT width, UINT height, graph::Lifetime lifetime, bool valid) noexcept;
  static graph::Resource graph_hdr_readback(std::uint64_t bytes) noexcept;
  Result<void> finish_graph();
  void publish_graph_submission() noexcept;
  struct DrawGraphBindings {
    const std::array<graph::ResourceId, viewport_max_textures> &textures;
    const std::array<graph::ResourceId, viewport_max_textures> &uploads;
    std::size_t texture_count, pending;
    bool physical, opaque_cube, opaque_mesh, selected_cube, selected_mesh, draw_grid, overlays;
    graph::ResourceId geometry, mesh, color, hdr, depth_id, back, timing;
  };
  Result<void> build_draw_passes(const DrawGraphBindings &bindings);
  Result<void> resolve_graph_resource(const GraphReference &ref, UINT slot_index, std::size_t texture_count,
                                      ID3D12Resource *&native);
  void register_graph_textures(UINT slot_index, std::size_t texture_count,
                               std::array<graph::ResourceId, viewport_max_textures> &textures,
                               std::array<graph::ResourceId, viewport_max_textures> &uploads,
                               std::size_t &pending);
  Result<void> admit_draw_graph(UINT slot_index, const RenderFrame &frame, bool draw_grid,
                                std::size_t texture_count, bool physical);
  Result<void> admit_capture_graph(UINT64 bytes, bool scene_linear = false);
  static bool graph_state(graph::State state, D3D12_RESOURCE_STATES &native) noexcept;
  static DXGI_FORMAT graph_format(ResourceFormat format) noexcept;
  Result<void> bind_graph(UINT slot_index, std::size_t texture_count);
  void record_graph_transitions(const graph::Step &step, bool after) noexcept;
  Result<void> preflight_textures(std::size_t count);
  Result<void> prepare_textures(UINT slot_index, std::size_t count);
  Result<void> make_targets(UINT width, UINT height);
  // Declared PhysicalDiffuseV1 output set: FP16 target, display image, depth, both swapchain
  // images and the worst-case HDR readback, preflighted with API allocation sizes against the
  // accepted ceiling before anything is created. Over budget rejects with Exhausted and
  // installs nothing; swapchain sizes are description-based estimates, not measurements.
  Result<void> query_output_allocations(const std::array<const D3D12_RESOURCE_DESC *, 6> &descriptions,
                                        std::array<std::uint64_t, 6> &api);
  void install_output_views();
  Result<void> install_output(UINT width, UINT height, const std::array<float, 4> &clear);
  // Borrowed frame preparation; every referenced value stays in its caller through completion.
  struct DrawFrame {
    const RenderFrame &frame;
    bool physical;
    const ResolvedLighting &resolved;
    const Result<Constants> &grid;
    bool draw_grid, draw_x_axis, draw_z_axis;
    const Constants &x_axis;
    const Constants &z_axis;
    std::size_t texture_count, texture_bytes, overlay_vertices;
  };
  struct DrawProfile {
    ProfileClock::time_point draw_started, validation_finished, wait_finished, record_finished;
    const WaitMetrics &wait_metrics;
  };
  Result<void> validate_overlays(const RenderFrame &frame, std::size_t &overlay_vertices);
  Result<void> collect_textures(const RenderFrame &frame, std::size_t &texture_count,
                                std::size_t &texture_bytes);
  Result<void> reserve_mesh(const RenderFrame &frame, std::size_t overlay_vertices);
  Result<void> prepare_mesh(const RenderFrame &frame, const DrawPrimitive &draw, std::size_t i);
  Result<void> prepare_draws(const RenderFrame &frame, bool physical, const ResolvedLighting &lighting,
                             ViewportVec3 light_direction, const Matrix &vp);
  void prepare_overlays(const RenderFrame &frame);
  Result<void> draw_prepared(const RenderFrame &frame, bool physical, const ResolvedLighting &lighting,
                             ViewportVec3 light_direction, const Matrix &vp, ViewportVec3 light,
                             std::size_t overlay_vertices, ProfileClock::time_point draw_started);
  Result<void> upload_mesh(Slot &slot);
  void record_upload(Slot &slot);
  void record_opaque(const DrawFrame &draw, UINT index, Slot &slot);
  void record_grid(const DrawFrame &draw, UINT index, Slot &slot);
  void record_overlays(const DrawFrame &draw, UINT index, Slot &slot);
  void record_output();
  Result<void> record_draw(const DrawFrame &draw, UINT index, Slot &slot);
  Result<void> draw_ready(const DrawFrame &draw, ProfileClock::time_point draw_started);
  void publish_draw(const DrawFrame &draw);
  void publish_draw_profile(Slot &slot, const DrawProfile &timing, ProfileClock::time_point execute_started,
                            ProfileClock::time_point execute_finished,
                            ProfileClock::time_point present_started,
                            ProfileClock::time_point present_finished,
                            ProfileClock::time_point signal_started, ProfileClock::time_point signal_finished,
                            ProfileClock::time_point completion_started,
                            ProfileClock::time_point draw_finished);
  Result<void> submit_draw(const DrawFrame &draw, UINT index, Slot &slot, const DrawProfile &timing);
  Result<void> copy_display_image(Slot &slot, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                                  UINT64 bytes);
  Result<void> copy_linear_image(Slot &slot, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                                 UINT64 bytes);
  Result<void> initialize_device(bool debug_configured);
  Result<void> initialize_queue();
  Result<void> initialize_surface(HWND hwnd, UINT width, UINT height, ComPtr<IDXGISwapChain1> &created);
  Result<void> initialize_commands();
  Result<void> initialize_root(std::span<const ResourceBinding> bindings, ComPtr<ID3DBlob> &serialized,
                               ComPtr<ID3DBlob> &errors);
  Result<void> initialize_pipelines(const Program &retained, D3D12_GRAPHICS_PIPELINE_STATE_DESC &pipeline,
                                    const D3D12_INPUT_ELEMENT_DESC *layout);
  Result<void> initialize_output_root(const Program &retained, ComPtr<ID3DBlob> &output_serialized,
                                      ComPtr<ID3DBlob> &output_errors);
  Result<void> initialize_output_pipeline(const Program &retained,
                                          const D3D12_GRAPHICS_PIPELINE_STATE_DESC &pipeline);
  Result<void> initialize_geometry(std::span<const Vertex> data);
  void initialize_timestamps();
  Result<void> initialize(HWND hwnd, const Program &retained, std::uint64_t epoch);
};
} // namespace race::render
