#ifndef RUNNER_HOST_TASKBAR_H_
#define RUNNER_HOST_TASKBAR_H_
#include <windows.h>
#include <flutter/encodable_value.h>
#include <flutter/method_channel.h>
#include <memory>
#include <optional>

enum class TaskbarStatus { none, indeterminate, attention };

// Native boundary, injectable for contract tests. The production constructor
// always supplies the Windows COM implementation; no emulated shell is used.
class TaskbarBackend {
 public:
  virtual ~TaskbarBackend() = default;
  virtual bool Available() const = 0;
  virtual bool Ready() = 0;
  virtual bool Apply(HWND window, TaskbarStatus status) = 0;
};

class HostTaskbar {
 public:
  explicit HostTaskbar(std::unique_ptr<TaskbarBackend> backend = nullptr);
  ~HostTaskbar();
  void Attach(flutter::BinaryMessenger* messenger, HWND window);
  void Detach();
  std::optional<LRESULT> HandleMessage(UINT message, WPARAM wparam, LPARAM lparam);
  UINT button_created_message() const { return button_created_; }
 private:
  std::unique_ptr<TaskbarBackend> backend_;
  std::unique_ptr<flutter::MethodChannel<flutter::EncodableValue>> channel_;
  HWND window_ = nullptr;
  UINT button_created_ = 0;
  bool ready_ = false;
  bool button_seen_ = false;
  TaskbarStatus desired_ = TaskbarStatus::none;
};
#endif  // RUNNER_HOST_TASKBAR_H_
