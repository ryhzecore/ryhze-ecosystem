#include "windows_viewport_internal.hpp"
#include "race/viewport_shader_identity.hpp"
#include "race/light_output_shader_identity.hpp"

namespace race::render {
namespace {
Result<std::vector<char>> shader(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return fail(ErrorCode::IoFailure, "compiled viewport DXIL shader missing");
  const auto size = file.tellg();
  if (size < 32 || size > 4 * 1024 * 1024)
    return fail(ErrorCode::InvalidInput, "invalid bounded viewport shader size");
  std::vector<char> data(static_cast<std::size_t>(size));
  file.seekg(0);
  if (!file.read(data.data(), static_cast<std::streamsize>(data.size())) ||
      std::memcmp(data.data(), "DXBC", 4) != 0)
    return fail(ErrorCode::InvalidInput, "viewport shader is not a DXIL container");
  const auto u32 = [&](std::size_t offset) {
    std::uint32_t value{};
    for (unsigned byte = 0; byte < 4; ++byte)
      value |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[offset + byte])) << (byte * 8);
    return value;
  };
  const auto parts = u32(28);
  if (u32(24) != data.size() || !parts || parts > 64 ||
      32 + static_cast<std::size_t>(parts) * 4 > data.size())
    return fail(ErrorCode::InvalidInput, "invalid DXIL container table");
  bool dxil = false;
  for (std::uint32_t i = 0; i < parts; ++i) {
    const auto offset = static_cast<std::size_t>(u32(32 + static_cast<std::size_t>(i) * 4));
    if (offset < 32 + static_cast<std::size_t>(parts) * 4 || offset > data.size() - 8 ||
        u32(offset + 4) > data.size() - offset - 8)
      return fail(ErrorCode::InvalidInput, "invalid DXIL container part");
    if (std::memcmp(data.data() + offset, "DXIL", 4) == 0 && u32(offset + 4) >= 16)
      dxil = true;
  }
  if (!dxil)
    return fail(ErrorCode::Unsupported, "DXC DXIL shader required; legacy DXBC shader rejected");
  return data;
}
template <std::size_t Bytes>
Result<std::array<std::byte, Bytes>> shader_manifest(const std::filesystem::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  if (!file)
    return fail(ErrorCode::IoFailure, "viewport shader manifest unavailable");
  if (file.tellg() != static_cast<std::streamoff>(Bytes))
    return fail(ErrorCode::InvalidInput, "viewport shader manifest size differs");
  std::array<std::byte, Bytes> bytes{};
  file.seekg(0);
  if (!file.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    return fail(ErrorCode::IoFailure, "viewport shader manifest read failed");
  return bytes;
}
} // namespace

#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
Result<NativeRemovalStatus> WindowsViewportRemovalProbe::observe(WindowsViewport &owner) {
  auto own = owner.owner_check();
  if (!own)
    return own.error();
  if (!owner.impl_ || !owner.impl_->device || !owner.impl_->fence)
    return fail(ErrorCode::FailedPrerequisite, "native removal probe requires its live test-owned device");
  const auto &p = *owner.impl_;
  ComPtr<ID3D12Device5> device5;
  const auto queried = p.device.As(&device5);
  const auto luid = p.device->GetAdapterLuid();
  NativeRemovalStatus result{static_cast<std::int64_t>(p.device->GetDeviceRemovedReason()),
                             static_cast<std::int64_t>(queried), p.fence->GetCompletedValue(), luid.LowPart,
                             luid.HighPart};
  result.next_signal = p.next_fence;
  result.graph_plan_installed = p.graph_work.plan.has_value();
  for (const auto &slot : p.slots)
    for (const auto &entry : slot.textures)
      if (entry.packet && !entry.resident)
        ++result.nonresident_textures;
  if (p.diagnostics) {
    const auto count = p.diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
    result.messages_discarded = p.diagnostics->GetNumMessagesDiscardedByMessageCountLimit();
    result.diagnostics_incomplete = count > 256 || result.messages_discarded != 0;
    constexpr char sentinel[] = "RACE P03.03 diagnostic retention sentinel";
    for (UINT64 i = 0; i < count && i < 256; ++i) {
      alignas(D3D12_MESSAGE) std::array<std::byte, 4096> storage{};
      SIZE_T size = storage.size();
      auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
      if (FAILED(p.diagnostics->GetMessage(i, message, &size))) {
        result.diagnostics_incomplete = true;
        continue;
      }
      const bool expected = message->ID == D3D12_MESSAGE_ID_STRING_FROM_APPLICATION &&
                            message->Severity == D3D12_MESSAGE_SEVERITY_WARNING &&
                            message->DescriptionByteLength == sizeof(sentinel) &&
                            std::memcmp(message->pDescription, sentinel, sizeof(sentinel)) == 0;
      if (expected)
        ++result.sentinel_warnings;
      else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
        ++result.unexpected_warnings;
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
          message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++result.debug_errors;
      result.last_message_id = static_cast<std::uint32_t>(message->ID);
      const auto bytes = std::min(message->DescriptionByteLength, result.last_message.size() - 1);
      std::memcpy(result.last_message.data(), message->pDescription, bytes);
      result.last_message[bytes] = 0;
    }
  }
  return result;
}
Result<void> WindowsViewportRemovalProbe::set_next_fence(WindowsViewport &owner, std::uint64_t value) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_)
    return fail(ErrorCode::FailedPrerequisite, "counter probe requires a live test-owned device");
  auto &p = *owner.impl_;
  if (value < p.next_fence)
    return fail(ErrorCode::InvalidInput, "counter probe cannot reuse an issued identity");
  auto drained = p.drain();
  if (!drained)
    return drained;
  if (value < p.next_fence)
    return fail(ErrorCode::InvalidInput, "counter probe value predates completed drain");
  p.next_fence = value;
  return {};
}
Result<void> WindowsViewportRemovalProbe::add_warning(WindowsViewport &owner) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_ || !owner.impl_->diagnostics)
    return fail(ErrorCode::Unsupported, "native diagnostic queue required");
  const auto result = owner.impl_->diagnostics->AddApplicationMessage(
      D3D12_MESSAGE_SEVERITY_WARNING, "RACE P03.03 diagnostic retention sentinel");
  if (FAILED(result))
    return fail(ErrorCode::PlatformFailure, "native diagnostic sentinel injection failed");
  return {};
}
Result<void> WindowsViewportRemovalProbe::set_epoch(WindowsViewport &owner, std::uint64_t value) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_ || value < owner.epoch_)
    return fail(ErrorCode::InvalidInput, "epoch probe requires a live owner and monotonic identity");
  auto drained = owner.impl_->drain();
  if (!drained)
    return drained;
  owner.impl_->invalidate_image_and_timing();
  owner.epoch_ = value;
  owner.impl_->information.device_epoch = value;
  return {};
}
Result<void> WindowsViewportRemovalProbe::fail_next_initialization(WindowsViewport &owner) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  owner.fail_next_initialization_ = true;
  return {};
}
Result<void> WindowsViewportRemovalProbe::reject_next_graph(WindowsViewport &owner, bool binding) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_)
    return fail(ErrorCode::FailedPrerequisite, "graph probe requires a live owner");
  owner.impl_->graph_fault =
      binding ? WindowsViewport::Impl::GraphFault::Binding : WindowsViewport::Impl::GraphFault::Description;
  return {};
}
Result<void> WindowsViewportRemovalProbe::collect_diagnostics(WindowsViewport &owner) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_)
    return fail(ErrorCode::FailedPrerequisite, "native diagnostics probe requires a live device");
  owner.impl_->read_diagnostics();
  return {};
}
Result<void> WindowsViewportRemovalProbe::limit_output_budget(WindowsViewport &owner, std::uint64_t bytes) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_)
    return fail(ErrorCode::FailedPrerequisite, "output ceiling probe requires a live owner");
  owner.impl_->output_budget_override = bytes;
  return {};
}
Result<void> WindowsViewportRemovalProbe::remove_test_device(WindowsViewport &owner) {
  auto own = owner.owner_check();
  if (!own)
    return own;
  if (!owner.impl_ || !owner.impl_->device || !owner.impl_->fence)
    return fail(ErrorCode::FailedPrerequisite, "native removal probe requires its live test-owned device");
  auto &p = *owner.impl_;
  if (FAILED(p.device->GetDeviceRemovedReason()))
    return fail(ErrorCode::FailedPrerequisite, "native removal probe device was already removed");
  ComPtr<ID3D12Device5> device5;
  const auto queried = p.device.As(&device5);
  if (queried == E_NOINTERFACE)
    return fail(ErrorCode::Unsupported, "native RemoveDevice interface unavailable");
  if (FAILED(queried))
    return fail(ErrorCode::PlatformFailure, "native removal interface query failed");
  device5->RemoveDevice(); // Actual runtime operation. Never write RACE loss flags here.
  device5.Reset();         // Release the extra COM reference before production recovery can run.
  return {};
}
#endif

WindowsViewport::WindowsViewport(std::unique_ptr<const Program> program)
    : program_(std::move(program)), image_owner_(std::make_shared<ImageOwner>()) {}
WindowsViewport::~WindowsViewport() = default;
Result<void> WindowsViewport::owner_check() const {
  if (owner_ != std::this_thread::get_id())
    return fail(ErrorCode::WrongThread, "viewport belongs to creating thread");
  return {};
}
Result<std::unique_ptr<const WindowsViewport::Program>>
WindowsViewport::load_program(const std::filesystem::path &directory) {
  auto manifest = shader_manifest<viewport_shader_manifest_bytes>(directory / "viewport-bindings.rmanifest");
  if (!manifest)
    return manifest.error();
  auto vs = shader(directory / "viewport.vs.dxil"), ps = shader(directory / "viewport.ps.dxil");
  if (!vs)
    return vs.error();
  if (!ps)
    return ps.error();
  auto admitted =
      admit_viewport_program(manifest.value(), std::as_bytes(std::span(vs.value())),
                             std::as_bytes(std::span(ps.value())), compiled_viewport_shader_identity);
  if (!admitted)
    return admitted.error();
  // The P03.04 programs are admitted after, and linked by digest to, the legacy vertex stage.
  auto light_manifest =
      shader_manifest<light_output_shader_manifest_bytes>(directory / "viewport-light-output.rmanifest");
  if (!light_manifest)
    return light_manifest.error();
  auto physical = shader(directory / "viewport-physical.ps.dxil"),
       output_vs = shader(directory / "viewport-output.vs.dxil"),
       output_ps = shader(directory / "viewport-output.ps.dxil");
  if (!physical)
    return physical.error();
  if (!output_vs)
    return output_vs.error();
  if (!output_ps)
    return output_ps.error();
  auto light = admit_light_output_programs(
      light_manifest.value(), std::as_bytes(std::span(physical.value())),
      std::as_bytes(std::span(output_vs.value())), std::as_bytes(std::span(output_ps.value())),
      compiled_light_output_shader_identity, admitted.value().vertex_digest());
  if (!light)
    return light.error();
  return std::unique_ptr<const Program>(new Program(
      std::move(vs).value(), std::move(ps).value(), std::move(admitted).value(), std::move(physical).value(),
      std::move(output_vs).value(), std::move(output_ps).value(), std::move(light).value()));
}
Result<std::unique_ptr<WindowsViewport>> WindowsViewport::create(HWND window,
                                                                 const std::filesystem::path &directory) {
  if (!IsWindow(window))
    return fail(ErrorCode::InvalidInput, "viewport requires a live native window");
  try {
    auto program = load_program(directory);
    if (!program)
      return program.error();
    auto owner = std::unique_ptr<WindowsViewport>(new WindowsViewport(std::move(program).value()));
    auto initialized = owner->replace(window, ViewportRecreation::Initial);
    if (!initialized)
      return initialized.error();
    return owner;
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "viewport initialization allocation failed");
  } catch (const std::filesystem::filesystem_error &) {
    return fail(ErrorCode::IoFailure, "viewport shader path failed");
  }
}
void WindowsViewport::retain_diagnostics(ViewportStats &source) noexcept {
  const auto add = [&](std::uint64_t &destination, std::uint64_t value) {
    if (value > UINT64_MAX - destination) {
      destination = UINT64_MAX;
      retained_diagnostics_incomplete_ = true;
    } else
      destination += value;
  };
  add(retained_debug_errors_, source.debug_errors);
  add(retained_debug_warnings_, source.debug_warnings);
  add(retained_debug_discarded_, source.debug_messages_discarded);
  retained_diagnostics_incomplete_ = retained_diagnostics_incomplete_ || source.diagnostics_incomplete;
  if (source.last_debug_message[0])
    retained_debug_message_ = source.last_debug_message;
  source.debug_errors = source.debug_warnings = source.debug_messages_discarded = 0;
  source.diagnostics_incomplete = false;
}
Result<void> WindowsViewport::retire_native() {
  if (!impl_)
    return {};
  auto &p = *impl_;
  // drain() returns only after fences establish live-device quiescence, or after
  // confirmed native removal. Failed waits never grant a timeout-to-free path.
  auto drained = (p.queue && p.fence) ? p.drain() : Result<void>{};
  p.read_diagnostics();
  retired_ = {p.information.device_epoch,          p.information.submitted_frames,
              p.information.completed_frames,      p.information.last_native_error,
              p.information.device_removal_reason, p.information.device_lost,
              p.information.removal_from_fence};
  retired_.debug_errors = p.information.debug_errors;
  retired_.debug_warnings = p.information.debug_warnings;
  retired_.debug_messages_discarded = p.information.debug_messages_discarded;
  retired_.diagnostics_incomplete = p.information.diagnostics_incomplete;
  retired_.last_debug_message = p.information.last_debug_message;
  retain_diagnostics(p.information);
  p.invalidate_image_and_timing();
  unavailable_ = std::move(p.information);
  p.retired = true;
  impl_.reset();
  if (!drained)
    lifecycle_error_ = drained.error();
  return drained;
}
Result<void> WindowsViewport::retire_surface() {
  auto own = owner_check();
  if (!own)
    return own;
  if (!impl_)
    return {};
  auto retired = retire_native();
  state_ = retired_.removed ? ViewportState::Removed : ViewportState::SurfaceRetired;
  return retired;
}
Result<void> WindowsViewport::replace(HWND window, ViewportRecreation cause) {
  auto own = owner_check();
  if (!own)
    return own;
  if (!IsWindow(window))
    return fail(ErrorCode::InvalidInput, "viewport recreation requires a live native window");
  RECT client{};
  if (!GetClientRect(window, &client))
    return fail(ErrorCode::PlatformFailure, "viewport recreation client size unavailable");
  if (client.right < client.left || client.bottom < client.top ||
      client.right - client.left > static_cast<LONG>(viewport_max_dimension) ||
      client.bottom - client.top > static_cast<LONG>(viewport_max_dimension))
    return fail(ErrorCode::Exhausted, "viewport recreation size exceeds 4096 bounds");
  if (epoch_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "viewport device epoch exhausted");
  if (cause == ViewportRecreation::DeviceRemoval)
    ++removal_recovery_attempts_;
  try {
    // Reserve only the empty CPU wrapper before retiring. Never create two live swapchains/caches.
    auto candidate = std::make_unique<Impl>();
    auto retired = retire_native();
    if (!retired && cause != ViewportRecreation::DeviceRemoval) {
      state_ = retired_.removed ? ViewportState::Removed : ViewportState::Unavailable;
      return retired;
    }
    ++epoch_;
    if (cause != ViewportRecreation::Initial)
      ++recreation_attempts_;
    recreation_ = cause;
    state_ = ViewportState::Unavailable;
    lifecycle_error_.reset();
    auto initialized = candidate->initialize(window, *program_, epoch_);
#ifdef RACE_VIEWPORT_NATIVE_REMOVAL_PROBE
    // Exercise failed replacement with real candidate resources but no submitted work.
    // This is an explicitly injected Result, never a native error/removal claim.
    if (fail_next_initialization_) {
      fail_next_initialization_ = false;
      if (initialized)
        initialized =
            fail(ErrorCode::Exhausted, "test-injected initialization failure after native resource creation");
    }
#endif
    if (!initialized) {
      lifecycle_error_ = initialized.error();
      candidate->read_diagnostics();
      retain_diagnostics(candidate->information);
      unavailable_ = std::move(candidate->information);
      return initialized;
    }
    impl_ = std::move(candidate);
    state_ = ViewportState::Ready;
    return {};
  } catch (const std::bad_alloc &) {
    const auto error = fail(ErrorCode::Exhausted, "viewport recreation allocation failed");
    lifecycle_error_ = error;
    if (!impl_)
      state_ = ViewportState::Unavailable;
    return error;
  }
}
Result<void> WindowsViewport::recreate(HWND window) {
  auto own = owner_check();
  if (!own)
    return own;
  if (!IsWindow(window))
    return fail(ErrorCode::InvalidInput, "viewport recreation requires a live native window");
  if ((impl_ && impl_->removed()) || state_ == ViewportState::Removed || (!impl_ && retired_.removed))
    return fail(ErrorCode::PlatformFailure, "confirmed device removal requires the bounded recovery path");
  return replace(window, state_ == ViewportState::SurfaceRetired ? ViewportRecreation::SurfaceRestore
                                                                 : ViewportRecreation::Controlled);
}
Result<void> WindowsViewport::recover_removed(HWND window) {
  auto own = owner_check();
  if (!own)
    return own;
  if (!IsWindow(window))
    return fail(ErrorCode::InvalidInput, "viewport recovery requires a live native window");
  if (!(impl_ ? impl_->removed() : state_ == ViewportState::Removed))
    return fail(ErrorCode::FailedPrerequisite, "viewport has no confirmed native removal");
  if (removal_recovery_attempts_)
    return fail(ErrorCode::FailedPrerequisite, "viewport automatic removal recovery already attempted");
  if (epoch_ == UINT64_MAX)
    return fail(ErrorCode::Overflow, "viewport device epoch exhausted");
  return replace(window, ViewportRecreation::DeviceRemoval);
}
Result<void> WindowsViewport::resize(std::uint32_t width, std::uint32_t height) {
  auto own = owner_check();
  if (!own)
    return own;
  if (!impl_)
    return fail(ErrorCode::FailedPrerequisite, "viewport has no native surface");
  auto &p = *impl_;
  if (width > viewport_max_dimension || height > viewport_max_dimension)
    return fail(ErrorCode::Exhausted, "viewport size exceeds 4096 bounds");
  if (p.removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 viewport device removed");
  if (!width || !height) {
    p.information.minimized = true;
    p.has_image = false;
    return {};
  }
  p.information.minimized = false;
  if (width == p.information.width && height == p.information.height && p.scene_color && p.depth &&
      p.targets[0] && p.targets[1])
    return {};
  auto drained = p.drain();
  if (!drained)
    return drained;
  p.has_image = false;
  for (auto &target : p.targets)
    target.Reset();
  p.scene_color.Reset();
  p.depth.Reset();
  p.capture_readback.Reset();
  p.hdr_color.Reset();
  p.hdr_readback.Reset();
  const auto result = p.swapchain->ResizeBuffers(frame_count, width, height, color_format, 0);
  if (FAILED(result))
    return p.native_error(result, "viewport swapchain resize failed");
  auto targets = p.make_targets(width, height);
  p.read_diagnostics();
  return targets;
}

ViewportState WindowsViewport::state() const noexcept {
  return impl_ && impl_->information.device_lost ? ViewportState::Removed : state_;
}
ViewportStats WindowsViewport::stats() const {
  auto result = impl_ ? impl_->information : unavailable_;
  result.device_epoch = epoch_;
  result.state = state();
  result.recreation = recreation_;
  result.recreation_attempts = recreation_attempts_;
  result.removal_recovery_attempts = removal_recovery_attempts_;
  result.retired = retired_;
  result.lifecycle_error = lifecycle_error_;
  result.has_image =
      impl_ && impl_->has_image && !impl_->information.minimized && !impl_->information.device_lost;
  const auto add = [&](std::uint64_t value, std::uint64_t retained) {
    if (retained > UINT64_MAX - value) {
      result.diagnostics_incomplete = true;
      return UINT64_MAX;
    }
    return value + retained;
  };
  result.debug_errors = add(result.debug_errors, retained_debug_errors_);
  result.debug_warnings = add(result.debug_warnings, retained_debug_warnings_);
  result.debug_messages_discarded = add(result.debug_messages_discarded, retained_debug_discarded_);
  result.diagnostics_incomplete = result.diagnostics_incomplete || retained_diagnostics_incomplete_;
  if (!result.last_debug_message[0])
    result.last_debug_message = retained_debug_message_;
  return result;
}

} // namespace race::render
