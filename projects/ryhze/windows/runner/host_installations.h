#ifndef RUNNER_HOST_INSTALLATIONS_H_
#define RUNNER_HOST_INSTALLATIONS_H_
#include <windows.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <memory>
#include <optional>
#include <string>

class HostInstallations {
 public:
  explicit HostInstallations(std::optional<std::wstring> fixture_root = std::nullopt);
  ~HostInstallations();
  void Attach(flutter::BinaryMessenger* messenger, HWND window);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
 private:
  struct WorkerState;
  std::optional<std::wstring> root_;
  HWND window_ = nullptr;
  HANDLE worker_ = nullptr;
  ULONGLONG deadline_ = 0;
  std::shared_ptr<WorkerState> state_;
  std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> pending_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  void Cancel();
  static unsigned __stdcall Run(void* context);
};
#endif
