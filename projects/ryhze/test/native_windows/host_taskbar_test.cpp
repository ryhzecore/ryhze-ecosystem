#include "host_taskbar.h"
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
void Check(bool value, const char* description) {
  if (!value) throw std::runtime_error(description);
}
struct Window {
  HWND value = CreateWindowExW(0, L"STATIC", L"taskbar contract test", 0,
      0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  ~Window() { if (value) DestroyWindow(value); }
};
class Backend final : public TaskbarBackend {
 public:
  bool available = true, ready = true, succeeds = true;
  int starts = 0;
  DWORD owner = GetCurrentThreadId();
  std::vector<TaskbarStatus> states;
  bool Available() const override { return available; }
  bool Ready() override { ++starts; return ready; }
  bool Apply(HWND window, TaskbarStatus value) override {
    Check(window && GetCurrentThreadId() == owner, "taskbar uses platform thread/window");
    Check(starts > 0, "no taskbar calls before button-created handshake");
    states.push_back(value);
    return succeeds;
  }
};
class Messenger final : public flutter::BinaryMessenger {
 public:
  void Send(const std::string&, const uint8_t*, size_t, flutter::BinaryReply) const override {
    Check(false, "taskbar cannot emit commands");
  }
  void SetMessageHandler(const std::string& name, flutter::BinaryMessageHandler handler) override {
    Check(name == "com.ryhze.ryhze/windows.taskbar.v1", "taskbar channel");
    handler_ = std::move(handler);
  }
  struct Reply { EncodableValue value; int successes = 0, errors = 0, unknown = 0; };
  Reply Call(const char* method, std::unique_ptr<EncodableValue> args = nullptr) {
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    auto wire = codec.EncodeMethodCall(flutter::MethodCall<EncodableValue>(method, std::move(args)));
    Reply reply;
    flutter::MethodResultFunctions<EncodableValue> decoder(
      [&](const EncodableValue* value) { ++reply.successes; if (value) reply.value = *value; },
      [&](const std::string& code, const std::string& text, const EncodableValue*) {
        Check(code == "invalidData" && text.empty(), "closed safe error"); ++reply.errors;
      }, [&] { ++reply.unknown; });
    Check(static_cast<bool>(handler_), "taskbar attached");
    handler_(wire->data(), wire->size(), [&](const uint8_t* data, size_t size) {
      if (!size) decoder.NotImplemented();
      else Check(codec.DecodeAndProcessResponseEnvelope(data, size, &decoder), "real taskbar codec");
    });
    Check(reply.successes + reply.errors + reply.unknown == 1, "one taskbar reply");
    return reply;
  }
  Reply Set(const char* state) {
    return Call("set", std::make_unique<EncodableValue>(EncodableMap{
      {EncodableValue("state"), EncodableValue(state)}}));
  }
  flutter::BinaryMessageHandler handler_;
};
void Result(const Messenger::Reply& reply, const char* expected) {
  Check(reply.successes == 1, "taskbar set result");
  const auto& map = std::get<EncodableMap>(reply.value);
  Check(map.size() == 1 && std::get<std::string>(map.at(EncodableValue("state"))) == expected,
        "exact taskbar result schema");
}
void ProtocolAndLifecycle() {
  Window window;
  Check(window.value != nullptr, "message-only test window");
  auto backend = std::make_unique<Backend>();
  auto* native = backend.get();
  Messenger messenger;
  HostTaskbar host(std::move(backend));
  host.Attach(&messenger, window.value);
  const auto probe = messenger.Call("probe");
  const auto& map = std::get<EncodableMap>(probe.value);
  Check(map.size() == 2 && std::get<int32_t>(map.at(EncodableValue("schemaVersion"))) == 1 &&
      std::get<bool>(map.at(EncodableValue("progress"))), "exact taskbar capability");
  Check(messenger.Call("probe", std::make_unique<EncodableValue>(EncodableMap{})).errors == 1,
        "probe has no arguments");
  Check(messenger.Set("50-percent").errors == 1, "no fabricated percentages");
  Check(messenger.Call("set").errors == 1, "set requires exact schema");
  Check(messenger.Call("set", std::make_unique<EncodableValue>(EncodableMap{
      {EncodableValue("state"), EncodableValue("none")},
      {EncodableValue("command"), EncodableValue("bad")}})).errors == 1, "reject extra fields");
  Check(messenger.Call("launch").unknown == 1, "no taskbar commands");
  Result(messenger.Set("indeterminate"), "queued");
  Check(native->states.empty(), "state retained before native button exists");
  Check(!host.HandleMessage(WM_CLOSE, 0, 0), "close handshake unchanged");
  Check(host.button_created_message() != 0, "registered shell message");
  host.HandleMessage(host.button_created_message(), 0, 0);
  Check(native->states.size() == 1 && native->states.back() == TaskbarStatus::indeterminate,
        "retained state applied after native button creation");
  Result(messenger.Set("attention"), "applied");
  Check(native->states.back() == TaskbarStatus::attention, "attention stays a distinct state");
  host.HandleMessage(host.button_created_message(), 0, 0);
  Check(native->starts == 2 && native->states.back() == TaskbarStatus::attention,
        "Explorer recreation reapplies current state");
  native->succeeds = false;
  Result(messenger.Set("none"), "unavailable");
  native->succeeds = true;
  native->ready = false;
  host.HandleMessage(host.button_created_message(), 0, 0);
  Result(messenger.Set("indeterminate"), "unavailable");
  native->ready = true;
  host.HandleMessage(host.button_created_message(), 0, 0);
  host.Detach();
  Check(!messenger.handler_ && native->states.back() == TaskbarStatus::none,
        "detach clears native shell state and callback");
  const size_t count = native->states.size();
  host.HandleMessage(host.button_created_message(), 0, 0);
  Check(native->states.size() == count, "detached window ignored");
  host.Detach();
}
void UnavailableCapability() {
  Window window;
  auto backend = std::make_unique<Backend>();
  backend->available = false;
  Messenger messenger;
  HostTaskbar host(std::move(backend));
  host.Attach(&messenger, window.value);
  const auto probe = messenger.Call("probe");
  Check(!std::get<bool>(std::get<EncodableMap>(probe.value).at(EncodableValue("progress"))),
        "unavailable backend is not claimed operational");
  Result(messenger.Set("attention"), "unavailable");
}
}  // namespace
int main() {
  try {
    ProtocolAndLifecycle();
    UnavailableCapability();
    std::cout << "PASS: taskbar codec/state/lifecycle contracts; real shell display requires Windows acceptance\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
