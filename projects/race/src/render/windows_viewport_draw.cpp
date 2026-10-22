#include "windows_viewport_internal.hpp"

namespace race::render {
Result<void> WindowsViewport::draw(const RenderFrame &frame) {
  const auto draw_started = ProfileClock::now();
  auto own = owner_check();
  if (!own)
    return own;
  if (!impl_)
    return fail(ErrorCode::FailedPrerequisite, "viewport has no native surface");
  auto &p = *impl_;
  if (p.removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 viewport device removed");
  if (p.submitted_without_fence)
    return fail(ErrorCode::Busy, "viewport submission requires quiescence before reuse");
  if (p.information.minimized)
    return {};
  auto capacity = p.require_signal_capacity(1);
  if (!capacity)
    return capacity;
  if (p.information.submitted_frames == UINT64_MAX)
    return fail(ErrorCode::Overflow, "viewport frame identity exhausted");
  if (!p.scene_color || !p.depth || !p.targets[0] || !p.targets[1])
    return fail(ErrorCode::Busy, "viewport targets unavailable after resize failure");
  if (frame.draws.size() > viewport_max_draws)
    return fail(ErrorCode::Exhausted, "viewport draw capacity is 1024");
  if (frame.overlays.size() > viewport_max_overlay_draws)
    return fail(ErrorCode::Exhausted, "viewport overlay draw capacity is 8");
  std::size_t overlay_vertices{};
  auto overlays = p.validate_overlays(frame, overlay_vertices);
  if (!overlays)
    return overlays;
  if (!frame.origin_revision || !valid_color(frame.clear_color) ||
      !std::isfinite(frame.directional_intensity) || frame.directional_intensity < 0 ||
      frame.directional_intensity > 32 || !std::isfinite(frame.ambient_intensity) ||
      frame.ambient_intensity < 0 || frame.ambient_intensity > 8)
    return fail(ErrorCode::InvalidInput, "invalid viewport frame lighting or origin");
  // Exactly one lighting profile per frame. Legacy frames must carry inactive physical
  // fields; physical frames resolve their photometric quantities before any GPU work.
  auto lighting = validate_frame_lighting(frame.lighting_profile, frame.physical);
  if (!lighting)
    return lighting;
  const bool physical = frame.lighting_profile == LightingProfile::PhysicalDiffuseV1;
  ResolvedLighting resolved{};
  if (physical) {
    auto admitted = resolve_physical_lighting(frame.physical);
    if (!admitted)
      return admitted.error();
    resolved = admitted.value();
  }
  auto light = normalized(frame.direction_to_light);
  if (!light)
    return light.error();
  const ViewportVec3 light_direction =
      physical ? ViewportVec3{resolved.direction_to_light[0], resolved.direction_to_light[1],
                              resolved.direction_to_light[2]}
               : light.value();
  auto vp = view_projection(frame.camera, static_cast<double>(p.information.width) / p.information.height);
  if (!vp)
    return vp.error();
  return p.draw_prepared(frame, physical, resolved, light_direction, vp.value(), light.value(),
                         overlay_vertices, draw_started);
}
Result<void> WindowsViewport::Impl::upload_mesh(Slot &slot) {
  auto &p = *this;
  if (!p.mesh_staging.empty()) {
    const auto bytes = p.mesh_staging.size() * sizeof(Vertex);
    if (slot.mesh_capacity < bytes) {
      // This slot has completed. Release before growth to bound total GPU storage
      // to two slot buffers, each at most
      // (3 * frame_indices + viewport_max_overlay_vertices) * sizeof(Vertex).
      slot.mesh_vertices.Reset();
      slot.mesh_capacity = 0;
      slot.mesh_view = {};
      const auto upload = heap(D3D12_HEAP_TYPE_UPLOAD);
      const auto desc = buffer(bytes);
      const auto allocated = p.device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc,
                                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                               IID_PPV_ARGS(&slot.mesh_vertices));
      if (FAILED(allocated))
        return p.native_error(allocated, "viewport mesh upload allocation failed");
      slot.mesh_capacity = bytes;
    }
    void *mapped{};
    D3D12_RANGE no_read{0, 0};
    const auto mapped_result = slot.mesh_vertices->Map(0, &no_read, &mapped);
    if (FAILED(mapped_result))
      return p.native_error(mapped_result, "viewport mesh upload map failed");
    std::memcpy(mapped, p.mesh_staging.data(), bytes);
    D3D12_RANGE written{0, bytes};
    slot.mesh_vertices->Unmap(0, &written);
    slot.mesh_view = {slot.mesh_vertices->GetGPUVirtualAddress(), static_cast<UINT>(bytes), sizeof(Vertex)};
  }
  return {};
}

void WindowsViewport::Impl::record_upload(Slot &slot) {
  auto &p = *this;
  for (auto &entry : slot.textures)
    if (entry.packet && !entry.resident) {
      D3D12_TEXTURE_COPY_LOCATION from{};
      from.pResource = entry.upload.Get();
      from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      D3D12_TEXTURE_COPY_LOCATION to{};
      to.pResource = entry.resource.Get();
      to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
      for (UINT level = 0; level < entry.packet->mip_count(); ++level) {
        from.PlacedFootprint = entry.footprints[level];
        to.SubresourceIndex = level;
        p.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
      }
    }
}

void WindowsViewport::Impl::record_opaque(const DrawFrame &draw, UINT index, Slot &slot) {
  auto &p = *this;
  const auto &frame = draw.frame;
  const bool physical = draw.physical;
  const auto target = p.rtv(physical ? frame_count + 1 : frame_count),
             depth_view = p.dsv_heap->GetCPUDescriptorHandleForHeapStart();
  p.commands->OMSetRenderTargets(1, &target, FALSE, &depth_view);
  // Physical frames clear the scene-linear target with the unexposed background
  // radiance (cd/m^2); legacy frames keep their display-encoded clear.
  // Physical clears use the installed target's optimized clear value, which draw() keeps
  // equal to this frame's background radiance.
  const float clear[]{physical ? p.hdr_clear[0] : display(frame.clear_color[0]),
                      physical ? p.hdr_clear[1] : display(frame.clear_color[1]),
                      physical ? p.hdr_clear[2] : display(frame.clear_color[2]), 1};
  p.commands->ClearRenderTargetView(target, clear, 0, nullptr);
  p.commands->ClearDepthStencilView(depth_view, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
  const D3D12_VIEWPORT viewport{
      0, 0, static_cast<float>(p.information.width), static_cast<float>(p.information.height), 0, 1};
  const D3D12_RECT scissor{0, 0, static_cast<LONG>(p.information.width),
                           static_cast<LONG>(p.information.height)};
  p.commands->RSSetViewports(1, &viewport);
  p.commands->RSSetScissorRects(1, &scissor);
  p.commands->SetGraphicsRootSignature(p.root.Get());
  p.commands->IASetVertexBuffers(0, 1, &p.vertex_view);
  ID3D12DescriptorHeap *heaps[]{p.srv_heap.Get()};
  p.commands->SetDescriptorHeaps(1, heaps);
  p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  if (physical)
    p.commands->SetPipelineState(p.physical_triangles.Get());
  for (std::size_t i = 0; i < p.prepared.size(); ++i) {
    const auto &range = p.mesh_ranges[i];
    p.commands->IASetVertexBuffers(0, 1, range.count ? &slot.mesh_view : &p.vertex_view);
    p.commands->SetGraphicsRoot32BitConstants(0, 44, &p.prepared[i], 0);
    p.commands->SetGraphicsRootDescriptorTable(
        1, p.srv_gpu(index, p.draw_textures[i] ? p.texture_bindings[p.draw_textures[i] - 1] : 0));
    p.commands->DrawInstanced(range.count ? range.count : 36, 1, range.count ? range.first : 0, 0);
  }
}

void WindowsViewport::Impl::record_grid(const DrawFrame &draw, UINT index, Slot &slot) {
  auto &p = *this;
  const auto &frame = draw.frame;
  const bool physical = draw.physical;
  const auto &grid = draw.grid;
  const bool draw_grid = draw.draw_grid, draw_x_axis = draw.draw_x_axis, draw_z_axis = draw.draw_z_axis;
  const auto &x_axis = draw.x_axis;
  const auto &z_axis = draw.z_axis;
  if (physical) {
    // Annotations draw into the display image after the output transform, with the legacy stage.
    const auto target = p.rtv(frame_count), depth_view = p.dsv_heap->GetCPUDescriptorHandleForHeapStart();
    p.commands->OMSetRenderTargets(1, &target, FALSE, &depth_view);
    p.commands->SetGraphicsRootSignature(p.root.Get());
  }
  p.commands->IASetVertexBuffers(0, 1, &p.vertex_view);
  p.commands->SetGraphicsRootDescriptorTable(1, p.srv_gpu(index, 0));
  p.commands->SetPipelineState(p.lines.Get());
  p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
  if (draw_grid) {
    p.commands->SetGraphicsRoot32BitConstants(0, 44, &grid.value(), 0);
    p.commands->DrawInstanced(164, 1, 60, 0);
    // Preserve the absolute world axes when they intersect this bounded patch.
    if (draw_x_axis) {
      p.commands->SetGraphicsRoot32BitConstants(0, 44, &x_axis, 0);
      p.commands->DrawInstanced(2, 1, 224, 0);
    }
    if (draw_z_axis) {
      p.commands->SetGraphicsRoot32BitConstants(0, 44, &z_axis, 0);
      p.commands->DrawInstanced(2, 1, 226, 0);
    }
  }
  for (std::size_t i = 0; i < frame.draws.size(); ++i)
    if (frame.draws[i].selected) {
      const auto &range = p.mesh_ranges[i];
      auto outline = p.prepared[i];
      outline.color = {4.f, 1.3f, .06f, 1};
      outline.parameters[1] = 1;
      outline.parameters[3] = 0;
      outline.parameters[2] = range.count ? 1.f : 1.006f;
      p.commands->IASetVertexBuffers(0, 1, range.count ? &slot.mesh_view : &p.vertex_view);
      p.commands->SetGraphicsRoot32BitConstants(0, 44, &outline, 0);
      p.commands->DrawInstanced(range.count ? range.outline_count : 24, 1,
                                range.count ? range.outline_first : 36, 0);
    }
}

void WindowsViewport::Impl::record_overlays(const DrawFrame &draw, UINT index, Slot &slot) {
  auto &p = *this;
  const auto &frame = draw.frame;
  const bool physical = draw.physical;
  if (!frame.overlays.empty()) {
    if (physical) {
      const auto target = p.rtv(frame_count), depth_view = p.dsv_heap->GetCPUDescriptorHandleForHeapStart();
      p.commands->OMSetRenderTargets(1, &target, FALSE, &depth_view);
      p.commands->SetGraphicsRootSignature(p.root.Get());
    }
    p.commands->SetGraphicsRootDescriptorTable(1, p.srv_gpu(index, 0));
    p.commands->SetPipelineState(p.overlay_triangles.Get());
    p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    p.commands->IASetVertexBuffers(0, 1, &slot.mesh_view);
    for (std::size_t i = 0; i < frame.overlays.size(); ++i) {
      const auto &range = p.overlay_ranges[i];
      p.commands->SetGraphicsRoot32BitConstants(0, 44, &p.overlay_prepared[i], 0);
      p.commands->DrawInstanced(range.count, 1, range.first, 0);
    }
  }
}

void WindowsViewport::Impl::record_output() {
  auto &p = *this;
  // Exactly one evaluation of exposure, ReinhardRGB-v1 and the sRGB transfer from the
  // stored scene-linear pixels into the display image; the HDR target is only read.
  const auto target = p.rtv(frame_count);
  p.commands->OMSetRenderTargets(1, &target, FALSE, nullptr);
  p.commands->SetGraphicsRootSignature(p.output_root.Get());
  p.commands->SetPipelineState(p.output_transform.Get());
  p.commands->SetGraphicsRoot32BitConstants(0, 8, &p.output_constants, 0);
  p.commands->SetGraphicsRootDescriptorTable(1, p.hdr_srv_gpu());
  p.commands->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  p.commands->DrawInstanced(3, 1, 0, 0);
}

Result<void> WindowsViewport::Impl::record_draw(const DrawFrame &draw, UINT index, Slot &slot) {
  auto &p = *this;
  for (const auto &step : p.graph_work.plan->steps()) {
    p.record_graph_transitions(step, false);
    switch (p.graph_work.plan->passes()[step.pass].operation) {
    case graph::Operation::Upload:
      p.record_upload(slot);
      break;
    case graph::Operation::Opaque:
      p.record_opaque(draw, index, slot);
      break;
    case graph::Operation::GridSelection:
      p.record_grid(draw, index, slot);
      break;
    case graph::Operation::Overlay:
      p.record_overlays(draw, index, slot);
      break;
    case graph::Operation::OutputTransform:
      p.record_output();
      break;
    case graph::Operation::PresentCopy: {
      p.commands->CopyResource(p.targets[index].Get(), p.scene_color.Get());
      break;
    }
    case graph::Operation::TimingResolve: {
      p.commands->EndQuery(p.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2 + 1);
      p.commands->ResolveQueryData(p.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2, 2,
                                   p.timestamp_readback.Get(), index * 16);
      break;
    }
    case graph::Operation::CaptureCopy:
      return fail(ErrorCode::Invariant, "capture operation reached an admitted draw graph");
    }
    p.record_graph_transitions(step, true);
  }
  return {};
}

Result<void> WindowsViewport::Impl::draw_ready(const DrawFrame &draw, ProfileClock::time_point draw_started) {
  auto &p = *this;
  const auto &frame = draw.frame;
  const bool physical = draw.physical;
  const bool draw_grid = draw.draw_grid;
  const auto &lighting = draw.resolved;
  const auto texture_count = draw.texture_count;
  const UINT index = p.swapchain->GetCurrentBackBufferIndex();
  auto &slot = p.slots[index];
  const auto validation_finished = ProfileClock::now();
  Impl::WaitMetrics wait_metrics;
  auto completed = p.wait(slot.fence, &wait_metrics);
  if (!completed)
    return completed;
  const auto wait_finished = ProfileClock::now();
  const std::array<float, 4> background{lighting.background_radiance[0], lighting.background_radiance[1],
                                        lighting.background_radiance[2], 1};
  if (physical && (!p.physical_output || p.hdr_clear != background)) {
    // An output configuration change behaves like a resize: quiesce, preflight the whole
    // declared set, then install it or reject the frame with nothing partially installed.
    // A changed background radiance re-creates the FP16 target around that clear value.
    auto drained = p.drain();
    if (!drained)
      return drained;
    auto installed = p.install_output(p.information.width, p.information.height, background);
    if (!installed)
      return installed;
  }
  auto preflight = p.preflight_textures(texture_count);
  if (!preflight)
    return preflight;
  auto admitted_graph = p.admit_draw_graph(index, frame, draw_grid, texture_count, physical);
  if (!admitted_graph)
    return admitted_graph;
  auto textures = p.prepare_textures(index, texture_count);
  if (!textures)
    return textures;
  auto uploaded = p.upload_mesh(slot);
  if (!uploaded)
    return uploaded;
  auto bound_graph = p.bind_graph(index, texture_count);
  if (!bound_graph)
    return bound_graph;
  p.resolved = lighting;
  p.output_constants = {};
  p.output_constants.values[0] = lighting.exposure_scale;
  auto result = slot.allocator->Reset();
  if (FAILED(result))
    return p.native_error(result, "viewport allocator reset failed");
  result = p.commands->Reset(slot.allocator.Get(), p.triangles.Get());
  if (FAILED(result))
    return p.native_error(result, "viewport command reset failed");
  if (p.information.gpu_timing_available)
    p.commands->EndQuery(p.timestamp_heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, index * 2);
  auto recorded = p.record_draw(draw, index, slot);
  if (!recorded)
    return recorded;
  result = p.commands->Close();
  if (FAILED(result))
    return p.native_error(result, "viewport command close failed");
  const auto record_finished = ProfileClock::now();
  const DrawProfile profile{draw_started, validation_finished, wait_finished, record_finished, wait_metrics};
  return p.submit_draw(draw, index, slot, profile);
}

void WindowsViewport::Impl::publish_draw(const DrawFrame &draw) {
  auto &p = *this;
  const auto &frame = draw.frame;
  const bool physical = draw.physical;
  const bool draw_grid = draw.draw_grid;
  const auto &lighting = draw.resolved;
  const auto texture_count = draw.texture_count, texture_bytes = draw.texture_bytes,
             overlay_vertices = draw.overlay_vertices;
  p.information.scene_generation = frame.scene_generation;
  p.information.origin_revision = frame.origin_revision;
  p.information.simulation_tick = frame.simulation_tick;
  p.information.last_draw_count = frame.draws.size();
  p.information.grid_visible = draw_grid;
  p.information.lighting_profile = frame.lighting_profile;
  p.information.output_target = physical ? lighting.target : OutputTarget::DisplaySdr;
  p.information.exposure_scale = physical ? lighting.exposure_scale : 0.f;
  p.information.reference_luminance = physical ? lighting.reference_luminance : 0.f;
  p.information.overlay_draws = frame.overlays.size();
  p.information.overlay_vertices = overlay_vertices;
  p.information.frame_textures = texture_count;
  p.information.frame_texture_bytes = texture_bytes;
}

void WindowsViewport::Impl::publish_draw_profile(
    Slot &slot, const DrawProfile &timing, ProfileClock::time_point execute_started,
    ProfileClock::time_point execute_finished, ProfileClock::time_point present_started,
    ProfileClock::time_point present_finished, ProfileClock::time_point signal_started,
    ProfileClock::time_point signal_finished, ProfileClock::time_point completion_started,
    ProfileClock::time_point draw_finished) {
  auto &p = *this;
  auto &profile = p.information;
  profile.last_cpu_frame = slot.frame;
  profile.last_validation_cpu_ms = elapsed_ms(timing.draw_started, timing.validation_finished);
  profile.last_inflight_wait_cpu_ms = elapsed_ms(timing.validation_finished, timing.wait_finished);
  profile.last_pending_fence_cpu_ms = timing.wait_metrics.pending_ms;
  profile.last_inflight_wait_pending = timing.wait_metrics.pending;
  if (timing.wait_metrics.pending)
    ++profile.inflight_wait_count;
  profile.total_inflight_wait_cpu_ms += profile.last_inflight_wait_cpu_ms;
  profile.last_record_cpu_ms = elapsed_ms(timing.wait_finished, timing.record_finished);
  profile.last_execute_cpu_ms = elapsed_ms(execute_started, execute_finished);
  profile.last_present_cpu_ms = elapsed_ms(present_started, present_finished);
  profile.last_signal_cpu_ms = elapsed_ms(signal_started, signal_finished);
  profile.last_completion_cpu_ms = elapsed_ms(completion_started, draw_finished);
  profile.last_draw_cpu_ms = elapsed_ms(timing.draw_started, draw_finished);
}

Result<void> WindowsViewport::Impl::submit_draw(const DrawFrame &draw, UINT index, Slot &slot,
                                                const DrawProfile &timing) {
  auto &p = *this;
  ID3D12CommandList *lists[]{p.commands.Get()};
  const auto execute_started = ProfileClock::now();
  auto capacity = p.require_signal_capacity(1);
  if (!capacity)
    return capacity;
  p.queue->ExecuteCommandLists(1, lists);
  const auto execute_finished = ProfileClock::now();
  p.submitted_without_fence = true;
  p.publish_graph_submission();
  // Graph final states are now queued. Do not roll residency back if Present or
  // Signal fails; fence/drain ownership still protects every submitted resource.
  for (auto &entry : slot.textures)
    if (entry.packet) {
      if (entry.resident)
        ++p.information.texture_cache_hits;
      else {
        entry.resident = true;
        ++p.information.texture_uploads;
        p.information.texture_upload_bytes += entry.upload_bytes;
      }
    }
  const auto present_started = ProfileClock::now();
  const auto presented = p.swapchain->Present(1, 0);
  const auto present_finished = ProfileClock::now();
  const auto signal_started = ProfileClock::now();
  auto signaled = p.signal();
  if (!signaled) {
    p.has_image = false;
    return signaled.error();
  }
  const auto signal_finished = ProfileClock::now();
  slot.fence = signaled.value();
  slot.frame = ++p.information.submitted_frames;
  slot.timing = p.information.gpu_timing_available;
  p.last_target = index;
  p.has_image = SUCCEEDED(presented);
  p.publish_draw(draw);
  const auto completion_started = ProfileClock::now();
  for (auto &completed_slot : p.slots)
    p.collect(completed_slot);
  p.read_diagnostics();
  const auto draw_finished = ProfileClock::now();
  p.publish_draw_profile(slot, timing, execute_started, execute_finished, present_started, present_finished,
                         signal_started, signal_finished, completion_started, draw_finished);
  if (FAILED(presented))
    return p.native_error(presented, "D3D12 viewport presentation failed");
  if (p.information.device_lost)
    return fail(ErrorCode::PlatformFailure, "D3D12 device removed during completion");
  return {};
}
} // namespace race::render
