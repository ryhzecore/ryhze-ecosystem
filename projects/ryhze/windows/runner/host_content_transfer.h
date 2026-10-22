#ifndef RUNNER_HOST_CONTENT_TRANSFER_H_
#define RUNNER_HOST_CONTENT_TRANSFER_H_
#include <windows.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "content_transfer.h"

class HostContentTransfer {
 public:
  static constexpr UINT kMessage = WM_APP + 0x52d;
  explicit HostContentTransfer(std::optional<std::wstring> root = {});
  ~HostContentTransfer();
  void Attach(flutter::BinaryMessenger* messenger, HWND window);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
#if defined(RYHZE_TRANSFER_NATIVE_TEST)
  // Test target only: hold one real worker before its bounded native operation.
  // The fixture owns both events until the held worker has actually settled.
  static void HoldNextWorkerForTesting(HANDLE entered, HANDLE release);
#endif
 private:
  struct WorkerState;
  using Result = flutter::MethodResult<flutter::EncodableValue>;
  std::optional<std::wstring> root_;
  std::string device_id_;
  HWND window_ = nullptr;
  HANDLE worker_ = nullptr;
  ULONGLONG deadline_ = 0;
  bool closing_ = false;
  std::shared_ptr<ryhze::ContentTransfer> transfer_;
  std::shared_ptr<WorkerState> state_;
  std::unique_ptr<Result> pending_;
  std::vector<std::unique_ptr<Result>> drains_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  void Cancel(const char* error);
  void Poll();
  bool Start(std::shared_ptr<WorkerState> next);
  void StartDrain();
  static unsigned __stdcall Run(void* context);
};
#endif
