#ifndef RUNNER_HOST_NOTIFICATIONS_H_
#define RUNNER_HOST_NOTIFICATIONS_H_

#include <windows.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <functional>
#include <map>
#include <memory>
#include <string>

struct NotificationRequest {
  std::string id, kind, uri;
  bool private_preview = true;
};

// Test injection is explicit; production always uses installed-identity-gated
// Windows.UI.Notifications. A successful Submit is not proof of visible display.
class NotificationBackend {
 public:
  virtual ~NotificationBackend() = default;
  virtual bool Available() = 0;
  virtual bool Submit(const std::wstring& tag, const std::wstring& xml) = 0;
  virtual bool Clear() = 0;
};

class HostNotifications {
 public:
  explicit HostNotifications(std::unique_ptr<NotificationBackend> backend = nullptr,
                             std::function<ULONGLONG()> clock = GetTickCount64);
  ~HostNotifications();
  void Attach(flutter::BinaryMessenger* messenger);
  void Detach();
  // Pure, bounded construction shared with real-codec contract tests.
  static bool Validate(const NotificationRequest& request);
  static std::wstring ToastXml(const NotificationRequest& request);
  static std::wstring Tag(const std::string& id);
 private:
  std::unique_ptr<NotificationBackend> backend_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  std::function<ULONGLONG()> clock_;
  std::map<std::wstring, ULONGLONG> recent_;
  ULONGLONG burst_start_ = 0;
  unsigned burst_count_ = 0;
};
#endif  // RUNNER_HOST_NOTIFICATIONS_H_
