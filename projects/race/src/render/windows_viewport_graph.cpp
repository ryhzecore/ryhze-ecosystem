#include "windows_viewport_internal.hpp"

namespace race::render {
graph::Resource WindowsViewport::Impl::graph_buffer(std::uint64_t bytes, graph::Memory memory,
                                                    std::uint32_t uses, bool valid) noexcept {
  const auto state =
      memory == graph::Memory::Upload ? graph::State::UploadRead : graph::State::CopyDestination;
  return {{ResourceKind::Buffer, ResourceFormat::Bytes, uses, bytes, 1, 1, 1, 1},
          graph::Lifetime::Persistent,
          memory,
          state,
          state,
          valid,
          false};
}

graph::Resource WindowsViewport::Impl::graph_texture(UINT width, UINT height, UINT mips,
                                                     ResourceFormat format, std::uint32_t uses,
                                                     graph::State initial, graph::State terminal, bool valid,
                                                     graph::Lifetime lifetime) noexcept {
  return {{ResourceKind::Texture2D, format, uses, 0, width, height, mips, 1},
          lifetime,
          graph::Memory::Device,
          initial,
          terminal,
          valid,
          false};
}

graph::Resource WindowsViewport::Impl::graph_hdr(UINT width, UINT height, graph::Lifetime lifetime,
                                                 bool valid) noexcept {
  graph::Resource r{{ResourceKind::Texture2D, ResourceFormat::Rgba16Float,
                     ColorTarget | ShaderSample | CopySource, 0, width, height, 1, 1},
                    lifetime,
                    graph::Memory::Device,
                    graph::State::CopySource,
                    graph::State::CopySource,
                    valid,
                    false};
  r.output_class = OutputResourceClass::HdrAttachment;
  return r;
}

graph::Resource WindowsViewport::Impl::graph_hdr_readback(std::uint64_t bytes) noexcept {
  graph::Resource r{{ResourceKind::Buffer, ResourceFormat::Bytes, CopyDestination, bytes, 1, 1, 1, 1},
                    graph::Lifetime::Persistent,
                    graph::Memory::Readback,
                    graph::State::CopyDestination,
                    graph::State::CopyDestination,
                    false,
                    false};
  r.output_class = OutputResourceClass::HdrReadback;
  return r;
}

Result<void> WindowsViewport::Impl::finish_graph() {
  auto &g = graph_work;
  if (g.overflow)
    return fail(ErrorCode::Exhausted, "viewport graph builder capacity exceeded");
  auto &stats = g.capture ? information.capture_graph : information.draw_graph;
  if (stats.compiles == UINT64_MAX || stats.submissions == UINT64_MAX)
    return fail(ErrorCode::Overflow, "viewport graph diagnostic identity exhausted");
  const auto started = ProfileClock::now();
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  if (!g.capture && graph_fault == GraphFault::Description) {
    graph_fault = GraphFault::None;
    if (g.use_count)
      g.uses[0].resource = static_cast<graph::ResourceId>(graph::max_resources);
  }
#endif
  auto admitted = graph::compile(
      {{g.resources.data(), g.resource_count}, {g.passes.data(), g.pass_count}, {g.uses.data(), g.use_count}},
      g.scratch);
  if (!admitted)
    return admitted.error();
  g.plan = std::move(admitted).value();
  g.compile_cpu_ms = elapsed_ms(started, ProfileClock::now());
  ++stats.compiles;
  return {};
}

void WindowsViewport::Impl::publish_graph_submission() noexcept {
  const auto &g = graph_work;
  const auto &plan = *g.plan;
  auto &stats = g.capture ? information.capture_graph : information.draw_graph;
  ++stats.submissions;
  stats.resources = static_cast<std::uint32_t>(plan.resources().size());
  stats.passes = static_cast<std::uint32_t>(plan.passes().size());
  stats.uses = static_cast<std::uint32_t>(plan.uses().size());
  stats.transitions = static_cast<std::uint32_t>(plan.transitions().size());
  stats.operation_mask = 0;
  for (const auto &pass : plan.passes())
    stats.operation_mask |= 1u << static_cast<unsigned>(pass.operation);
  stats.last_compile_cpu_ms = g.compile_cpu_ms;
  information.graph_retained_bytes = sizeof(GraphWork) + sizeof(TexturePreparation);
  information.graph_plan_bytes = sizeof(graph::Plan);
}

void WindowsViewport::Impl::register_graph_textures(
    UINT slot_index, std::size_t texture_count,
    std::array<graph::ResourceId, viewport_max_textures> &textures,
    std::array<graph::ResourceId, viewport_max_textures> &uploads, std::size_t &pending) {
  using S = graph::State;
  auto &g = graph_work;
  for (std::size_t i = 0; i < texture_count; ++i) {
    const auto &packet = *texture_packets[i];
    bool resident = false;
    for (const auto &entry : slots[slot_index].textures)
      if (entry.packet == texture_packets[i]) {
        resident = entry.resident;
        break;
      }
    g.resident[i] = resident;
    textures[i] =
        g.resource(graph_texture(packet.width(), packet.height(), packet.mip_count(),
                                 packet.encoding() == TextureEncoding::LinearData ? ResourceFormat::Rgba8Unorm
                                                                                  : ResourceFormat::Rgba8Srgb,
                                 CopyDestination | ShaderSample,
                                 resident ? S::SampledRead : S::CopyDestination, S::SampledRead, resident),
                   {GraphBinding::Texture, static_cast<std::uint16_t>(i)});
    if (!resident) {
      uploads[i] = g.resource(
          graph_buffer(texture_preparation.upload_sizes[i], graph::Memory::Upload, CopySource, true),
          {GraphBinding::TextureUpload, static_cast<std::uint16_t>(i)});
      ++pending;
    }
  }
}

Result<void> WindowsViewport::Impl::admit_draw_graph(UINT slot_index, const RenderFrame &frame,
                                                     bool draw_grid, std::size_t texture_count,
                                                     bool physical) {
  using S = graph::State;
  if (slot_index >= slots.size() || texture_count > viewport_max_textures)
    return fail(ErrorCode::InvalidInput, "viewport graph slot or texture count is invalid");
  auto &g = graph_work;
  g.reset(false);
  std::array<graph::ResourceId, viewport_max_textures> textures{}, uploads{};
  std::size_t pending{};
  register_graph_textures(slot_index, texture_count, textures, uploads, pending);
  bool opaque_cube = false, opaque_mesh = false, selected_cube = false, selected_mesh = false;
  for (const auto &draw : frame.draws) {
    if (draw.geometry) {
      opaque_mesh = true;
      selected_mesh = selected_mesh || draw.selected;
    } else {
      opaque_cube = true;
      selected_cube = selected_cube || draw.selected;
    }
  }
  graph::ResourceId geometry{}, mesh{};
  if (opaque_cube || draw_grid)
    geometry = g.resource(graph_buffer(vertex_view.SizeInBytes, graph::Memory::Upload, VertexInput, true),
                          {GraphBinding::Vertices, 0});
  if (opaque_mesh || !frame.overlays.empty())
    mesh = g.resource(
        graph_buffer(mesh_staging.size() * sizeof(Vertex), graph::Memory::Upload, VertexInput, true),
        {GraphBinding::MeshVertices, 0});
  auto retained_color = graph_texture(information.width, information.height, 1, ResourceFormat::Rgba8Unorm,
                                      ColorTarget | CopySource, S::CopySource, S::CopySource, false);
  retained_color.exported = true;
  const auto color = g.resource(retained_color, {GraphBinding::Color, 0});
  graph::ResourceId hdr{};
  if (physical)
    hdr = g.resource(graph_hdr(information.width, information.height, graph::Lifetime::Persistent, false),
                     {GraphBinding::HdrColor, 0});
  const auto depth_id =
      g.resource(graph_texture(information.width, information.height, 1, ResourceFormat::Depth32Float,
                               DepthTarget, S::DepthAttachment, S::DepthAttachment, false),
                 {GraphBinding::Depth, 0});
  const auto back =
      g.resource(graph_texture(information.width, information.height, 1, ResourceFormat::Rgba8Unorm,
                               CopyDestination, S::Present, S::Present, false, graph::Lifetime::Imported),
                 {GraphBinding::Backbuffer, 0});
  graph::ResourceId timing{};
  if (information.gpu_timing_available)
    timing = g.resource(graph_buffer(frame_count * 16, graph::Memory::Readback, CopyDestination, false),
                        {GraphBinding::TimingReadback, 0});
  const DrawGraphBindings bindings{textures,
                                   uploads,
                                   texture_count,
                                   pending,
                                   physical,
                                   opaque_cube,
                                   opaque_mesh,
                                   selected_cube,
                                   selected_mesh,
                                   draw_grid,
                                   !frame.overlays.empty(),
                                   geometry,
                                   mesh,
                                   color,
                                   hdr,
                                   depth_id,
                                   back,
                                   timing};
  return build_draw_passes(bindings);
}

Result<void> WindowsViewport::Impl::build_draw_passes(const DrawGraphBindings &bindings) {
  using A = graph::Access;
  using S = graph::State;
  using O = graph::Operation;
  auto &g = graph_work;
  const auto texture_count = bindings.texture_count, pending = bindings.pending;
  const auto physical = bindings.physical, opaque_cube = bindings.opaque_cube,
             opaque_mesh = bindings.opaque_mesh, selected_cube = bindings.selected_cube,
             selected_mesh = bindings.selected_mesh, draw_grid = bindings.draw_grid;
  const auto geometry = bindings.geometry, mesh = bindings.mesh, color = bindings.color, hdr = bindings.hdr,
             depth_id = bindings.depth_id, back = bindings.back, timing = bindings.timing;
  const auto &textures = bindings.textures;
  const auto &uploads = bindings.uploads;
  if (pending) {
    g.pass(O::Upload);
    for (std::size_t i = 0; i < texture_count; ++i)
      if (!g.resident[i]) {
        g.use(uploads[i], A::Read, S::UploadRead);
        g.use(textures[i], A::Write, S::CopyDestination, true);
      }
  }
  g.pass(O::Opaque);
  for (std::size_t i = 0; i < texture_count; ++i)
    g.use(textures[i], A::Read, S::SampledRead);
  if (opaque_cube)
    g.use(geometry, A::Read, S::UploadRead);
  if (opaque_mesh)
    g.use(mesh, A::Read, S::UploadRead);
  g.use(physical ? hdr : color, A::Write, S::ColorAttachment, true);
  g.use(depth_id, A::Write, S::DepthAttachment, true);
  // Physical frames shade into the FP16 target; one OutputTransform then writes the whole
  // display image before annotations. Legacy frames keep their direct display write.
  if (physical) {
    g.pass(O::OutputTransform);
    g.use(hdr, A::Read, S::SampledRead);
    g.use(color, A::Write, S::ColorAttachment, true);
  }
  if (draw_grid || selected_cube || selected_mesh) {
    g.pass(O::GridSelection);
    if (draw_grid || selected_cube)
      g.use(geometry, A::Read, S::UploadRead);
    if (selected_mesh)
      g.use(mesh, A::Read, S::UploadRead);
    g.use(depth_id, A::Read, S::DepthAttachment);
    g.use(color, A::ReadWrite, S::ColorAttachment);
  }
  if (bindings.overlays) {
    g.pass(O::Overlay);
    g.use(mesh, A::Read, S::UploadRead);
    g.use(color, A::ReadWrite, S::ColorAttachment);
  }
  const auto present = g.pass(O::PresentCopy);
  g.use(color, A::Read, S::CopySource);
  g.use(back, A::Write, S::CopyDestination, true);
  // Query resolve otherwise has no image hazard. Preserve its measured scope,
  // including the terminal PRESENT transition after the presentation copy.
  if (information.gpu_timing_available) {
    if (present == graph::no_pass)
      return fail(ErrorCode::Exhausted, "viewport graph presentation pass unavailable");
    g.pass(O::TimingResolve, static_cast<graph::PassMask>(1u << present));
    g.use(timing, A::Write, S::CopyDestination);
  }
  return finish_graph();
}

Result<void> WindowsViewport::Impl::admit_capture_graph(UINT64 bytes, bool scene_linear) {
  using A = graph::Access;
  using S = graph::State;
  auto &g = graph_work;
  g.reset(true);
  const auto source =
      scene_linear
          ? g.resource(graph_hdr(information.width, information.height, graph::Lifetime::Imported, true),
                       {GraphBinding::HdrColor, 0})
          : g.resource(graph_texture(information.width, information.height, 1, ResourceFormat::Rgba8Unorm,
                                     CopySource, S::CopySource, S::CopySource, true,
                                     graph::Lifetime::Imported),
                       {GraphBinding::Color, 0});
  const auto readback = scene_linear
                            ? g.resource(graph_hdr_readback(bytes), {GraphBinding::HdrReadback, 0})
                            : g.resource(graph_buffer(bytes, graph::Memory::Readback, CopyDestination, false),
                                         {GraphBinding::CaptureReadback, 0});
  g.pass(graph::Operation::CaptureCopy);
  g.use(source, A::Read, S::CopySource);
  g.use(readback, A::Write, S::CopyDestination);
  return finish_graph();
}

bool WindowsViewport::Impl::graph_state(graph::State state, D3D12_RESOURCE_STATES &native) noexcept {
  switch (state) {
  case graph::State::UploadRead:
    native = D3D12_RESOURCE_STATE_GENERIC_READ;
    return true;
  case graph::State::CopySource:
    native = D3D12_RESOURCE_STATE_COPY_SOURCE;
    return true;
  case graph::State::CopyDestination:
    native = D3D12_RESOURCE_STATE_COPY_DEST;
    return true;
  case graph::State::VertexRead:
    native = D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    return true;
  case graph::State::SampledRead:
    native = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
  case graph::State::ColorAttachment:
    native = D3D12_RESOURCE_STATE_RENDER_TARGET;
    return true;
  case graph::State::DepthAttachment:
    native = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    return true;
  case graph::State::Present:
    native = D3D12_RESOURCE_STATE_PRESENT;
    return true;
  default:
    return false; // No speculative Undefined/alias allocation in this backend.
  }
}

DXGI_FORMAT WindowsViewport::Impl::graph_format(ResourceFormat format) noexcept {
  switch (format) {
  case ResourceFormat::Rgba8Unorm:
    return DXGI_FORMAT_R8G8B8A8_UNORM;
  case ResourceFormat::Rgba8Srgb:
    return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
  case ResourceFormat::Rgba16Float:
    return DXGI_FORMAT_R16G16B16A16_FLOAT;
  case ResourceFormat::Depth32Float:
    return DXGI_FORMAT_D32_FLOAT;
  default:
    return DXGI_FORMAT_UNKNOWN;
  }
}

Result<void> WindowsViewport::Impl::resolve_graph_resource(const GraphReference &ref, UINT slot_index,
                                                           std::size_t texture_count,
                                                           ID3D12Resource *&native) {
  auto &g = graph_work;
  switch (ref.kind) {
  case GraphBinding::Texture:
  case GraphBinding::TextureUpload: {
    const auto n = ref.index;
    if (n >= texture_count || !texture_bindings[n] || texture_bindings[n] > viewport_max_textures)
      return fail(ErrorCode::Invariant, "viewport graph texture binding is unavailable");
    const auto &entry = slots[slot_index].textures[texture_bindings[n] - 1];
    if (entry.packet != texture_packets[n] || entry.resident != g.resident[n])
      return fail(ErrorCode::Invariant,
                  "viewport graph prospective residency differs from materialized cache");
    native = ref.kind == GraphBinding::Texture ? entry.resource.Get() : entry.upload.Get();
    if (ref.kind == GraphBinding::TextureUpload) {
      if (entry.upload_bytes != texture_preparation.upload_sizes[n])
        return fail(ErrorCode::Invariant, "viewport graph upload footprint size differs");
      for (UINT mip = 0; mip < entry.packet->mip_count(); ++mip) {
        const auto &a = entry.footprints[mip];
        const auto &b = texture_preparation.footprints[n][mip];
        if (a.Offset != b.Offset || a.Footprint.Format != b.Footprint.Format ||
            a.Footprint.Width != b.Footprint.Width || a.Footprint.Height != b.Footprint.Height ||
            a.Footprint.Depth != b.Footprint.Depth || a.Footprint.RowPitch != b.Footprint.RowPitch)
          return fail(ErrorCode::Invariant, "viewport graph upload footprint layout differs");
      }
    }
    break;
  }
  case GraphBinding::Vertices:
    native = vertices.Get();
    break;
  case GraphBinding::MeshVertices:
    native = slots[slot_index].mesh_vertices.Get();
    break;
  case GraphBinding::Color:
    native = scene_color.Get();
    break;
  case GraphBinding::Depth:
    native = depth.Get();
    break;
  case GraphBinding::Backbuffer:
    native = targets[slot_index].Get();
    break;
  case GraphBinding::TimingReadback:
    native = timestamp_readback.Get();
    break;
  case GraphBinding::CaptureReadback:
    native = capture_readback.Get();
    break;
  case GraphBinding::HdrColor:
    native = hdr_color.Get();
    break;
  case GraphBinding::HdrReadback:
    native = hdr_readback.Get();
    break;
  }
  return {};
}

Result<void> WindowsViewport::Impl::bind_graph(UINT slot_index, std::size_t texture_count) {
  auto &g = graph_work;
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  if (!g.capture && graph_fault == GraphFault::Binding) {
    graph_fault = GraphFault::None;
    if (g.resource_count)
      g.references[0] = {GraphBinding::Texture, static_cast<std::uint16_t>(viewport_max_textures)};
  }
#endif
  if (!g.plan || slot_index >= slots.size() || g.plan->resources().size() != g.resource_count)
    return fail(ErrorCode::Invariant, "viewport graph candidate is unavailable");
  // Everything that can reject the native mapping runs before allocator Reset.
  for (const auto &pass : g.plan->passes()) {
    const bool capture = pass.operation == graph::Operation::CaptureCopy;
    if (capture != g.capture)
      return fail(ErrorCode::Unsupported, "viewport graph operation does not belong to this submission");
  }
  for (std::size_t i = 0; i < g.resource_count; ++i) {
    const auto &ref = g.references[i];
    const auto &resource = g.plan->resources()[i];
    const auto &d = resource.description;
    ID3D12Resource *native{};
    auto resolution = resolve_graph_resource(ref, slot_index, texture_count, native);
    if (!resolution)
      return resolution;
    if (!native || resource.lifetime == graph::Lifetime::Transient)
      return fail(ErrorCode::Invariant, "viewport graph requires an owned committed or imported resource");
    for (std::size_t j = 0; j < i; ++j)
      if (g.native_resources[j] == native)
        return fail(ErrorCode::Invariant, "viewport graph has duplicate physical resource identities");
    const auto actual = native->GetDesc();
    if (actual.DepthOrArraySize != 1 || actual.SampleDesc.Count != 1 || actual.SampleDesc.Quality != 0)
      return fail(ErrorCode::Invariant, "viewport graph resource sample or array description differs");
    if (d.kind == ResourceKind::Buffer) {
      if (actual.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER || actual.Format != DXGI_FORMAT_UNKNOWN ||
          actual.Width < d.buffer_bytes || actual.Height != 1 || actual.MipLevels != 1)
        return fail(ErrorCode::Invariant, "viewport graph byte-buffer description differs");
    } else if (actual.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
               actual.Format != graph_format(d.format) || actual.Width != d.width ||
               actual.Height != d.height || actual.MipLevels != d.mip_levels ||
               ((d.uses & ColorTarget) && !(actual.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) ||
               ((d.uses & DepthTarget) && !(actual.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ||
               ((d.uses & ShaderSample) && (actual.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)))
      return fail(ErrorCode::Invariant, "viewport graph texture description differs");
    D3D12_RESOURCE_STATES initial{}, terminal{};
    if (!graph_state(resource.initial, initial) || !graph_state(resource.terminal, terminal))
      return fail(ErrorCode::Unsupported, "viewport graph native state is unsupported");
    g.native_resources[i] = native;
  }
  std::size_t n{};
  for (const auto &change : g.plan->transitions()) {
    D3D12_RESOURCE_STATES before{}, after{};
    if (change.resource >= g.resource_count || !graph_state(change.before, before) ||
        !graph_state(change.after, after))
      return fail(ErrorCode::Unsupported, "viewport graph transition cannot be mapped before recording");
    g.native_transitions[n++] = transition(g.native_resources[change.resource], before, after);
  }
  return {};
}

void WindowsViewport::Impl::record_graph_transitions(const graph::Step &step, bool after) noexcept {
  const auto first = after ? step.after_first : step.before_first;
  const auto count = after ? step.after_count : step.before_count;
  if (count)
    commands->ResourceBarrier(count, graph_work.native_transitions.data() + first);
}
} // namespace race::render
