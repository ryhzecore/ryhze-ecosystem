#ifndef RUNNER_HOST_CONTENT_STAGING_H_
#define RUNNER_HOST_CONTENT_STAGING_H_
#include <windows.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "content_stager.h"

class HostContentStaging {
 public:
  static constexpr UINT kMessage = WM_APP + 0x52b;
  explicit HostContentStaging(std::optional<std::wstring> private_root = std::nullopt);
  ~HostContentStaging();
  void Attach(flutter::BinaryMessenger* messenger, HWND window);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
 private:
  struct WorkerState;
  using Result = flutter::MethodResult<flutter::EncodableValue>;
  std::optional<std::wstring> root_;
  std::string device_id_;
  HWND window_ = nullptr;
  HANDLE worker_ = nullptr;
  ULONGLONG deadline_ = 0;
  bool closing_ = false;
  std::shared_ptr<WorkerState> state_;
  std::unique_ptr<Result> pending_;
  std::vector<std::unique_ptr<Result>> drains_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  void Cancel(const char* error);
  void Poll();
  static unsigned __stdcall Run(void* context);
};
#endif