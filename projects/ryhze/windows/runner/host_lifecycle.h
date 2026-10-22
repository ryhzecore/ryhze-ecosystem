#ifndef RUNNER_HOST_LIFECYCLE_H_
#define RUNNER_HOST_LIFECYCLE_H_

#include <windows.h>

#include <flutter/binary_messenger.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

// Window-thread bridge only. It does not block power/session events, save data,
// implement a background service, or replace Flutter's close handshake.
class HostLifecycle {
 public:
  static constexpr const char* kChannel =
      "com.ryhze.ryhze/windows.lifecycle.v1";

  HostLifecycle() = default;
  ~HostLifecycle();
  HostLifecycle(const HostLifecycle&) = delete;
  HostLifecycle& operator=(const HostLifecycle&) = delete;

  void Attach(flutter::BinaryMessenger* messenger);
  // Must run before the messenger/engine is destroyed.
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam,
                                       LPARAM lparam);

 private:
  flutter::EncodableMap Snapshot() const;
  void Transition(const char* state);

  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  bool connected_ = false;
  int64_t sequence_ = 0;
  std::string state_ = "active";
};

#endif  // RUNNER_HOST_LIFECYCLE_H_
