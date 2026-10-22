#include "host_content_transfer.h"
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <bcrypt.h>
#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <utility>
namespace {
using flutter::EncodableValue;
using flutter::EncodableMap;
namespace fs = std::filesystem;
constexpr const char* kChannel = "com.ryhze.ryhze/windows.content-transfer.v1";
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Messenger final : public flutter::BinaryMessenger {
 public:
  std::function<void()> pump;
  void Bind(HostContentTransfer& host, HWND window) {
    host.Attach(this, window);
    pump = [&host] { host.HandleMessage(HostContentTransfer::kMessage, 0, 0); };
  }
  void Send(const std::string&, const uint8_t*, size_t, flutter::BinaryReply) const override {
    Check(false, "updates must not emit unsolicited messages");
  }
  void SetMessageHandler(const std::string& channel, flutter::BinaryMessageHandler handler) override {
    Check(channel == kChannel, "update channel identity");
    handler_ = std::move(handler);
  }
  struct Reply {
    EncodableValue value, details;
    std::string code, message;
    int successes = 0, errors = 0, unimplemented = 0, replies = 0;
  };
  std::shared_ptr<Reply> CallAsync(const std::string& method,
                                 std::unique_ptr<EncodableValue> arguments = nullptr) {
    Check(static_cast<bool>(handler_), "attached update handler");
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const auto wire = codec.EncodeMethodCall(flutter::MethodCall<EncodableValue>(method, std::move(arguments)));
    auto reply = std::make_shared<Reply>();
    const DWORD caller_thread = GetCurrentThreadId();
    handler_(wire->data(), wire->size(), [reply, caller_thread](const uint8_t* data, size_t size) {
      Check(GetCurrentThreadId() == caller_thread, "update replies stay on platform thread");
      ++reply->replies;
      flutter::MethodResultFunctions<EncodableValue> decoder(
        [&](const EncodableValue* value) { ++reply->successes; if (value) reply->value = *value; },
        [&](const std::string& code, const std::string& message, const EncodableValue* details) {
          ++reply->errors; reply->code = code; reply->message = message; if (details) reply->details = *details;
        }, [&] { ++reply->unimplemented; });
      if (!size) { decoder.NotImplemented(); return; }
      Check(flutter::StandardMethodCodec::GetInstance().DecodeAndProcessResponseEnvelope(
          data, size, &decoder), "valid update reply envelope");
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
    Check(reply->replies == 1, "exactly one bounded update reply");
    return *reply;
  }
  flutter::BinaryMessageHandler handler_;
};
std::unique_ptr<EncodableValue> Args(EncodableMap map) {
  return std::make_unique<EncodableValue>(std::move(map));
}
EncodableMap Request() {
  std::vector<uint8_t> digest(32);
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "hash");
  uint8_t bytes[] = {1, 2, 3};
  const auto status = BCryptHash(algorithm, nullptr, 0, bytes, 3, digest.data(), 32);
  BCryptCloseAlgorithmProvider(algorithm, 0); Check(status >= 0, "digest");
  return {{EncodableValue("slot"), EncodableValue(int32_t{0})},
    {EncodableValue("attempt"), EncodableValue(int32_t{0})},
    {EncodableValue("operationId"), EncodableValue(std::string(64, 'a'))},
    {EncodableValue("expectedSize"), EncodableValue(int64_t{3})},
    {EncodableValue("expectedSha256"), EncodableValue(digest)},
    {EncodableValue("rootIdentity"), EncodableValue("")},
    {EncodableValue("directoryIdentity"), EncodableValue("")},
    {EncodableValue("sourceIdentity"), EncodableValue("")},
    {EncodableValue("observedBytes"), EncodableValue(int64_t{0})},
    {EncodableValue("observedSha256"), EncodableValue("")}};
}
EncodableMap Observe(EncodableMap request, const Messenger::Reply& reply, const char* state) {
  Check(reply.successes == 1, "successful native response");
  const auto& value = std::get<EncodableMap>(reply.value);
  Check(value.size() == 7 && std::get<std::string>(value.at(EncodableValue("state"))) == state, "closed observation");
  for (const auto key : {"rootIdentity", "directoryIdentity", "sourceIdentity", "observedBytes", "observedSha256"})
    request[EncodableValue(key)] = value.at(EncodableValue(key));
  return request;
}
std::unique_ptr<EncodableValue> Wrapped(const EncodableMap& request) {
  return Args({{EncodableValue("request"), EncodableValue(request)}});
}
void Run() {
  wchar_t temp[MAX_PATH + 1]{};
  Check(GetTempPathW(MAX_PATH, temp) != 0, "temp");
  std::array<uint8_t, 16> random{};
  Check(BCryptGenRandom(nullptr, random.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "random");
  std::wstring name = L"ryhze-transfer-channel-";
  for (auto b : random) { name += L"0123456789abcdef"[b >> 4]; name += L"0123456789abcdef"[b & 15]; }
  const fs::path root = fs::path(temp) / name;
  Check(CreateDirectoryW(root.c_str(), nullptr) != FALSE, "exclusive fixture");
  fs::create_directory(root / "incoming"); fs::create_directory(root / "content-staging");
  std::wcout << L"Retained native channel fixture: " << root.wstring() << L'\n';
  HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "message window");
  {
    Messenger messenger; HostContentTransfer host(root.wstring()); messenger.Bind(host, window);
    const auto probe = std::get<EncodableMap>(messenger.Call("probe").value);
    Check(probe.size() == 5 && std::get<bool>(probe.at(EncodableValue("transfer"))) &&
      !std::get<bool>(probe.at(EncodableValue("activation"))) &&
      !std::get<bool>(probe.at(EncodableValue("authenticity"))), "inert transfer capability");
    Check(messenger.Call("probe", Args({})).code == "invalidData", "strict probe shape");
    Check(messenger.Call("execute").unimplemented == 1, "no executable capability");
    auto request = Request();
    for (const auto field : {"root", "publisher", "path", "execute"}) {
      auto bad = request; bad[EncodableValue(field)] = EncodableValue("injected");
      Check(messenger.Call("inspect", Wrapped(bad)).code == "invalidData", "reject injected authority fields");
    }
    for (const auto& slot : {EncodableValue(int64_t{-1}), EncodableValue(int64_t{8}), EncodableValue(false), EncodableValue(0.0)}) {
      auto bad = request; bad[EncodableValue("slot")] = slot;
      Check(messenger.Call("inspect", Wrapped(bad)).code == "invalidData", "slot exact integer bounds");
    }
    auto begin = messenger.Call("begin", Args({{EncodableValue("request"), EncodableValue(request)},
      {EncodableValue("resume"), EncodableValue()}}));
    request = Observe(request, begin, "writing");
    Check(request.at(EncodableValue("rootIdentity")) == probe.at(EncodableValue("deviceId")), "consistent device identity");
    auto append = messenger.Call("append", Args({{EncodableValue("request"), EncodableValue(request)},
      {EncodableValue("bytes"), EncodableValue(std::vector<uint8_t>{1,2,3})}}));
    request = Observe(request, append, "writing");
    request = Observe(request, messenger.Call("finish", Wrapped(request)), "complete");
    Observe(request, messenger.Call("inspect", Wrapped(request)), "complete");
    Observe(request, messenger.Call("discard", Wrapped(request)), "discarded");
    auto pending = messenger.CallAsync("inspect", Wrapped(request));
    messenger.Call("cancel");
    auto drain = messenger.CallAsync("drain"), second = messenger.CallAsync("drain"), close = messenger.CallAsync("close");
    const auto deadline = GetTickCount64() + 15000;
    while ((!drain->replies || !second->replies || !close->replies) && GetTickCount64() < deadline) {
      messenger.pump(); Sleep(1);
    }
    Check(pending->replies == 1 && drain->successes == 1 && second->successes == 1 && close->successes == 1,
      "actual worker settlement acknowledges concurrent drain and close exactly once");
    Check(messenger.Call("inspect", Wrapped(request)).code == "unsupported", "closed permanent admission");
    host.Detach(); Check(!messenger.handler_, "detach unbinds callback");
  }
  {
    Messenger original, replacement;
    HostContentTransfer host(root.wstring()); original.Bind(host, window);
    HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(entered && release, "exclusive worker gate events");
    HostContentTransfer::HoldNextWorkerForTesting(entered, release);
    auto pending = original.CallAsync("inspect", Wrapped(Request()));
    const bool started = WaitForSingleObject(entered, 15000) == WAIT_OBJECT_0;
    if (!started) SetEvent(release);
    Check(started, "old worker is pending before detach");
    host.Detach();
    Check(pending->replies == 1 && pending->code == "cancelled", "detach cancels result without claiming worker exit");
    replacement.Bind(host, window);
    auto draining = replacement.CallAsync("drain");
    auto closing = replacement.CallAsync("close");
    Check(draining->replies == 0 && closing->replies == 0, "reattached drain waits held worker");
    SetEvent(release);
    const auto deadline = GetTickCount64() + 15000;
    while ((!draining->replies || !closing->replies) && GetTickCount64() < deadline) {
      // Do not call Messenger::pump/Poll. Only genuine window timers/messages
      // may detect exit of the old detached worker, which has no reply window.
      MSG message{};
      while (PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
        if (!host.HandleMessage(message.message, message.wParam, message.lParam))
          DispatchMessageW(&message);
      }
      Sleep(1);
    }
    Check(draining->successes == 1 && closing->successes == 1 &&
      draining->replies == 1 && closing->replies == 1, "reattached timer acknowledges actual drain exactly once");
    Check(replacement.Call("inspect", Wrapped(Request())).code == "unsupported", "reattached close keeps admission shut");
    host.Detach(); CloseHandle(entered); CloseHandle(release);
  }
  DestroyWindow(window);
}
}  // namespace
int main() { try { Run(); std::cout << "host content transfer passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
