#ifndef RUNNER_HOST_UPDATES_H_
#define RUNNER_HOST_UPDATES_H_
#include <windows.h>
#include <flutter/method_channel.h>
#include <flutter/encodable_value.h>
#include <memory>
#include <optional>
#include <vector>
#include "update_stager.h"

class HostUpdates {
 public:
  static constexpr UINT kMessage = WM_APP + 0x528;
  // Trust is supplied by the composition/build owner, never the feed or channel.
  explicit HostUpdates(std::wstring private_root = L"",
      std::optional<ryhze::UpdateDigest> publisher = std::nullopt);
  ~HostUpdates();
  void Attach(flutter::BinaryMessenger* messenger, HWND window = nullptr);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
#ifdef RYHZE_UPDATES_NATIVE_TEST
  // Compile-only worker gate. Tests own these handles until drain completes.
  void SetWorkerGateForTesting(HANDLE entered, HANDLE release);
  void ExpireForTesting();
#endif
 private:
  struct WorkerState;
  using Result = flutter::MethodResult<flutter::EncodableValue>;
  const std::wstring private_root_;
  const std::optional<ryhze::UpdateDigest> publisher_;
  const bool configured_;
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
  bool ArmTimer();
#ifdef RYHZE_UPDATES_NATIVE_TEST
  HANDLE test_entered_ = nullptr;
  HANDLE test_release_ = nullptr;
#endif
  static unsigned __stdcall Run(void* context);
};
#endif
