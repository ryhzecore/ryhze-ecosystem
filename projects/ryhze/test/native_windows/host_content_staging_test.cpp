#include "host_content_staging.h"
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
constexpr const char* kChannel = "com.ryhze.ryhze/windows.content-staging.v1";
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
class Messenger final : public flutter::BinaryMessenger {
 public:
  std::function<void()> pump;
  void Bind(HostContentStaging& host, HWND window) {
    host.Attach(this, window);
    pump = [&host] { host.HandleMessage(HostContentStaging::kMessage, 0, 0); };
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
  std::vector<uint8_t> hash(32);
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "hash");
  uint8_t bytes[] = {1, 2, 3};
  const auto status = BCryptHash(algorithm, nullptr, 0, bytes, 3, hash.data(), 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  Check(status >= 0, "digest");
  return {{EncodableValue("slot"), EncodableValue(int32_t{0})},
    {EncodableValue("operationId"), EncodableValue(std::vector<uint8_t>(32, 1))},
    {EncodableValue("manifestSha256"), EncodableValue(std::vector<uint8_t>(32, 2))},
    {EncodableValue("sourceRelative"), EncodableValue("payload.bin")},
    {EncodableValue("size"), EncodableValue(int64_t{3})},
    {EncodableValue("minimumVerifiedBytes"), EncodableValue(int64_t{0})},
    {EncodableValue("sha256"), EncodableValue(hash)},
    {EncodableValue("rootIdentity"), EncodableValue("")},
    {EncodableValue("sourceIdentity"), EncodableValue("")}};
}
std::string State(const Messenger::Reply& reply) {
  Check(reply.successes == 1 && reply.replies == 1, "one successful response");
  const auto& map = std::get<EncodableMap>(reply.value);
  Check(map.size() == 5, "closed receipt");
  return std::get<std::string>(map.at(EncodableValue("state")));
}
void Run() {
  {
    Messenger messenger;
    HostContentStaging unavailable(std::wstring{});
    unavailable.Attach(&messenger, nullptr);
    auto probe = std::get<EncodableMap>(messenger.Call("probe").value);
    Check(!std::get<bool>(probe.at(EncodableValue("staging"))) &&
      probe.at(EncodableValue("deviceId")).IsNull(), "unavailable has no device identity");
    Check(State(messenger.Call("inspect", Args(Request()))) == "unavailable", "unconfigured no effects");
  }
  wchar_t temp[MAX_PATH + 1]{};
  Check(GetTempPathW(MAX_PATH, temp) != 0, "temp");
  std::array<uint8_t, 16> random{};
  Check(BCryptGenRandom(nullptr, random.data(), 16, BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "random");
  std::wstring name = L"ryhze-content-channel-";
  for (auto b : random) { name += L"0123456789abcdef"[b >> 4]; name += L"0123456789abcdef"[b & 15]; }
  const fs::path root = fs::path(temp) / name;
  Check(CreateDirectoryW(root.c_str(), nullptr) != 0, "exclusive fixture");
  struct Cleanup { fs::path root; ~Cleanup() { std::error_code error; fs::remove_all(root, error); } } cleanup{root};
  fs::create_directory(root / "incoming"); fs::create_directory(root / "content-staging");
  { std::ofstream file(root / "incoming" / "payload.bin", std::ios::binary); const char data[] = {1,2,3}; file.write(data, 3); }
  HWND window = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
  Check(window != nullptr, "message window");
  {
    Messenger messenger;
    HostContentStaging host(root.wstring()); messenger.Bind(host, window);
    const auto probe = std::get<EncodableMap>(messenger.Call("probe").value);
    Check(probe.size() == 5 && std::get<bool>(probe.at(EncodableValue("staging"))) &&
      !std::get<bool>(probe.at(EncodableValue("activation"))) &&
      !std::get<bool>(probe.at(EncodableValue("authenticity"))), "inert capability only");
    const auto discard_probe = std::get<EncodableMap>(messenger.Call("probeDiscard").value);
    Check(discard_probe.size() == 2 && std::get<int32_t>(discard_probe.at(EncodableValue("schemaVersion"))) == 1 &&
      std::get<bool>(discard_probe.at(EncodableValue("discard"))), "separate explicit discard capability");
    Check(messenger.Call("probeDiscard", Args({})).code == "invalidData", "probe takes no authority arguments");
    for (const auto& value : {EncodableValue(), EncodableValue("not a plan"),
        EncodableValue(std::vector<uint8_t>{}), EncodableValue(std::vector<uint8_t>(512 * 1024 + 1, 1))}) {
      Check(messenger.Call("discard", Args({{EncodableValue("plan"), value}})).code == "invalidData", "bounded opaque discard plan");
    }
    Check(messenger.Call("inspectDiscard", Args({{EncodableValue("plan"), EncodableValue(std::vector<uint8_t>{1})},
        {EncodableValue("root"), EncodableValue("outside")}})).code == "invalidData", "discard cannot select a root");
    auto request = Request();
    Check(messenger.Call("stage", Args(request)).code == "invalidData", "stage requires pinned preflight identities");
    Check(messenger.Call("planDiscard", Args(request)).code == "invalidData", "discard plan requires pinned preflight identities");
    for (auto key : {"root", "publisher", "bytes", "execute"}) {
      auto extra = request; extra.emplace(EncodableValue(key), EncodableValue("injected"));
      Check(messenger.Call("inspect", Args(extra)).code == "invalidData", "reject extra authority");
    }
    auto absent_floor = request;
    absent_floor.erase(EncodableValue("minimumVerifiedBytes"));
    Check(messenger.Call("inspect", Args(absent_floor)).code == "invalidData", "floor is explicit on the wire");
    absent_floor.emplace(EncodableValue("unexpectedFloor"), EncodableValue(int64_t{0}));
    Check(messenger.Call("inspect", Args(absent_floor)).code == "invalidData", "same-sized unknown field cannot replace floor");
    for (const auto& value : {EncodableValue(int64_t{-1}), EncodableValue(int64_t{1}),
                             EncodableValue(int64_t{4}), EncodableValue(0.0),
                             EncodableValue(false), EncodableValue("0"), EncodableValue()}) {
      auto bad = request;
      bad[EncodableValue("minimumVerifiedBytes")] = value;
      Check(messenger.Call("inspect", Args(bad)).code == "invalidData", "floor bounds and integer datatype strict");
    }
    for (auto path : {"C:/outside", "\\outside", "folder\\file", "/absolute"}) {
      auto bad = request; bad[EncodableValue("sourceRelative")] = EncodableValue(path);
      Check(messenger.Call("inspect", Args(bad)).code == "invalidData", "one relative wire encoding");
    }
    auto too_large = request; too_large[EncodableValue("size")] = EncodableValue(int64_t{1073741825});
    Check(messenger.Call("inspect", Args(too_large)).code == "invalidData", "bounded streaming size");
    Check(messenger.Call("launch").unimplemented == 1, "no launch method");
    auto traversal = request; traversal[EncodableValue("sourceRelative")] = EncodableValue("../payload.bin");
    Check(messenger.Call("inspect", Args(traversal)).code == "invalidData", "native traversal rejected");
    auto preflight = messenger.Call("inspect", Args(request));
    Check(State(preflight) == "checkpoint", "real read-only preflight");
    const auto observed = std::get<EncodableMap>(preflight.value);
    Check(observed.at(EncodableValue("rootIdentity")) == probe.at(EncodableValue("deviceId")), "probe is same pinned root identity");
    request[EncodableValue("rootIdentity")] = observed.at(EncodableValue("rootIdentity"));
    request[EncodableValue("sourceIdentity")] = observed.at(EncodableValue("sourceIdentity"));
    Check(State(messenger.Call("stage", Args(request))) == "staged", "real native stage");
    Check(State(messenger.Call("inspect", Args(request))) == "staged", "real native revalidation");
    request[EncodableValue("minimumVerifiedBytes")] = EncodableValue(int64_t{3});
    Check(State(messenger.Call("stage", Args(request))) == "staged", "wire final-size floor accepted");
    const auto chunk = root / "content-staging" / "slot-0" / "chunk-0.bin";
    const auto retained = root / "retained-removed-chunk.bin";
    Check(MoveFileW(chunk.c_str(), retained.c_str()) != FALSE, "retain source chunk removed after inspection");
    const auto shortened = messenger.Call("stage", Args(request));
    Check(State(shortened) == "recovery" &&
      std::get<int64_t>(std::get<EncodableMap>(shortened.value).at(EncodableValue("verifiedBytes"))) == 0 &&
      !fs::exists(chunk) && fs::file_size(retained) == 3,
      "wire floor reaches native refusal without recreating lost bytes");
    Check(MoveFileW(retained.c_str(), chunk.c_str()) != FALSE, "restore exact retained fixture for cleanup plan");
    const auto plan_reply = messenger.Call("planDiscard", Args(request));
    Check(plan_reply.successes == 1, "read-only discard planning replies");
    const auto plan_result = std::get<EncodableMap>(plan_reply.value);
    Check(plan_result.size() == 4 && std::get<std::string>(plan_result.at(EncodableValue("state"))) == "staged" &&
      fs::exists(chunk), "planning preserves staged content");
    const auto plan = std::get<std::vector<uint8_t>>(plan_result.at(EncodableValue("plan")));
    Check(!plan.empty() && plan.size() <= 512 * 1024, "bounded opaque plan from native");
    const auto inspect_discard = std::get<EncodableMap>(messenger.Call("inspectDiscard",
        Args({{EncodableValue("plan"), EncodableValue(plan)}})).value);
    Check(inspect_discard.size() == 3 &&
      std::get<std::string>(inspect_discard.at(EncodableValue("state"))) == "ready" && fs::exists(chunk),
      "discard inspection validates without effects");
    const auto removed = std::get<EncodableMap>(messenger.Call("discard",
        Args({{EncodableValue("plan"), EncodableValue(plan)}})).value);
    Check(removed.size() == 3 && std::get<std::string>(removed.at(EncodableValue("state"))) == "completed" &&
      std::get<int64_t>(removed.at(EncodableValue("removedBytes"))) == 3 &&
      std::get<int64_t>(removed.at(EncodableValue("removedFiles"))) == 2 &&
      !fs::exists(root / "content-staging" / "slot-0") && fs::file_size(root / "incoming" / "payload.bin") == 3,
      "explicit discard removes only fixture payload and checkpoint then slot");
    const auto recheck = std::get<EncodableMap>(messenger.Call("inspectDiscard",
        Args({{EncodableValue("plan"), EncodableValue(plan)}})).value);
    Check(std::get<std::string>(recheck.at(EncodableValue("state"))) == "completed", "read-only completion reconciliation");
    auto running = messenger.CallAsync("inspect", Args(request));
    messenger.Call("cancel");
    auto draining = messenger.CallAsync("drain");
    auto second_drain = messenger.CallAsync("drain");
    auto closing = messenger.CallAsync("close");
    const ULONGLONG stop = GetTickCount64() + 15000;
    while ((!draining->replies || !second_drain->replies || !closing->replies) && GetTickCount64() < stop) {
      messenger.pump(); Sleep(1);
    }
    Check(draining->successes == 1 && second_drain->successes == 1 && closing->successes == 1 &&
      draining->replies == 1 && second_drain->replies == 1 && closing->replies == 1,
      "overlapping drain and close acknowledge actual exit exactly once");
    Check(running->replies == 1, "cancel or completed reply exactly once");
    Check(messenger.Call("close").successes == 1, "close drains");
    Check(State(messenger.Call("stage", Args(request))) == "unavailable", "closed admission");
    host.Detach(); Check(!messenger.handler_, "detached callback");
  }
  DestroyWindow(window);
}
}
int main() { try { Run(); std::cout << "host content staging passed\n"; return 0; }
  catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; } }
