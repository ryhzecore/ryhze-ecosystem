#include "windows_viewport_internal.hpp"

namespace race::render {
namespace {
// Exact IEEE binary16 -> binary32 widening of a stored scene-linear texel.
float half_to_float(std::uint16_t half) noexcept {
  const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000u) << 16,
                      exponent = (half >> 10) & 0x1Fu, mantissa = half & 0x3FFu;
  std::uint32_t bits{};
  if (exponent == 0) {
    if (mantissa) {
      int shift = -1;
      std::uint32_t m = mantissa;
      do {
        ++shift;
        m <<= 1;
      } while (!(m & 0x400u));
      bits = sign | ((112u - static_cast<std::uint32_t>(shift)) << 23) | ((m & 0x3FFu) << 13);
    } else
      bits = sign;
  } else if (exponent == 31)
    bits = sign | 0x7F800000u | (mantissa << 13);
  else
    bits = sign | ((exponent + 112u) << 23) | (mantissa << 13);
  float value{};
  std::memcpy(&value, &bits, sizeof value);
  return value;
}
bool write_display_image(const std::filesystem::path &path, UINT width, UINT height, const void *mapped,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  bool written = false;
  try {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << "P6\n" << width << ' ' << height << "\n255\n";
    std::array<char, viewport_max_dimension * 3> row{};
    for (UINT y = 0; y < height && out; ++y) {
      const auto *source = static_cast<const unsigned char *>(mapped) + footprint.Offset +
                           static_cast<SIZE_T>(y) * footprint.Footprint.RowPitch;
      for (UINT x = 0; x < width; ++x)
        for (UINT c = 0; c < 3; ++c)
          row[x * 3 + c] = static_cast<char>(source[x * 4 + c]);
      out.write(row.data(), static_cast<std::streamsize>(width) * 3);
    }
    out.close();
    written = static_cast<bool>(out);
  } catch (...) {
    written = false;
  }
  return written;
}
void widen_linear_image(const SceneLinearCapture &capture, const void *mapped,
                        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint, std::span<std::byte> raw_bytes,
                        std::span<std::byte> pfm, std::size_t header_size) {
  for (UINT y = 0; y < capture.height; ++y) {
    const auto *source = static_cast<const unsigned char *>(mapped) + footprint.Offset +
                         static_cast<SIZE_T>(y) * footprint.Footprint.RowPitch;
    std::memcpy(raw_bytes.data() + static_cast<std::size_t>(y) * capture.width * 8, source,
                static_cast<std::size_t>(capture.width) * 8);
    // PFM rows run bottom to top; little-endian float32 RGB widened exactly from the stored halves.
    auto *row =
        pfm.data() + header_size + static_cast<std::size_t>(capture.height - 1 - y) * capture.width * 12;
    for (UINT x = 0; x < capture.width; ++x)
      for (UINT c = 0; c < 3; ++c) {
        const auto half = static_cast<std::uint16_t>(source[x * 8 + c * 2] |
                                                     (static_cast<unsigned>(source[x * 8 + c * 2 + 1]) << 8));
        const float value = half_to_float(half);
        std::memcpy(row + (static_cast<std::size_t>(x) * 3 + c) * 4, &value, sizeof value);
      }
  }
}
Result<void> write_linear_image(const std::filesystem::path &image, const std::filesystem::path &raw,
                                SceneLinearCapture &capture, const void *mapped,
                                const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint) {
  Error failure{ErrorCode::IoFailure, "scene-linear capture write failed"};
  bool written = false;
  try {
    const std::size_t pixels = static_cast<std::size_t>(capture.width) * capture.height;
    std::vector<std::byte> raw_bytes(pixels * 8);
    const std::string header =
        "PF\n" + std::to_string(capture.width) + " " + std::to_string(capture.height) + "\n-1.0\n";
    std::vector<std::byte> pfm(header.size() + pixels * 12);
    std::memcpy(pfm.data(), header.data(), header.size());
    widen_linear_image(capture, mapped, footprint, raw_bytes, pfm, header.size());
    capture.raw_sha256 = sha256(raw_bytes);
    capture.image_sha256 = sha256(pfm);
    const auto write = [](const std::filesystem::path &path, std::span<const std::byte> data) {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(reinterpret_cast<const char *>(data.data()), static_cast<std::streamsize>(data.size()));
      out.close();
      return !out.fail();
    };
    written = write(raw, raw_bytes) && write(image, pfm);
  } catch (const std::bad_alloc &) {
    failure = fail(ErrorCode::Exhausted, "scene-linear capture staging allocation failed");
  } catch (...) {
    written = false;
  }
  if (!written)
    return failure;
  return {};
}

} // namespace
Result<WindowsViewport::ImageToken> WindowsViewport::image_token() const {
  auto own = owner_check();
  if (!own)
    return own.error();
  if (!impl_)
    return fail(ErrorCode::FailedPrerequisite, "viewport has no native surface");
  auto &p = *impl_;
  if (p.removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 viewport device removed");
  if (!p.has_image || p.information.minimized)
    return fail(ErrorCode::Busy, "viewport capture requires a drawn visible-size frame");
  return ImageToken{image_owner_, epoch_, p.information.submitted_frames};
}
Result<void> WindowsViewport::capture(const std::filesystem::path &path) {
  auto token = image_token();
  if (!token)
    return token.error();
  return capture(path, token.value());
}
Result<void> WindowsViewport::capture(const std::filesystem::path &path, const ImageToken &token) {
  auto own = owner_check();
  if (!own)
    return own;
  // Reject stale/cross-owner identity before any GPU work, file open or output mutation.
  if (token.owner_.lock() != image_owner_ || !impl_ || token.epoch_ != epoch_ || !token.frame_ ||
      token.frame_ != impl_->information.submitted_frames || !impl_->has_image ||
      impl_->information.minimized)
    return fail(ErrorCode::StaleHandle, "viewport image token is stale or belongs to another owner");
  auto &p = *impl_;
  if (p.removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 viewport device removed");
  auto capacity = p.require_signal_capacity(2);
  if (!capacity)
    return capacity; // Predrain + copy completion.
  auto drained = p.drain();
  if (!drained)
    return drained;
  auto &slot = p.slots[p.last_target];
  const auto texture = p.scene_color->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes{};
  p.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  if (bytes > static_cast<UINT64>(viewport_max_dimension) * viewport_max_dimension * 4)
    return fail(ErrorCode::Exhausted, "viewport readback budget exceeded");
  auto copied = p.copy_display_image(slot, footprint, bytes);
  if (!copied)
    return copied.error();
  void *mapped{};
  const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
  auto result = p.capture_readback->Map(0, &range, &mapped);
  if (FAILED(result))
    return p.native_error(result, "viewport readback map failed");
  const bool written =
      write_display_image(path, p.information.width, p.information.height, mapped, footprint);
  const D3D12_RANGE no_write{0, 0};
  p.capture_readback->Unmap(0, &no_write);
  p.read_diagnostics();
  if (!written)
    return fail(ErrorCode::IoFailure, "viewport PPM capture write failed");
  return {};
}
Result<SceneLinearCapture> WindowsViewport::capture_scene_linear(const std::filesystem::path &image,
                                                                 const std::filesystem::path &raw,
                                                                 const ImageToken &token) {
  auto own = owner_check();
  if (!own)
    return own.error();
  if (token.owner_.lock() != image_owner_ || !impl_ || token.epoch_ != epoch_ || !token.frame_ ||
      token.frame_ != impl_->information.submitted_frames || !impl_->has_image ||
      impl_->information.minimized)
    return fail(ErrorCode::StaleHandle, "viewport image token is stale or belongs to another owner");
  auto &p = *impl_;
  if (!p.physical_output || !p.hdr_color ||
      p.information.lighting_profile != LightingProfile::PhysicalDiffuseV1)
    return fail(ErrorCode::FailedPrerequisite,
                "scene-linear capture requires a submitted PhysicalDiffuseV1 frame");
  if (p.removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 viewport device removed");
  auto capacity = p.require_signal_capacity(2);
  if (!capacity)
    return capacity.error();
  auto drained = p.drain();
  if (!drained)
    return drained.error();
  auto &slot = p.slots[p.last_target];
  const auto texture = p.hdr_color->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes{};
  p.device->GetCopyableFootprints(&texture, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  auto admitted = validate_output_resource(
      {ResourceKind::Buffer, ResourceFormat::Bytes, CopyDestination, bytes, 1, 1, 1, 1},
      OutputResourceClass::HdrReadback);
  if (!admitted)
    return admitted.error();
  auto copied = p.copy_linear_image(slot, footprint, bytes);
  if (!copied)
    return copied.error();
  void *mapped{};
  const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)};
  auto result = p.hdr_readback->Map(0, &range, &mapped);
  if (FAILED(result))
    return p.native_error(result, "scene-linear readback map failed");
  SceneLinearCapture capture{};
  capture.width = p.information.width;
  capture.height = p.information.height;
  auto written = write_linear_image(image, raw, capture, mapped, footprint);
  const D3D12_RANGE no_write{0, 0};
  p.hdr_readback->Unmap(0, &no_write);
  p.read_diagnostics();
  if (!written)
    return written.error();
  capture.submitted_frame = p.information.submitted_frames;
  capture.device_epoch = epoch_;
  capture.scene_generation = p.information.scene_generation;
  capture.origin_revision = p.information.origin_revision;
  capture.simulation_tick = p.information.simulation_tick;
  capture.exposure_scale = p.resolved.exposure_scale;
  capture.reference_luminance = p.resolved.reference_luminance;
  capture.requested_target = p.resolved.target;
  capture.effective_target = OutputTarget::DisplaySdr;
  capture.dxgi_format = static_cast<std::uint32_t>(hdr_format);
  capture.annotations_included = false;
  return capture;
}

Result<void> WindowsViewport::Impl::copy_display_image(Slot &slot,
                                                       const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                                                       UINT64 bytes) {
  auto &p = *this;
  auto admitted_graph = p.admit_capture_graph(bytes);
  if (!admitted_graph)
    return admitted_graph;
  const auto readback = heap(D3D12_HEAP_TYPE_READBACK);
  const auto desc = buffer(bytes);
  p.capture_readback.Reset();
  auto result = p.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&p.capture_readback));
  if (FAILED(result))
    return p.native_error(result, "viewport readback allocation failed");
  auto bound_graph = p.bind_graph(p.last_target, 0);
  if (!bound_graph)
    return bound_graph;
  result = slot.allocator->Reset();
  if (FAILED(result))
    return p.native_error(result, "capture allocator reset failed");
  result = p.commands->Reset(slot.allocator.Get(), nullptr);
  if (FAILED(result))
    return p.native_error(result, "capture command reset failed");
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = p.scene_color.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = p.capture_readback.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  for (const auto &step : p.graph_work.plan->steps()) {
    p.record_graph_transitions(step, false);
    p.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    p.record_graph_transitions(step, true);
  }
  result = p.commands->Close();
  if (FAILED(result))
    return p.native_error(result, "capture command close failed");
  auto capacity = p.require_signal_capacity(1);
  if (!capacity)
    return capacity;
  ID3D12CommandList *lists[]{p.commands.Get()};
  p.queue->ExecuteCommandLists(1, lists);
  p.submitted_without_fence = true;
  p.publish_graph_submission();
  auto signaled = p.signal();
  if (!signaled)
    return signaled.error();
  slot.fence = signaled.value();
  auto complete = p.wait(slot.fence);
  if (!complete)
    return complete;

  return {};
}

Result<void> WindowsViewport::Impl::copy_linear_image(Slot &slot,
                                                      const D3D12_PLACED_SUBRESOURCE_FOOTPRINT &footprint,
                                                      UINT64 bytes) {
  auto &p = *this;
  auto admitted_graph = p.admit_capture_graph(bytes, true);
  if (!admitted_graph)
    return admitted_graph.error();
  const auto readback = heap(D3D12_HEAP_TYPE_READBACK);
  const auto desc = buffer(bytes);
  p.hdr_readback.Reset();
  auto result = p.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&p.hdr_readback));
  if (FAILED(result))
    return p.native_error(result, "scene-linear readback allocation failed");
  auto bound_graph = p.bind_graph(p.last_target, 0);
  if (!bound_graph)
    return bound_graph.error();
  result = slot.allocator->Reset();
  if (FAILED(result))
    return p.native_error(result, "scene-linear capture allocator reset failed");
  result = p.commands->Reset(slot.allocator.Get(), nullptr);
  if (FAILED(result))
    return p.native_error(result, "scene-linear capture command reset failed");
  D3D12_TEXTURE_COPY_LOCATION from{};
  from.pResource = p.hdr_color.Get();
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  D3D12_TEXTURE_COPY_LOCATION to{};
  to.pResource = p.hdr_readback.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  for (const auto &step : p.graph_work.plan->steps()) {
    p.record_graph_transitions(step, false);
    p.commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    p.record_graph_transitions(step, true);
  }
  result = p.commands->Close();
  if (FAILED(result))
    return p.native_error(result, "scene-linear capture command close failed");
  auto capacity = p.require_signal_capacity(1);
  if (!capacity)
    return capacity.error();
  ID3D12CommandList *lists[]{p.commands.Get()};
  p.queue->ExecuteCommandLists(1, lists);
  p.submitted_without_fence = true;
  p.publish_graph_submission();
  auto signaled = p.signal();
  if (!signaled)
    return signaled.error();
  slot.fence = signaled.value();
  auto complete = p.wait(slot.fence);
  if (!complete)
    return complete.error();

  return {};
}
} // namespace race::render
