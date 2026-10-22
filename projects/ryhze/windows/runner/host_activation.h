#ifndef RUNNER_HOST_ACTIVATION_H_
#define RUNNER_HOST_ACTIVATION_H_

#include <windows.h>

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

// Per-executable, per-logon/session navigation transport. No registration,
// privileged helper, credentials, arbitrary commands or provider operations.
class HostActivation {
 public:
  enum class StartResult { primary, forwarded, invalidArguments, unavailable };
  static constexpr const char* kChannel =
      "com.ryhze.ryhze/windows.activation.v1";
  // The message is only a wakeup. Its parameters never carry trusted input.
  static constexpr UINT kMessage = WM_APP + 0x527;
  static constexpr size_t kQueueLimit = 16;

  HostActivation();
  ~HostActivation();
  HostActivation(const HostActivation&) = delete;
  HostActivation& operator=(const HostActivation&) = delete;

  // Call once, before creating Flutter or opening application storage.
  // nullopt means an ordinary launch (focus-only when another instance exists).
  StartResult Start(const std::optional<std::string>& uri = std::nullopt);
  static std::optional<std::string> CanonicalizeUri(const std::string& uri);

  // These methods and destruction belong to the platform/window thread.
  // Attach after the HWND and messenger exist; Detach before either dies.
  void Attach(flutter::BinaryMessenger* messenger, HWND window,
              std::function<void()> bring_forward);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam);

  // Non-secret endpoint identifier, useful for transport diagnostics/tests.
  std::wstring pipe_name() const;

 private:
  struct State;
  std::shared_ptr<State> state_;
  HANDLE worker_ = nullptr;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::function<void()> bring_forward_;
  bool connected_ = false;
  int64_t last_sent_ = 0;

  void Stop();
  static unsigned __stdcall Serve(void* context);
};

#endif  // RUNNER_HOST_ACTIVATION_H_
