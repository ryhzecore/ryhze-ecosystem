#include "host_updates.h"
#include "secure_files.h"

#include <windows.h>
#include <bcrypt.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>
#include <memory>
#include <utility>

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>
#include <functional>

namespace {
namespace fs = std::filesystem;
using ryhze::UpdateDigest;
using ryhze::UpdateStager;
using ryhze::UpdateStageState;
void Check(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
struct Fixture {
  fs::path directory;
  Fixture() {
    wchar_t temp[MAX_PATH + 1]{};
    Check(GetTempPathW(MAX_PATH, temp) != 0, "temporary fixture location");
    std::array<uint8_t, 16> bytes{};
    Check(BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()),
                         BCRYPT_USE_SYSTEM_PREFERRED_RNG) >= 0, "fixture identity");
    constexpr wchar_t hex[] = L"0123456789abcdef";
    std::wstring name = L"ryhze-host-updates-test-";
    for (uint8_t byte : bytes) { name += hex[byte >> 4]; name += hex[byte & 15]; }
    directory = fs::path(temp) / name;
    Check(CreateDirectoryW(directory.c_str(), nullptr) != 0, "exclusive fixture directory");
  }
  ~Fixture() { std::error_code error; fs::remove_all(directory, error); }
  fs::path Root(const wchar_t* name) const { return directory / name; }
};
UpdateDigest Digest(const std::vector<uint8_t>& bytes) {
  BCRYPT_ALG_HANDLE algorithm = nullptr;
  Check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "fixture hash provider");
  UpdateDigest result{};
  const auto status = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
      static_cast<ULONG>(bytes.size()), result.data(), static_cast<ULONG>(result.size()));
  BCryptCloseAlgorithmProvider(algorithm, 0);
  Check(status >= 0, "fixture digest");
  return result;
}
std::wstring Name(const UpdateDigest& digest) {
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring name;
  for (uint8_t byte : digest) { name += hex[byte >> 4]; name += hex[byte & 15]; }
  return name + L".stage";
}
std::vector<uint8_t> Read(const fs::path& path) {
  Check(fs::file_size(path) <= UpdateStager::kMaximumBytes, "bounded explicit fixture");
  std::ifstream stream(path, std::ios::binary);
  Check(stream.good(), "explicit fixture readable");
  return std::vector<uint8_t>(std::istreambuf_iterator<char>(stream), {});
}
void Put(const fs::path& path, const std::vector<uint8_t>& bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  Check(stream.good(), "synthetic fixture write");
}
std::wstring Environment(const wchar_t* name) {
  const DWORD size = GetEnvironmentVariableW(name, nullptr, 0);
  if (!size || size > 32768) return {};
  std::wstring value(size, L'\0');
  const DWORD read = GetEnvironmentVariableW(name, value.data(), size);
  if (!read || read >= size) return {};
  value.resize(read);
  return value;
}
UpdateDigest Pin(const std::wstring& text) {
  Check(text.size() == 64, "independently supplied SHA256 publisher pin required");
  auto nibble = [](wchar_t c) -> int {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
  };
  UpdateDigest pin{};
  for (size_t i = 0; i < pin.size(); ++i) {
    const int high = nibble(text[i * 2]), low = nibble(text[i * 2 + 1]);
    Check(high >= 0 && low >= 0, "publisher pin hexadecimal");
    pin[i] = static_cast<uint8_t>((high << 4) | low);
  }
  return pin;
}
using flutter::EncodableMap;
using flutter::EncodableValue;
constexpr const char* kChannel = "com.ryhze.ryhze/windows.updates.v1";
class Messenger final : public flutter::BinaryMessenger {
 public:
  std::function<void()> pump;
  void Bind(HostUpdates& host, HWND window) {
    host.Attach(this, window);
    pump = [&host] { host.HandleMessage(HostUpdates::kMessage, 0, 0); };
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
EncodableMap Stage(const std::vector<uint8_t>& bytes, const UpdateDigest& digest) {
  return {{EncodableValue("bytes"), EncodableValue(bytes)},
    {EncodableValue("sha256"), EncodableValue(std::vector<uint8_t>(digest.begin(), digest.end()))}};
}
EncodableMap Inspect(const UpdateDigest& digest, int64_t size) {
  return {{EncodableValue("size"), EncodableValue(size)},
    {EncodableValue("sha256"), EncodableValue(std::vector<uint8_t>(digest.begin(), digest.end()))}};
}
void Error(const Messenger::Reply& reply) {
  Check(reply.errors == 1 && !reply.successes && !reply.unimplemented && reply.code == "invalidData" &&
    reply.message.empty() && reply.details.IsNull(), "closed invalid-data error, no raw input/path/signature details");
}
void State(const Messenger::Reply& reply, const char* state, int64_t bytes) {
  Check(reply.successes == 1 && !reply.errors && !reply.unimplemented, "one update state reply");
  const auto* map = std::get_if<EncodableMap>(&reply.value);
  Check(map && map->size() == 2, "exact closed state schema");
  Check(std::get<std::string>(map->at(EncodableValue("state"))) == state &&
        std::get<int64_t>(map->at(EncodableValue("bytes"))) == bytes, "typed update state and size");
}
void Probe(const Messenger::Reply& reply, bool configured) {
  Check(reply.successes == 1 && !reply.errors, "probe success");
  const auto& map = std::get<EncodableMap>(reply.value);
  Check(map.size() == 3 && std::get<int32_t>(map.at(EncodableValue("schemaVersion"))) == 1 &&
    std::get<bool>(map.at(EncodableValue("staging"))) == configured &&
    !std::get<bool>(map.at(EncodableValue("activation"))), "activation never enabled");
}
void Empty(const Messenger::Reply& reply) {
  Check(reply.replies == 1 && reply.successes == 1 && reply.value.IsNull() &&
        !reply.errors && !reply.unimplemented, "exact null control acknowledgement");
}
void ClosedSchemaAndNoPin() {
  Fixture fixture;
  Messenger first, second;
  const std::vector<uint8_t> bytes = {1, 2, 3};
  const auto digest = Digest(bytes);
  {
    HostUpdates production;
    production.Attach(&first);
    Probe(first.Call("probe"), false);
    State(first.Call("stage", Args(Stage(bytes, digest))), "unavailable", 0);
    State(first.Call("inspect", Args(Inspect(digest, 3))), "unavailable", 0);
  }
  Check(!first.handler_, "production destructor detaches");
  {
    HostUpdates no_pin(fixture.Root(L"never-created").wstring());
    no_pin.Attach(&first);
    Probe(first.Call("probe", std::make_unique<EncodableValue>()), false);
    State(first.Call("stage", Args(Stage(bytes, digest))), "unavailable", 0);
    Error(first.Call("probe", Args({})));
    for (const auto* method : {"cancel", "drain", "close"}) {
      Error(first.Call(method, Args({})));
      Error(first.Call(method, std::make_unique<EncodableValue>(true)));
      Empty(first.Call(method));
      Empty(first.Call(method, std::make_unique<EncodableValue>()));
    }
    for (const auto* method : {"stage", "inspect"}) {
      Error(first.Call(method));
      Error(first.Call(method, std::make_unique<EncodableValue>(true)));
      Error(first.Call(method, Args({})));
      auto malformed = method == std::string("stage") ? Stage(bytes, digest) : Inspect(digest, 3);
      for (const auto* injection : {"publisher", "certificate", "root", "execute"}) {
        auto extra = malformed;
        extra.emplace(EncodableValue(injection), EncodableValue("untrusted-channel-input"));
        Error(first.Call(method, Args(extra)));
      }
      malformed[EncodableValue("sha256")] = EncodableValue(std::string("../not-a-hash"));
      Error(first.Call(method, Args(malformed)));
      malformed[EncodableValue("sha256")] = EncodableValue(std::vector<uint8_t>(31));
      Error(first.Call(method, Args(malformed)));
      malformed.erase(EncodableValue("sha256"));
      malformed.emplace(EncodableValue(int32_t{7}), EncodableValue(std::vector<uint8_t>(32)));
      Error(first.Call(method, Args(malformed)));
    }
    Error(first.Call("stage", Args(Stage({}, digest))));
    Error(first.Call("stage", Args(Stage(std::vector<uint8_t>(UpdateStager::kMaximumBytes + 1), digest))));
    auto invalid_bytes = Stage(bytes, digest);
    invalid_bytes[EncodableValue("bytes")] = EncodableValue("not-byte-data");
    Error(first.Call("stage", Args(invalid_bytes)));
    for (int64_t size : {int64_t{0}, int64_t{-1}, int64_t{UpdateStager::kMaximumBytes + 1}})
      Error(first.Call("inspect", Args(Inspect(digest, size))));
    auto invalid_size = Inspect(digest, 3);
    invalid_size[EncodableValue("size")] = EncodableValue(3.0);
    Error(first.Call("inspect", Args(invalid_size)));
    auto small_size = Inspect(digest, 3);
    small_size[EncodableValue("size")] = EncodableValue(int32_t{3});
    State(first.Call("inspect", Args(small_size)), "unavailable", 0);
    for (const auto* method : {"activate", "install", "launch", "delete", "setPublisher"}) {
      const auto unknown = first.Call(method, Args(Stage(bytes, digest)));
      Check(unknown.unimplemented == 1 && !unknown.errors && !unknown.successes, "unknown operations unavailable");
    }
    Check(!fs::exists(fixture.Root(L"never-created")), "missing trust and rejected input have no filesystem effects");
    no_pin.Attach(&second);
    Check(!first.handler_ && static_cast<bool>(second.handler_), "reattach clears old callback");
    no_pin.Detach(); no_pin.Detach();
    Check(!second.handler_, "detach idempotent");
    no_pin.Attach(&first);
  }
  Check(!first.handler_, "destructor removes channel callback");
}
#ifdef RYHZE_UPDATES_NATIVE_TEST
struct Gate {
  HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  HANDLE release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  Gate() { Check(entered && release, "worker gate events"); }
  ~Gate() { SetEvent(release); CloseHandle(entered); CloseHandle(release); }
};
struct PollWindow {
  HostUpdates* host = nullptr;
  HWND value = nullptr;
  static LRESULT CALLBACK Dispatch(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
      auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
      SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* owner = reinterpret_cast<PollWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (owner && owner->host) {
      if (auto result = owner->host->HandleMessage(message, wparam, lparam)) return *result;
    }
    return DefWindowProcW(window, message, wparam, lparam);
  }
  PollWindow() {
    WNDCLASSW type{};
    type.lpfnWndProc = Dispatch;
    type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"RyhzeUpdateSettlementTests";
    Check(RegisterClassW(&type) || GetLastError() == ERROR_CLASS_ALREADY_EXISTS, "test window class");
    value = CreateWindowExW(0, type.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, type.hInstance, this);
    Check(value != nullptr, "real timer dispatch window");
  }
  ~PollWindow() { host = nullptr; if (value) DestroyWindow(value); }
};
void PumpUntil(const std::function<bool()>& done) {
  const auto deadline = GetTickCount64() + 5000;
  while (!done() && GetTickCount64() < deadline) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message); DispatchMessageW(&message);
    }
    Sleep(1);
  }
  Check(done(), "actual native worker settled within test bound");
}
void SettlementOwnership() {
  Fixture fixture;
  const std::vector<uint8_t> bytes = {1, 2, 3};
  const auto digest = Digest(bytes);
  UpdateDigest pin{}; pin[0] = 1; // Never reached by cancelled gated workers.
  for (const auto* reason : {"cancel", "timeout", "close", "reattach"}) {
    Gate gate;
    PollWindow window;
    Messenger first, second;
    HostUpdates host(fixture.Root(L"cancelled-before-io").wstring(), pin);
    window.host = &host;
    host.SetWorkerGateForTesting(gate.entered, gate.release);
    host.Attach(&first, window.value);
    auto work = first.CallAsync("stage", Args(Stage(bytes, digest)));
    Check(WaitForSingleObject(gate.entered, 5000) == WAIT_OBJECT_0, "worker entered real OS thread");
    auto drain = first.CallAsync("drain");
    Check(drain->replies == 0 && work->replies == 0, "drain never substitutes request completion");
    State(first.Call("inspect", Args(Inspect(digest, 3))), "conflict", 0);
    std::shared_ptr<Messenger::Reply> close;
    if (std::string(reason) == "timeout") {
      host.ExpireForTesting(); host.HandleMessage(HostUpdates::kMessage, 0, 0);
      Check(work->errors == 1 && work->code == "recovery", "timeout remains unknown completion");
    } else if (std::string(reason) == "close") {
      close = first.CallAsync("close");
      Check(close->replies == 0, "close waits for actual worker");
      Probe(first.Call("probe"), false);
      State(first.Call("inspect", Args(Inspect(digest, 3))), "unavailable", 0);
    } else if (std::string(reason) == "reattach") {
      host.Detach();
      Check(drain->errors == 1 && drain->code == "cancelled", "detach does not claim settlement");
      host.Attach(&second, window.value);
      drain = second.CallAsync("drain");
      Check(drain->replies == 0, "reattached drain retains old worker");
    } else {
      Empty(first.Call("cancel"));
      Empty(first.Call("cancel"));
    }
    if (std::string(reason) != "timeout")
      Check(work->errors == 1 && work->code == "cancelled", "one cancellation response");
    Check(drain->replies == 0, "cancellation/timeout is not settlement");
    // No synthetic HandleMessage polling after release: the replacement timer
    // must observe even a cancelled worker that exits without posting a message.
    SetEvent(gate.release);
    PumpUntil([&] { return drain->replies != 0 && (!close || close->replies != 0); });
    Empty(*drain);
    if (close) Empty(*close);
    Check(work->replies == 1, "late worker exit cannot reply twice");
    Check(!fs::exists(fixture.Root(L"cancelled-before-io")), "cancelled preflight makes no files");
    host.Detach(); window.host = nullptr;
  }
  {
    Gate gate;
    PollWindow window;
    Messenger messenger;
    HostUpdates host(fixture.Root(L"bounded-drain").wstring(), pin);
    window.host = &host;
    host.SetWorkerGateForTesting(gate.entered, gate.release);
    host.Attach(&messenger, window.value);
    auto work = messenger.CallAsync("stage", Args(Stage(bytes, digest)));
    Check(WaitForSingleObject(gate.entered, 5000) == WAIT_OBJECT_0, "bounded drain worker entered");
    std::vector<std::shared_ptr<Messenger::Reply>> drains;
    for (int i = 0; i < 8; ++i) drains.push_back(messenger.CallAsync("drain"));
    auto overflow = messenger.CallAsync("drain");
    Check(overflow->errors == 1 && overflow->code == "conflict", "bounded outstanding drains");
    Empty(messenger.Call("cancel"));
    SetEvent(gate.release);
    PumpUntil([&] { return drains.back()->replies != 0; });
    for (const auto& drain : drains) Empty(*drain);
    Check(work->replies == 1, "bounded worker completion replied once");
    host.Detach(); window.host = nullptr;
  }
}
#endif
void ConfiguredSignedFixture(const fs::path& external, const UpdateDigest& pin) {
  Fixture fixture;
  struct TestWindow {
    HWND value = CreateWindowExW(0, L"STATIC", L"update worker tests", 0,
        0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    ~TestWindow() { if (value) DestroyWindow(value); }
  } window;
  Check(window.value != nullptr, "message-only update completion window");
  const auto original = Read(external);
  const auto digest = Digest(original);
  const int64_t size = static_cast<int64_t>(original.size());
  Messenger messenger;
  {
    HostUpdates bridge(fixture.Root(L"stage").wstring(), pin);
    messenger.Bind(bridge, window.value);
    Probe(messenger.Call("probe"), true);
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "unavailable", 0);
    auto wrong_digest = digest; wrong_digest[0] ^= 1;
    State(messenger.Call("stage", Args(Stage(original, wrong_digest))), "invalid", 0);
    Check(!fs::exists(fixture.Root(L"stage")), "bridge hash failure before writes");
    State(messenger.Call("stage", Args(Stage(original, digest))), "staged", size);
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "staged", size);
    auto extra = Stage(original, digest);
    extra.emplace(EncodableValue("publisher"), EncodableValue(std::vector<uint8_t>(32)));
    Error(messenger.Call("stage", Args(extra)));
    State(messenger.Call("stage", Args(Stage(original, digest))), "staged", size);
    State(messenger.Call("inspect", Args(Inspect(digest, size + 1))), "recovery", size);
    const std::vector<uint8_t> unsigned_bytes = {1, 2, 3, 4};
    State(messenger.Call("stage", Args(Stage(unsigned_bytes, Digest(unsigned_bytes)))), "untrusted", 4);
    ryhze::secure_files::Error error;
    auto root = ryhze::secure_files::Root::Open(fixture.Root(L"stage").wstring(), false, &error);
    Check(root.has_value(), "bridge fixture root");
    auto lease = root->Lock(L"stage.lease", &error);
    Check(lease.has_value(), "bridge conflict fixture lease");
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "conflict", 0);
  }
  {
    auto wrong_pin = pin; wrong_pin[0] ^= 1;
    HostUpdates wrong_publisher(fixture.Root(L"stage").wstring(), wrong_pin);
    messenger.Bind(wrong_publisher, window.value);
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "untrusted", size);
  }
  {
    HostUpdates reopened(fixture.Root(L"stage").wstring(), pin);
    messenger.Bind(reopened, window.value);
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "staged", size);
    auto damaged = original;
    Check(damaged.size() > 4096, "signed fixture expected PE bounds");
    damaged[4096] ^= 0x40;
    Put(fixture.Root(L"stage") / Name(digest), damaged);
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "recovery", size);
    State(messenger.Call("stage", Args(Stage(original, digest))), "recovery", size);
    Check(Read(fixture.Root(L"stage") / Name(digest)) == damaged, "bridge never overwrites unknown changed file");
  }
  Check(Read(external) == original, "explicit external signed fixture unchanged");
  {
    HostUpdates async(fixture.Root(L"async").wstring(), pin);
    messenger.Bind(async, window.value);
    const auto request = messenger.CallAsync("stage", Args(Stage(original, digest)));
    Check(request->replies == 0, "configured stage never completes inline on UI thread");
    State(messenger.Call("inspect", Args(Inspect(digest, size))), "conflict", 0);
    Probe(messenger.Call("probe"), true);
    // A forged wakeup has no completion authority. It only checks owned state.
    async.HandleMessage(HostUpdates::kMessage, 0x1234, 0x5678);
    const ULONGLONG deadline = GetTickCount64() + 15000;
    while (!request->replies && GetTickCount64() < deadline) {
      async.HandleMessage(HostUpdates::kMessage, 0, 0);
      Sleep(1);
    }
    State(*request, "staged", size);
    const auto detached = messenger.CallAsync("inspect", Args(Inspect(digest, size)));
    const ULONGLONG started = GetTickCount64();
    async.Detach();
    Check(GetTickCount64() - started < 1000, "detach has bounded worker wait");
    Check(detached->replies == 1 && detached->errors == 1 && detached->code == "cancelled",
          "detach releases the outstanding platform reply exactly once");
    async.HandleMessage(HostUpdates::kMessage, 0, 0);
    Check(detached->replies == 1, "late completion cannot reply after detach");
    Check(!messenger.handler_, "detach unregisters channel");
    messenger.Bind(async, window.value);
    Empty(messenger.Call("drain"));
    async.Detach();
  }
}
}  // namespace
int wmain(int argc, wchar_t** argv) {
  try {
    ClosedSchemaAndNoPin();
#ifdef RYHZE_UPDATES_NATIVE_TEST
    SettlementOwnership();
#endif
    const auto path = argc == 3 ? std::wstring(argv[1]) : Environment(L"RYHZE_UPDATE_TEST_SIGNED_FILE");
    const auto pin = argc == 3 ? std::wstring(argv[2]) : Environment(L"RYHZE_UPDATE_TEST_PUBLISHER_SHA256");
    if (path.empty() || pin.empty()) {
      std::cerr << "Update bridge schema checks passed; configured signed acceptance NOT RUN: explicit fixture/pin required.\n";
      return 2;
    }
    ConfiguredSignedFixture(path, Pin(pin));
    std::cout << "PASS: update bridge real-codec schema and signed-stager suites; no activation\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
