#include "host_taskbar.h"
#include <shobjidl.h>
#include <wrl/client.h>
#include <flutter/standard_method_codec.h>
#include <utility>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;

class WindowsTaskbar final : public TaskbarBackend {
 public:
  WindowsTaskbar() {
    // COM object activation is a probe. No ITaskbarList3 method is invoked
    // before Windows delivers TaskbarButtonCreated for this HWND.
    available_ = SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_)));
  }
  bool Available() const override { return available_; }
  bool Ready() override {
    // Explorer can recreate the button and its COM endpoint independently.
    taskbar_.Reset();
    available_ = SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&taskbar_)));
    return available_ && SUCCEEDED(taskbar_->HrInit());
  }
  bool Apply(HWND window, TaskbarStatus status) override {
    if (!taskbar_) return false;
    const bool attention = status == TaskbarStatus::attention;
    HICON warning = attention ? LoadIconW(nullptr, MAKEINTRESOURCEW(32515)) : nullptr;
    if (attention && !warning) return false;
    const HRESULT icon = taskbar_->SetOverlayIcon(window,
        warning,
        attention ? L"Ryhze activity requires attention" : L"");
    const HRESULT progress = taskbar_->SetProgressState(window,
        status == TaskbarStatus::indeterminate ? TBPF_INDETERMINATE : TBPF_NOPROGRESS);
    return SUCCEEDED(icon) && SUCCEEDED(progress);
  }
 private:
  Microsoft::WRL::ComPtr<ITaskbarList3> taskbar_;
  bool available_ = false;
};
}  // namespace

HostTaskbar::HostTaskbar(std::unique_ptr<TaskbarBackend> backend)
    : backend_(backend ? std::move(backend) : std::make_unique<WindowsTaskbar>()),
      button_created_(RegisterWindowMessageW(L"TaskbarButtonCreated")) {}
HostTaskbar::~HostTaskbar() { Detach(); }

void HostTaskbar::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach();
  window_ = window;
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.taskbar.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    const auto* args = call.arguments();
    if (call.method_name() == "probe") {
      if (args && !args->IsNull()) { result->Error("invalidData"); return; }
      result->Success(EncodableValue(EncodableMap{
        {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
        {EncodableValue("progress"), EncodableValue(
            window_ && button_created_ && backend_->Available())}}));
      return;
    }
    if (call.method_name() != "set") { result->NotImplemented(); return; }
    const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
    const auto entry = map ? map->find(EncodableValue("state")) : EncodableMap::const_iterator{};
    const auto* value = map && entry != map->end()
        ? std::get_if<std::string>(&entry->second) : nullptr;
    if (!map || map->size() != 1 || !value ||
        (*value != "none" && *value != "indeterminate" && *value != "attention")) {
      result->Error("invalidData"); return;
    }
    desired_ = *value == "indeterminate" ? TaskbarStatus::indeterminate
        : *value == "attention" ? TaskbarStatus::attention : TaskbarStatus::none;
    const char* status = !window_ || !button_created_ || !backend_->Available()
        ? "unavailable" : !ready_ ? (button_seen_ ? "unavailable" : "queued")
        : backend_->Apply(window_, desired_) ? "applied" : "unavailable";
    result->Success(EncodableValue(EncodableMap{
      {EncodableValue("state"), EncodableValue(status)}}));
  });
}

std::optional<LRESULT> HostTaskbar::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)wparam;
  (void)lparam;
  if (!button_created_ || message != button_created_) return std::nullopt;
  if (window_) {
    button_seen_ = true;
    ready_ = backend_->Ready();
    if (ready_) backend_->Apply(window_, desired_);
  }
  return 0;
}

void HostTaskbar::Detach() {
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset();
  if (window_ && ready_) backend_->Apply(window_, TaskbarStatus::none);
  window_ = nullptr;
  ready_ = false;
  button_seen_ = false;
  desired_ = TaskbarStatus::none;
}
