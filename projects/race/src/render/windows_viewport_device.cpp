#include "windows_viewport_internal.hpp"

namespace race::render {
namespace {
// The only process-wide state here configures the D3D12 SDK before the first
// WindowsViewport device. Thread-safe static initialization never re-enables it
// while another viewport/device is alive. This is not a device/service registry.
struct DebugConfiguration {
  HRESULT interface_result{};
  bool configured{};
};
const DebugConfiguration &debug_configuration() {
  static const DebugConfiguration configured = [] {
    ComPtr<ID3D12Debug> controller;
    const auto result = D3D12GetDebugInterface(IID_PPV_ARGS(&controller));
    if (FAILED(result))
      return DebugConfiguration{result, false};
    controller->EnableDebugLayer();
    return DebugConfiguration{result, true};
  }();
  return configured;
}
std::string utf8(const wchar_t *text) {
  const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (count <= 1)
    return "Unknown D3D12 adapter";
  std::string result(static_cast<std::size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), count, nullptr, nullptr);
  result.pop_back();
  return result;
}
} // namespace
WindowsViewport::Impl::~Impl() {
  // All resources below remain owned while the GPU can still reference them.
  // A timeout never causes release. Device removal is a terminal queue state.
  if (!retired && queue && fence) {
    auto result = drain();
    (void)result;
  }
  if (fence_event)
    CloseHandle(fence_event);
}

void WindowsViewport::Impl::invalidate_image_and_timing() noexcept {
  has_image = false;
  information.gpu_samples = {};
  information.gpu_sample_count = 0;
  information.last_timed_frame = 0;
  information.gpu_milliseconds = 0;
  for (auto &slot : slots)
    slot.timing = false;
}

bool WindowsViewport::Impl::observe_removal(bool fence_removal) {
  if (information.device_lost)
    return true;
  const auto reason = device ? device->GetDeviceRemovedReason() : S_OK;
  if (FAILED(reason) || fence_removal) {
    information.device_lost = true;
    information.device_removal_reason = static_cast<std::int64_t>(reason);
    information.removal_from_fence = fence_removal;
    invalidate_image_and_timing();
    return true;
  }
  return false;
}

Error WindowsViewport::Impl::native_error(HRESULT result, const char *message) {
  information.last_native_error = static_cast<std::int64_t>(result);
  (void)observe_removal();
  return fail(ErrorCode::PlatformFailure, message);
}

bool WindowsViewport::Impl::removed() { return observe_removal(); }

void WindowsViewport::Impl::read_diagnostics() {
  if (!diagnostics)
    return;
  const auto count = diagnostics->GetNumStoredMessagesAllowedByRetrievalFilter();
  const auto discarded = diagnostics->GetNumMessagesDiscardedByMessageCountLimit();
  if (count < diagnostic_cursor || discarded < diagnostic_discarded) {
    information.diagnostics_incomplete = true;
    ++information.debug_warnings;
    diagnostic_cursor = 0;
  }
  if (discarded > diagnostic_discarded) {
    information.debug_messages_discarded += discarded - diagnostic_discarded;
    information.diagnostics_incomplete = true;
    ++information.debug_warnings;
  }
  diagnostic_discarded = discarded;
  for (UINT64 i = diagnostic_cursor; i < count && i < 256; ++i) {
    alignas(D3D12_MESSAGE) std::array<std::byte, 4096> storage{};
    SIZE_T size = storage.size();
    auto *message = reinterpret_cast<D3D12_MESSAGE *>(storage.data());
    if (SUCCEEDED(diagnostics->GetMessage(i, message, &size))) {
      if (message->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
          message->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION)
        ++information.debug_errors;
      else if (message->Severity == D3D12_MESSAGE_SEVERITY_WARNING)
        ++information.debug_warnings;
      const auto message_bytes =
          std::min(message->DescriptionByteLength, information.last_debug_message.size() - 1);
      std::memcpy(information.last_debug_message.data(), message->pDescription, message_bytes);
      information.last_debug_message[message_bytes] = 0;
      OutputDebugStringA(message->pDescription);
      OutputDebugStringA("\n");
    } else {
      ++information.debug_warnings;
      information.diagnostics_incomplete = true;
    }
  }
  if (count > 256) {
    ++information.debug_warnings;
    information.diagnostics_incomplete = true;
  }
  diagnostic_cursor = count;
  // Do not clear a shared device's queue: every viewport observes it independently.
  // Retention remains bounded by the SDK's 256-message limit; overflow is explicit.
}

void WindowsViewport::Impl::record_timing(std::uint64_t frame, double milliseconds) {
  auto &samples = information.gpu_samples;
  auto &count = information.gpu_sample_count;
  if (count < samples.size())
    samples[count++] = {frame, milliseconds, information.device_epoch};
  else if (frame > samples[0].frame)
    samples[0] = {frame, milliseconds, information.device_epoch};
  // Slot completion may be observed in slot order rather than frame order.
  // Retain the newest four identities without associating an old pair with a
  // newer submission, and do not let a late older pair regress latest timing.
  std::sort(samples.begin(), samples.begin() + count,
            [](const auto &a, const auto &b) { return a.frame < b.frame; });
  if (frame > information.last_timed_frame) {
    information.last_timed_frame = frame;
    information.gpu_milliseconds = milliseconds;
  }
}

void WindowsViewport::Impl::collect(Slot &slot) {
  const auto completed = fence->GetCompletedValue();
  if (completed == UINT64_MAX) {
    (void)observe_removal(true);
    return;
  }
  if (!slot.frame || completed < slot.fence)
    return;
  information.completed_frames = std::max(information.completed_frames, slot.frame);
  if (slot.timing && timestamp_readback) {
    const auto index = static_cast<std::size_t>(&slot - slots.data());
    D3D12_RANGE range{index * 16, index * 16 + 16};
    void *mapped{};
    if (SUCCEEDED(timestamp_readback->Map(0, &range, &mapped))) {
      const auto *values = static_cast<const UINT64 *>(mapped) + index * 2;
      if (values[1] >= values[0] && timestamp_frequency)
        record_timing(slot.frame, static_cast<double>(values[1] - values[0]) * 1000 /
                                      static_cast<double>(timestamp_frequency));
      const D3D12_RANGE written{0, 0};
      timestamp_readback->Unmap(0, &written);
    } else
      information.gpu_timing_available = false;
    slot.timing = false;
  }
}

Result<void> WindowsViewport::Impl::wait(UINT64 value, WaitMetrics *metrics) {
  if (!value)
    return {};
  if (removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 device removed; viewport closed");
  const auto started = metrics ? ProfileClock::now() : ProfileClock::time_point{};
  if (metrics)
    metrics->pending = fence->GetCompletedValue() < value;
  const auto event_result = fence->SetEventOnCompletion(value, fence_event);
  // If event registration fails while the device is alive, poll the fence;
  // failure to wait never authorizes releasing resources still in flight.
  while (fence->GetCompletedValue() < value) {
    if (removed())
      return fail(ErrorCode::PlatformFailure, "D3D12 device removed while waiting");
    if (SUCCEEDED(event_result))
      WaitForSingleObject(fence_event, 50);
    else
      Sleep(1);
  }
  if (metrics && metrics->pending)
    metrics->pending_ms = elapsed_ms(started, ProfileClock::now());
  if (fence->GetCompletedValue() == UINT64_MAX || removed()) {
    (void)observe_removal(fence->GetCompletedValue() == UINT64_MAX);
    return fail(ErrorCode::PlatformFailure, "D3D12 completion reports device removal");
  }
  for (auto &slot : slots)
    collect(slot);
  return {};
}

Result<void> WindowsViewport::Impl::require_signal_capacity(UINT64 count) const {
  // UINT64_MAX is the native removal sentinel, never a signal identity.
  // Owner-thread serialization keeps admitted capacity available until Execute.
  if (count > UINT64_MAX - next_fence)
    return fail(ErrorCode::Overflow, "viewport fence identity exhausted before submission");
  return {};
}

Result<UINT64> WindowsViewport::Impl::signal() {
  if (next_fence == UINT64_MAX)
    return fail(ErrorCode::Overflow, "viewport fence identity exhausted");
  const auto value = next_fence;
  const auto result = queue->Signal(fence.Get(), value);
  if (FAILED(result))
    return native_error(result, "D3D12 queue signal failed");
  ++next_fence;
  submitted_without_fence = false;
  return value;
}

Result<void> WindowsViewport::Impl::drain() {
  if (removed())
    return fail(ErrorCode::PlatformFailure, "D3D12 device removed");
  // Retry a failed signal only while previously submitted work remains live.
  // This is a quiescence contract, not a finite-time shutdown promise.
  // Once no further signal identity exists, already-issued slot fences still
  // prove retirement. New Execute calls cannot reach this state unfenced.
  if (next_fence == UINT64_MAX && !submitted_without_fence) {
    for (auto &slot : slots) {
      auto done = wait(slot.fence);
      if (!done)
        return done;
    }
    return {};
  }
  auto value = signal();
  while (!value && submitted_without_fence && !removed()) {
    Sleep(1);
    value = signal();
  }
  if (!value) {
    // Existing per-frame fences still establish safe retirement on a signal
    // failure that occurred without any new unfenced command submission.
    for (auto &slot : slots) {
      auto done = wait(slot.fence);
      if (!done)
        return done;
    }
    return value.error();
  }
  return wait(value.value());
}

Result<void> WindowsViewport::Impl::initialize_device(bool debug_configured) {
  auto result = CreateDXGIFactory2(debug_configured ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory));
  if (FAILED(result))
    result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  if (FAILED(result))
    return native_error(result, "DXGI factory creation failed");
  for (UINT i = 0;; ++i) {
    ComPtr<IDXGIAdapter1> adapter;
    const auto enumerated =
        factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    if (enumerated == DXGI_ERROR_NOT_FOUND)
      break;
    if (FAILED(enumerated))
      return native_error(enumerated, "DXGI adapter enumeration failed");
    DXGI_ADAPTER_DESC1 desc{};
    if (FAILED(adapter->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
      continue;
    ComPtr<ID3D12Device> candidate;
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&candidate))))
      continue;
    D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_0};
    if (FAILED(candidate->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model, sizeof(model))) ||
        model.HighestShaderModel < D3D_SHADER_MODEL_6_0)
      continue;
    device = std::move(candidate);
    information.adapter = utf8(desc.Description);
    information.vendor_id = desc.VendorId;
    information.device_id = desc.DeviceId;
    information.dedicated_video_bytes = desc.DedicatedVideoMemory;
    information.hardware = true;
    break;
  }
  if (!device)
    return fail(ErrorCode::Unsupported, "hardware D3D12 shader-model-6 raster device required");
  for (const auto format : {texture_format, scalar_texture_format}) {
    D3D12_FEATURE_DATA_FORMAT_SUPPORT texture_support{format};
    if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &texture_support,
                                           sizeof(texture_support))) ||
        !(texture_support.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) ||
        !(texture_support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE))
      return fail(ErrorCode::Unsupported, "viewport requires sampled RGBA8 sRGB and linear textures");
  }
  if (debug_configured) {
    result = device.As(&diagnostics);
    if (FAILED(result))
      return native_error(result, "configured D3D12 debug validation unavailable on device");
    information.debug_layer = true; // Device-local evidence, not merely a request to enable it.
    result = diagnostics->SetMessageCountLimit(256);
    if (FAILED(result))
      return native_error(result, "D3D12 diagnostic storage bound failed");
    D3D12_MESSAGE_SEVERITY denied[]{D3D12_MESSAGE_SEVERITY_INFO, D3D12_MESSAGE_SEVERITY_MESSAGE};
    D3D12_INFO_QUEUE_FILTER filter{};
    filter.DenyList.NumSeverities = 2;
    filter.DenyList.pSeverityList = denied;
    result = diagnostics->AddStorageFilterEntries(&filter);
    if (FAILED(result))
      return native_error(result, "D3D12 diagnostic storage filter failed");
  }

  return {};
}
Result<void> WindowsViewport::Impl::initialize_queue() {
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  auto result = device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  if (FAILED(result))
    return native_error(result, "D3D12 direct queue creation failed");
  result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  if (FAILED(result))
    return native_error(result, "D3D12 fence creation failed");
  fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (!fence_event)
    return fail(ErrorCode::PlatformFailure, "viewport fence event creation failed");

  return {};
}
Result<void> WindowsViewport::Impl::initialize_surface(HWND hwnd, UINT width, UINT height,
                                                       ComPtr<IDXGISwapChain1> &created) {
  DXGI_SWAP_CHAIN_DESC1 swap{};
  swap.Width = width;
  swap.Height = height;
  swap.Format = color_format;
  swap.SampleDesc.Count = 1;
  swap.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  swap.BufferCount = frame_count;
  swap.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  swap.Scaling = DXGI_SCALING_STRETCH;
  swap.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
  auto result = factory->CreateSwapChainForHwnd(queue.Get(), hwnd, &swap, nullptr, nullptr, &created);
  if (FAILED(result))
    return native_error(result, "native D3D12 viewport swapchain creation failed");
  result = created.As(&swapchain);
  if (FAILED(result))
    return native_error(result, "swapchain interface unavailable");
  factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
  D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
  descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  descriptors.NumDescriptors = frame_count + 2; // Two swapchain images, display image, FP16 target.
  result = device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&rtv_heap));
  if (FAILED(result))
    return native_error(result, "viewport RTV heap creation failed");
  descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  descriptors.NumDescriptors = 1;
  result = device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&dsv_heap));
  if (FAILED(result))
    return native_error(result, "viewport depth descriptor creation failed");
  descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  descriptors.NumDescriptors = frame_count * texture_descriptors + 1; // Plus the scene-linear source.
  descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  result = device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&srv_heap));
  if (FAILED(result))
    return native_error(result, "viewport texture descriptor creation failed");
  srv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  for (UINT i = 0; i < frame_count; ++i)
    texture_view(nullptr, i, 0);
  rtv_increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  auto target_result = make_targets(width, height);
  if (!target_result)
    return target_result;

  return {};
}
Result<void> WindowsViewport::Impl::initialize_commands() {
  for (auto &slot : slots) {
    const auto result =
        device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
    if (FAILED(result))
      return native_error(result, "viewport command allocator creation failed");
  }
  auto result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots[0].allocator.Get(),
                                          nullptr, IID_PPV_ARGS(&commands));
  if (FAILED(result))
    return native_error(result, "viewport command list creation failed");
  result = commands->Close();
  if (FAILED(result))
    return native_error(result, "initial viewport command list close failed");

  return {};
}
Result<void> WindowsViewport::Impl::initialize_geometry(std::span<const Vertex> data) {
  const auto bytes = data.size() * sizeof(Vertex);
  const auto upload = heap(D3D12_HEAP_TYPE_UPLOAD);
  const auto desc = buffer(bytes);
  auto result =
      device->CreateCommittedResource(&upload, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                      nullptr, IID_PPV_ARGS(&vertices));
  if (FAILED(result))
    return native_error(result, "viewport geometry allocation failed");
  const D3D12_RANGE no_read{0, 0};
  void *mapped{};
  result = vertices->Map(0, &no_read, &mapped);
  if (FAILED(result))
    return native_error(result, "viewport geometry upload failed");
  std::memcpy(mapped, data.data(), bytes);
  const D3D12_RANGE written{0, bytes};
  vertices->Unmap(0, &written);
  vertex_view = {vertices->GetGPUVirtualAddress(), static_cast<UINT>(bytes), sizeof(Vertex)};

  return {};
}
void WindowsViewport::Impl::initialize_timestamps() {
  if (SUCCEEDED(queue->GetTimestampFrequency(&timestamp_frequency)) && timestamp_frequency) {
    D3D12_QUERY_HEAP_DESC query{};
    query.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query.Count = frame_count * 2;
    const auto readback = heap(D3D12_HEAP_TYPE_READBACK);
    const auto timing = buffer(frame_count * 16);
    if (SUCCEEDED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&timestamp_heap))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &timing,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&timestamp_readback))))
      information.gpu_timing_available = true;
  }
}
Result<void> WindowsViewport::Impl::initialize(HWND hwnd, const Program &retained, std::uint64_t epoch) {
  window = hwnd;
  information.device_epoch = epoch;
  if (!IsWindow(hwnd))
    return fail(ErrorCode::InvalidInput, "viewport requires a live native window");
  // Initial admission retained exact immutable bytes outside this per-device Impl.
  // Recreation rechecks native capabilities but never rereads a shader directory.
  const auto bindings = retained.admitted.bindings();
  RECT client{};
  if (!GetClientRect(hwnd, &client))
    return fail(ErrorCode::PlatformFailure, "viewport client size unavailable");
  const auto w = client.right - client.left, h = client.bottom - client.top;
  if (w < 0 || h < 0 || w > static_cast<LONG>(viewport_max_dimension) ||
      h > static_cast<LONG>(viewport_max_dimension))
    return fail(ErrorCode::Exhausted, "viewport initial size exceeds 4096 bounds");
  const UINT width = static_cast<UINT>(std::max<LONG>(1, w)),
             height = static_cast<UINT>(std::max<LONG>(1, h));
  information.minimized = !w || !h;
  const auto &debug = debug_configuration();
  information.debug_interface_result = static_cast<std::int64_t>(debug.interface_result);
  information.debug_layer_configured = debug.configured;
  auto admitted_device = initialize_device(debug.configured);
  if (!admitted_device)
    return admitted_device;
  auto admitted_queue = initialize_queue();
  if (!admitted_queue)
    return admitted_queue;
  ComPtr<IDXGISwapChain1> created;
  auto surface = initialize_surface(hwnd, width, height, created);
  if (!surface)
    return surface;
  auto command_storage = initialize_commands();
  if (!command_storage)
    return command_storage;
  ComPtr<ID3DBlob> serialized, errors;
  auto root_signature = initialize_root(bindings, serialized, errors);
  if (!root_signature)
    return root_signature;
  D3D12_INPUT_ELEMENT_DESC layout[]{
      {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
      {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}};
  D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};
  auto pipelines = initialize_pipelines(retained, pipeline, layout);
  if (!pipelines)
    return pipelines;
  ComPtr<ID3DBlob> output_serialized, output_errors;
  auto output_signature = initialize_output_root(retained, output_serialized, output_errors);
  if (!output_signature)
    return output_signature;
  auto output_pipeline = initialize_output_pipeline(retained, pipeline);
  if (!output_pipeline)
    return output_pipeline;
  program = retained.admitted;
  const auto data = geometry();
  auto uploaded = initialize_geometry(data);
  if (!uploaded)
    return uploaded;
  initialize_timestamps();
  prepared.reserve(viewport_max_draws);
  read_diagnostics();
  return {};
}

} // namespace race::render
