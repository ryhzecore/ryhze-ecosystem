#include "windows_viewport_internal.hpp"

namespace race::render {
D3D12_CPU_DESCRIPTOR_HANDLE WindowsViewport::Impl::rtv(UINT index) const {
  auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(index) * rtv_increment;
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE WindowsViewport::Impl::srv_cpu(UINT slot, UINT texture) const {
  auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(slot * texture_descriptors + texture) * srv_increment;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE WindowsViewport::Impl::srv_gpu(UINT slot, UINT texture) const {
  auto handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(slot * texture_descriptors + texture) * srv_increment;
  return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE WindowsViewport::Impl::hdr_srv_cpu() const {
  auto handle = srv_heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(frame_count * texture_descriptors) * srv_increment;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE WindowsViewport::Impl::hdr_srv_gpu() const {
  auto handle = srv_heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(frame_count * texture_descriptors) * srv_increment;
  return handle;
}

void WindowsViewport::Impl::texture_view(ID3D12Resource *resource, UINT slot, UINT texture, UINT mips,
                                         DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Texture2D.MipLevels = mips;
  device->CreateShaderResourceView(resource, &desc, srv_cpu(slot, texture));
}

Result<void> WindowsViewport::Impl::preflight_textures(std::size_t count) {
  // Preflight actual allocation sizes, including GPU alignment and row pitch,
  // before changing the cache. No vector growth or texture decoding occurs here.
  auto &descriptions = texture_preparation.descriptions;
  auto &footprints = texture_preparation.footprints;
  auto &row_counts = texture_preparation.row_counts;
  auto &row_sizes = texture_preparation.row_sizes;
  auto &upload_sizes = texture_preparation.upload_sizes;
  UINT64 gpu_total{}, upload_total{};
  for (std::size_t i = 0; i < count; ++i) {
    const auto &packet = *texture_packets[i];
    auto &desc = descriptions[i];
    // Reusable preflight storage must not inherit query outputs on failure.
    desc = {};
    footprints[i].fill({});
    row_counts[i].fill(0);
    row_sizes[i].fill(0);
    upload_sizes[i] = 0;
    const auto format = packet.encoding() == TextureEncoding::LinearData ? ResourceFormat::Rgba8Unorm
                                                                         : ResourceFormat::Rgba8Srgb;
    auto sampled = validate_viewport_sampled_format(format);
    if (!sampled)
      return sampled.error();
    auto logical = validate_resource(
        {ResourceKind::Texture2D, format, CopyDestination | ShaderSample, 0, packet.width(), packet.height(),
         packet.mip_count(), 1},
        {viewport_texture_frame_bytes, viewport_texture_max_dimension, viewport_texture_max_mips});
    if (!logical)
      return logical.error();
    if (logical.value() != packet.texels().size())
      return fail(ErrorCode::Invariant, "viewport texture description payload differs");
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = packet.width();
    desc.Height = packet.height();
    desc.DepthOrArraySize = 1;
    desc.MipLevels = static_cast<UINT16>(packet.mip_count());
    desc.Format = packet_format(packet);
    desc.SampleDesc.Count = 1;
    const auto allocation = device->GetResourceAllocationInfo(0, 1, &desc);
    device->GetCopyableFootprints(&desc, 0, packet.mip_count(), 0, footprints[i].data(), row_counts[i].data(),
                                  row_sizes[i].data(), &upload_sizes[i]);
    if (!upload_sizes[i] || upload_sizes[i] > viewport_texture_slot_upload_bytes)
      return fail(ErrorCode::Exhausted, "viewport texture footprint exceeds upload bounds");
    UINT64 previous_end{};
    for (UINT level = 0; level < packet.mip_count(); ++level) {
      const auto mip = packet.mip(level);
      const auto &footprint = footprints[i][level];
      if (!mip || footprint.Footprint.Format != desc.Format || footprint.Footprint.Depth != 1 ||
          footprint.Footprint.Width != mip->width || footprint.Footprint.Height != mip->height ||
          row_counts[i][level] != mip->height || row_sizes[i][level] != static_cast<UINT64>(mip->width) * 4 ||
          footprint.Footprint.RowPitch < row_sizes[i][level] ||
          footprint.Footprint.RowPitch % D3D12_TEXTURE_DATA_PITCH_ALIGNMENT ||
          footprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT || footprint.Offset < previous_end ||
          footprint.Offset > upload_sizes[i])
        return fail(ErrorCode::Exhausted, "viewport mip footprint dimensions or placement invalid");
      const auto level_bytes =
          static_cast<UINT64>(footprint.Footprint.RowPitch) * (mip->height - 1) + row_sizes[i][level];
      if (level_bytes > upload_sizes[i] - footprint.Offset)
        return fail(ErrorCode::Exhausted, "viewport mip footprint exceeds upload bounds");
      previous_end = footprint.Offset + level_bytes;
    }
    const auto upload_desc = buffer(upload_sizes[i]);
    const auto upload_allocation = device->GetResourceAllocationInfo(0, 1, &upload_desc);
    if (!allocation.SizeInBytes || !upload_allocation.SizeInBytes ||
        allocation.SizeInBytes > viewport_texture_slot_gpu_bytes - gpu_total ||
        upload_allocation.SizeInBytes > viewport_texture_slot_upload_bytes - upload_total)
      return fail(ErrorCode::Exhausted, "viewport texture GPU/upload allocation budget exceeded");
    gpu_total += allocation.SizeInBytes;
    upload_total += upload_allocation.SizeInBytes;
  }
  return {};
}

Result<void> WindowsViewport::Impl::prepare_textures(UINT slot_index, std::size_t count) {
  // Preflight and prospective graph admission already passed. This slot is
  // fenced. Partial allocation failure may retain new nonresident entries;
  // the next candidate re-inspects that actual cache instead of assuming rollback.
  const auto &descriptions = texture_preparation.descriptions;
  const auto &footprints = texture_preparation.footprints;
  const auto &upload_sizes = texture_preparation.upload_sizes;
  auto &cache = slots[slot_index].textures;
  for (auto &entry : cache) {
    bool needed = false;
    for (std::size_t i = 0; i < count; ++i)
      if (entry.packet == texture_packets[i]) {
        needed = true;
        break;
      }
    if (!needed)
      entry = {}; // Retire before growth; never momentarily double the slot budget.
    else if (entry.resident)
      entry.upload.Reset(); // Completed copy; GPU texture stays cached.
  }
  for (std::size_t i = 0; i < count; ++i) {
    std::size_t position = cache.size();
    for (std::size_t j = 0; j < cache.size(); ++j)
      if (cache[j].packet == texture_packets[i]) {
        position = j;
        break;
      }
    if (position == cache.size()) {
      for (std::size_t j = 0; j < cache.size(); ++j)
        if (!cache[j].packet) {
          position = j;
          break;
        }
      if (position == cache.size())
        return fail(ErrorCode::Exhausted, "viewport texture cache exhausted");
      TextureEntry entry;
      entry.footprints = footprints[i];
      entry.upload_bytes = upload_sizes[i];
      const auto gpu = heap(D3D12_HEAP_TYPE_DEFAULT), upload = heap(D3D12_HEAP_TYPE_UPLOAD);
      auto result = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &descriptions[i],
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&entry.resource));
      if (FAILED(result))
        return native_error(result, "viewport texture allocation failed");
      const auto upload_desc = buffer(entry.upload_bytes);
      result = device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &upload_desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&entry.upload));
      if (FAILED(result))
        return native_error(result, "viewport texture upload allocation failed");
      void *mapped{};
      const D3D12_RANGE no_read{0, 0};
      result = entry.upload->Map(0, &no_read, &mapped);
      if (FAILED(result))
        return native_error(result, "viewport texture upload map failed");
      const auto &packet = *texture_packets[i];
      for (UINT level = 0; level < packet.mip_count(); ++level) {
        const auto mip = packet.mip(level).value();
        const auto &footprint = entry.footprints[level];
        const auto row_bytes = static_cast<std::size_t>(mip.width) * 4;
        for (std::size_t y = 0; y < mip.height; ++y)
          std::memcpy(static_cast<std::byte *>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch,
                      mip.pixels.data() + y * row_bytes, row_bytes);
      }
      const D3D12_RANGE written{0, static_cast<SIZE_T>(entry.upload_bytes)};
      entry.upload->Unmap(0, &written);
      entry.packet = texture_packets[i];
      cache[position] = std::move(entry);
    }
    texture_bindings[i] = static_cast<UINT>(position) + 1;
    texture_view(cache[position].resource.Get(), slot_index, texture_bindings[i],
                 texture_packets[i]->mip_count(), packet_format(*texture_packets[i]));
  }
  return {};
}

Result<void> WindowsViewport::Impl::make_targets(UINT width, UINT height) {
  for (const auto format : {ResourceFormat::Rgba8Unorm, ResourceFormat::Depth32Float}) {
    const auto use = format == ResourceFormat::Depth32Float ? std::uint32_t(DepthTarget)
                                                            : std::uint32_t(ColorTarget | CopySource);
    auto valid = validate_resource({ResourceKind::Texture2D, format, use, 0, width, height, 1, 1});
    if (!valid)
      return valid.error();
  }
  for (UINT i = 0; i < frame_count; ++i) {
    const auto result = swapchain->GetBuffer(i, IID_PPV_ARGS(&targets[i]));
    if (FAILED(result))
      return native_error(result, "swapchain buffer acquisition failed");
    device->CreateRenderTargetView(targets[i].Get(), nullptr, rtv(i));
  }
  // Keep an owned color image: flip-discard back-buffer contents are not a
  // durable readback source after Present. Queue ordering protects this target.
  D3D12_RESOURCE_DESC color{};
  color.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  color.Width = width;
  color.Height = height;
  color.DepthOrArraySize = 1;
  color.MipLevels = 1;
  color.Format = color_format;
  color.SampleDesc.Count = 1;
  color.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const auto color_heap = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CLEAR_VALUE optimal{};
  optimal.Format = color_format;
  optimal.Color[0] = display(.015f);
  optimal.Color[1] = display(.02f);
  optimal.Color[2] = display(.032f);
  optimal.Color[3] = 1;
  const auto allocated =
      device->CreateCommittedResource(&color_heap, D3D12_HEAP_FLAG_NONE, &color,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE, &optimal, IID_PPV_ARGS(&scene_color));
  if (FAILED(allocated))
    return native_error(allocated, "viewport retained color allocation failed");
  device->CreateRenderTargetView(scene_color.Get(), nullptr, rtv(frame_count));
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = depth_format;
  desc.SampleDesc.Count = 1;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  const auto properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  D3D12_CLEAR_VALUE clear{};
  clear.Format = depth_format;
  clear.DepthStencil.Depth = 1;
  const auto result =
      device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &desc,
                                      D3D12_RESOURCE_STATE_DEPTH_WRITE, &clear, IID_PPV_ARGS(&depth));
  if (FAILED(result))
    return native_error(result, "viewport depth allocation failed");
  device->CreateDepthStencilView(depth.Get(), nullptr, dsv_heap->GetCPUDescriptorHandleForHeapStart());
  // A resize of an installed physical output re-preflights and recreates its target too.
  if (physical_output) {
    auto installed = install_output(width, height, hdr_clear);
    if (!installed)
      return installed;
  }
  information.width = width;
  information.height = height;
  return {};
}

Result<void> WindowsViewport::Impl::query_output_allocations(
    const std::array<const D3D12_RESOURCE_DESC *, 6> &descriptions, std::array<std::uint64_t, 6> &api) {
  for (std::size_t i = 0; i < api.size(); ++i) {
    const auto info = device->GetResourceAllocationInfo(0, 1, descriptions[i]);
    if (!info.SizeInBytes || info.SizeInBytes == UINT64_MAX)
      return fail(ErrorCode::PlatformFailure, "viewport output allocation query failed");
    api[i] = info.SizeInBytes;
  }
  return {};
}

Result<void> WindowsViewport::Impl::install_output(UINT width, UINT height,
                                                   const std::array<float, 4> &clear) {
  const ResourceDescription description{ResourceKind::Texture2D,
                                        ResourceFormat::Rgba16Float,
                                        ColorTarget | ShaderSample | CopySource,
                                        0,
                                        width,
                                        height,
                                        1,
                                        1};
  auto admitted = validate_output_resource(description, OutputResourceClass::HdrAttachment);
  if (!admitted)
    return admitted.error();
  D3D12_RESOURCE_DESC hdr{};
  hdr.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  hdr.Width = width;
  hdr.Height = height;
  hdr.DepthOrArraySize = 1;
  hdr.MipLevels = 1;
  hdr.Format = hdr_format;
  hdr.SampleDesc.Count = 1;
  hdr.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  auto color = hdr;
  color.Format = color_format;
  auto depth_desc = hdr;
  depth_desc.Format = depth_format;
  depth_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  UINT64 readback_bytes{};
  device->GetCopyableFootprints(&hdr, 0, 1, 0, nullptr, nullptr, nullptr, &readback_bytes);
  auto readback_class = validate_output_resource(
      {ResourceKind::Buffer, ResourceFormat::Bytes, CopyDestination, readback_bytes, 1, 1, 1, 1},
      OutputResourceClass::HdrReadback);
  if (!readback_class)
    return readback_class.error();
  const auto readback_desc = buffer(readback_bytes);
  const std::array<const D3D12_RESOURCE_DESC *, 6> descriptions{&hdr,   &color, &depth_desc,
                                                                &color, &color, &readback_desc};
  std::array<std::uint64_t, 6> api{};
  auto queried = query_output_allocations(descriptions, api);
  if (!queried)
    return queried;
  auto total = admit_output_allocations(api, OutputTarget::DisplaySdr);
  if (!total)
    return total.error();
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
  if (output_budget_override && total.value() > output_budget_override)
    return fail(ErrorCode::Exhausted, "test-limited output ceiling rejected the owned output set");
#endif
  ComPtr<IDXGIAdapter3> adapter;
  DXGI_QUERY_VIDEO_MEMORY_INFO memory{};
  const bool observed =
      factory && SUCCEEDED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&adapter))) &&
      SUCCEEDED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &memory));
  const auto properties = heap(D3D12_HEAP_TYPE_DEFAULT);
  // The optimized clear value is the background radiance this target is created for and the
  // Opaque pass clears with exactly that value, so the debug layer raises no clear-value
  // warning. A physical frame with another background re-creates the target after the fence
  // wait (draw()); a failed creation leaves the previous target installed and untouched.
  D3D12_CLEAR_VALUE optimal{};
  optimal.Format = hdr_format;
  std::copy(clear.begin(), clear.end(), optimal.Color);
  ComPtr<ID3D12Resource> target;
  const auto created =
      device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE, &hdr,
                                      D3D12_RESOURCE_STATE_COPY_SOURCE, &optimal, IID_PPV_ARGS(&target));
  if (FAILED(created))
    return native_error(created, "viewport scene-linear target allocation failed");
  hdr_color = std::move(target);
  hdr_clear = clear;
  ++information.hdr_target_installs;
  install_output_views();
  physical_output = true;
  information.physical_output_installed = true;
  information.output_allocation_bytes = total.value();
  information.output_budget_bytes = output_budget_bytes(OutputTarget::DisplaySdr);
  information.hdr_target_bytes = api[0];
  information.video_memory_budget_bytes = observed ? memory.Budget : 0;
  information.video_memory_usage_bytes = observed ? memory.CurrentUsage : 0;
  return {};
}
void WindowsViewport::Impl::install_output_views() {
  device->CreateRenderTargetView(hdr_color.Get(), nullptr, rtv(frame_count + 1));
  D3D12_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = hdr_format;
  view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  view.Texture2D.MipLevels = 1;
  device->CreateShaderResourceView(hdr_color.Get(), &view, hdr_srv_cpu());
}
} // namespace race::render
