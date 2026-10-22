#include "flutter_window.h"

#include <optional>

#include "flutter/generated_plugin_registrant.h"

FlutterWindow::FlutterWindow(const flutter::DartProject& project, HostActivation& activation)
    : project_(project), host_activation_(activation) {}

FlutterWindow::~FlutterWindow() {}

bool FlutterWindow::OnCreate() {
  if (!Win32Window::OnCreate()) {
    return false;
  }

  RECT frame = GetClientArea();

  // The size here must match the window dimensions to avoid unnecessary surface
  // creation / destruction in the startup path.
  flutter_controller_ = std::make_unique<flutter::FlutterViewController>(
      frame.right - frame.left, frame.bottom - frame.top, project_);
  // Ensure that basic setup of the controller was successful.
  if (!flutter_controller_->engine() || !flutter_controller_->view()) {
    return false;
  }
  RegisterPlugins(flutter_controller_->engine());
  host_lifecycle_.Attach(flutter_controller_->engine()->messenger());
  host_security_.Attach(flutter_controller_->engine()->messenger());
  host_updates_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_taskbar_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_notifications_.Attach(flutter_controller_->engine()->messenger());
  host_installations_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_content_staging_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_content_transfer_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_artwork_cache_.Attach(flutter_controller_->engine()->messenger(), GetHandle());
  host_activation_.Attach(flutter_controller_->engine()->messenger(), GetHandle(), [this]() {
    ::ShowWindow(GetHandle(), ::IsIconic(GetHandle()) ? SW_RESTORE : SW_SHOW);
    ::SetForegroundWindow(GetHandle());
  });
  SetChildContent(flutter_controller_->view()->GetNativeWindow());

  flutter_controller_->engine()->SetNextFrameCallback([&]() {
    this->Show();
  });

  // Flutter can complete the first frame before the "show window" callback is
  // registered. The following call ensures a frame is pending to ensure the
  // window is shown. It is a no-op if the first frame hasn't completed yet.
  flutter_controller_->ForceRedraw();

  return true;
}

void FlutterWindow::OnDestroy() {
  host_activation_.Detach();
  host_security_.Detach();
  host_updates_.Detach();
  host_taskbar_.Detach();
  host_notifications_.Detach();
  host_installations_.Detach();
  host_content_staging_.Detach();
  host_content_transfer_.Detach();
  host_artwork_cache_.Detach();
  host_lifecycle_.Detach();
  if (flutter_controller_) {
    flutter_controller_ = nullptr;
  }

  Win32Window::OnDestroy();
}

LRESULT
FlutterWindow::MessageHandler(HWND hwnd, UINT const message,
                              WPARAM const wparam,
                              LPARAM const lparam) noexcept {
  // Power/session events are immediate native notifications. WM_CLOSE remains
  // entirely with Flutter's existing cancelable exit handshake below.
  if (auto result = host_lifecycle_.HandleMessage(message, wparam, lparam)) {
    return *result;
  }
  if (auto result = host_activation_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_updates_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_taskbar_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_installations_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_content_staging_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_content_transfer_.HandleMessage(message, wparam, lparam)) return *result;
  if (auto result = host_artwork_cache_.HandleMessage(message, wparam, lparam)) return *result;
  // Give Flutter, including plugins, an opportunity to handle window messages.
  if (flutter_controller_) {
    std::optional<LRESULT> result =
        flutter_controller_->HandleTopLevelWindowProc(hwnd, message, wparam,
                                                      lparam);
    if (result) {
      return *result;
    }
  }

  switch (message) {
    case WM_FONTCHANGE:
      flutter_controller_->engine()->ReloadSystemFonts();
      break;
  }

  return Win32Window::MessageHandler(hwnd, message, wparam, lparam);
}
