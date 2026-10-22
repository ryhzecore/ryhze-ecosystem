#include "host_installations.h"
#include <windows.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
namespace {
using flutter::EncodableValue;
using flutter::EncodableMap;
constexpr const char* kChannel = "com.ryhze.ryhze/windows.installations.v1";
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
class Messenger final : public flutter::BinaryMessenger {
 public:
  std::function<void()> pump;
  void Bind(HostInstallations& host, HWND window) {
    host.Attach(this, window);
    pump = [&host] { host.HandleMessage(WM_TIMER, reinterpret_cast<UINT_PTR>(&host), 0); };
  }
  void Send(const std::string&, const uint8_t*, size_t, flutter::BinaryReply) const override {
    Check(false, "installations must not emit unsolicited messages");
  }
  void SetMessageHandler(const std::string& channel, flutter::BinaryMessageHandler handler) override {
    Check(channel == kChannel, "installation channel identity");
    handler_ = std::move(handler);
  }
  struct Reply {
    EncodableValue value, details;
    std::string code, message;
    int successes = 0, errors = 0, unimplemented = 0, replies = 0;
  };
  std::shared_ptr<Reply> CallAsync(const std::string& method,
                                 std::unique_ptr<EncodableValue> arguments = nullptr) {
    Check(static_cast<bool>(handler_), "attached installation handler");
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const auto wire = codec.EncodeMethodCall(flutter::MethodCall<EncodableValue>(method, std::move(arguments)));
    auto reply = std::make_shared<Reply>();
    const DWORD caller_thread = GetCurrentThreadId();
    handler_(wire->data(), wire->size(), [reply, caller_thread](const uint8_t* data, size_t size) {
      Check(GetCurrentThreadId() == caller_thread, "installation replies stay on platform thread");
      ++reply->replies;
      flutter::MethodResultFunctions<EncodableValue> decoder(
        [&](const EncodableValue* value) { ++reply->successes; if (value) reply->value = *value; },
        [&](const std::string& code, const std::string& message, const EncodableValue* details) {
          ++reply->errors; reply->code = code; reply->message = message; if (details) reply->details = *details;
        }, [&] { ++reply->unimplemented; });
      if (!size) { decoder.NotImplemented(); return; }
      Check(flutter::StandardMethodCodec::GetInstance().DecodeAndProcessResponseEnvelope(
          data, size, &decoder), "valid installation reply envelope");
    });
    return reply;
  }
  Reply Call(const std::string& method, std::unique_ptr<EncodableValue> arguments = nullptr) {
    auto reply = CallAsync(method, std::move(arguments));
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (!reply->replies && GetTickCount64() < deadline) {
      if (pump) pump();
      Sleep(1);
    }
    Check(reply->replies == 1, "exactly one bounded installation reply");
    return *reply;
  }
  flutter::BinaryMessageHandler handler_;
};

}
int main() {
  try {
    Messenger messenger;
    HostInstallations host(L"relative-invalid-fixture");
    HWND window = CreateWindowExW(0, L"STATIC", L"ryhze-inventory-test", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "message-only test window");
    messenger.Bind(host, window);
    const auto probe = messenger.Call("probe");
    const auto& fields = std::get<EncodableMap>(probe.value);
    Check(fields.size() == 4 && std::get<bool>(fields.at(EncodableValue("readOnlyInventory"))) &&
        !std::get<bool>(fields.at(EncodableValue("ownership"))) &&
        !std::get<bool>(fields.at(EncodableValue("launch"))), "probe has no ownership or launch authority");
    Check(messenger.Call("launch").unimplemented == 1, "unknown method rejected");
    Check(messenger.Call("inspectSteam", std::make_unique<EncodableValue>(EncodableMap{
        {EncodableValue("root"), EncodableValue("C:\\")}})).code == "invalidData", "caller paths rejected");
    const auto scan = messenger.Call("inspectSteam");
    Check(scan.successes == 1 && std::get<std::string>(std::get<EncodableMap>(scan.value).at(
        EncodableValue("quality"))) == "unavailable", "invalid injected root unavailable");
    host.Detach();
    HostInstallations second(L"relative-invalid-fixture");
    messenger.Bind(second, window);
    auto pending = messenger.CallAsync("inspectSteam");
    Check(messenger.Call("inspectSteam").code == "conflict", "concurrent scan bounded");
    Check(messenger.Call("cancel").successes == 1, "explicit cancellation acknowledged");
    Check(pending->replies == 1 && pending->code == "cancelled", "pending result cancelled on platform thread");
    host.Detach();
    second.Detach();
    DestroyWindow(window);
    std::cout << "PASS read-only installation channel contracts\n";
    return 0;
  } catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << "\n"; return 1; }
}