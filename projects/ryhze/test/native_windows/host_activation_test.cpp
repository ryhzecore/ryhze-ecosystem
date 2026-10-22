#include "host_activation.h"

#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableList;
using flutter::EncodableMap;
using flutter::EncodableValue;

void Check(bool condition, const char* description) {
  if (!condition) throw std::runtime_error(description);
}

class Handle {
 public:
  explicit Handle(HANDLE value = nullptr) : value_(value) {}
  ~Handle() {
    if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
  }
  Handle(const Handle&) = delete;
  Handle& operator=(const Handle&) = delete;
  HANDLE get() const { return value_; }
  explicit operator bool() const {
    return value_ && value_ != INVALID_HANDLE_VALUE;
  }
 private:
  HANDLE value_;
};

std::wstring Wide(const std::string& text) {
  return std::wstring(text.begin(), text.end());
}

std::string Narrow(const std::wstring& text) {
  std::string result;
  for (wchar_t c : text) {
    Check(c < 128, "ASCII test argument");
    result.push_back(static_cast<char>(c));
  }
  return result;
}

std::wstring Executable() {
  std::vector<wchar_t> path(32768);
  const DWORD length = GetModuleFileNameW(nullptr, path.data(),
                                         static_cast<DWORD>(path.size()));
  Check(length > 0 && length < path.size(), "test executable path");
  return std::wstring(path.data(), length);
}

// CreateProcess argument quoting, without a shell or command substitution.
std::wstring Quote(const std::wstring& text) {
  std::wstring result = L"\"";
  size_t slashes = 0;
  for (wchar_t c : text) {
    if (c == L'\\') { ++slashes; continue; }
    result.append(c == L'\"' ? slashes * 2 + 1 : slashes, L'\\');
    result.push_back(c);
    slashes = 0;
  }
  result.append(slashes * 2, L'\\');
  result.push_back(L'\"');
  return result;
}

class Child {
 public:
  Child(const std::wstring& executable, const std::vector<std::wstring>& args) {
    std::wstring command = Quote(executable);
    for (const auto& arg : args) command += L" " + Quote(arg);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    Check(CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
                          FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                          &process_) != 0, "create owned test child");
    CloseHandle(process_.hThread);
    process_.hThread = nullptr;
  }
  ~Child() {
    if (!process_.hProcess) return;
    if (WaitForSingleObject(process_.hProcess, 0) == WAIT_TIMEOUT) {
      // Only this harness's exact newly-created process handle is terminated.
      TerminateProcess(process_.hProcess, 99);
      WaitForSingleObject(process_.hProcess, 2000);
    }
    CloseHandle(process_.hProcess);
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
  DWORD Wait() {
    Check(WaitForSingleObject(process_.hProcess, 7000) == WAIT_OBJECT_0,
          "owned child completes within deadline");
    DWORD code = 0;
    Check(GetExitCodeProcess(process_.hProcess, &code) != 0, "child exit code");
    return code;
  }
 private:
  PROCESS_INFORMATION process_{};
};

DWORD Run(const std::vector<std::wstring>& args,
          const std::wstring& executable = Executable()) {
  Child child(executable, args);
  return child.Wait();
}

class CopyOfExecutable {
 public:
  CopyOfExecutable() {
    std::array<wchar_t, MAX_PATH + 1> temp{}, leaf{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(temp.size()), temp.data());
    Check(length > 0 && length < temp.size(), "temporary directory");
    Check(GetTempFileNameW(temp.data(), L"rya", 0, leaf.data()) != 0,
          "unique temporary test directory name");
    Check(DeleteFileW(leaf.data()) != 0 &&
          CreateDirectoryW(leaf.data(), nullptr) != 0, "create test directory");
    directory_ = leaf.data();
    path_ = directory_ + L"\\activation child.exe";
    Check(CopyFileW(Executable().c_str(), path_.c_str(), TRUE) != 0,
          "copy only the harness executable");
  }
  ~CopyOfExecutable() {
    // Exact files created by this fixture only; never recurse through TEMP.
    DeleteFileW(path_.c_str());
    RemoveDirectoryW(directory_.c_str());
  }
  const std::wstring& path() const { return path_; }
 private:
  std::wstring directory_, path_;
};

class Window {
 public:
  Window() : value_(CreateWindowExW(0, L"STATIC", L"activation test", 0,
                                    0, 0, 0, 0, HWND_MESSAGE, nullptr,
                                    GetModuleHandleW(nullptr), nullptr)) {
    Check(value_ != nullptr, "message-only test window");
  }
  ~Window() { DestroyWindow(value_); }
  HWND get() const { return value_; }
 private:
  HWND value_;
};

class Messenger final : public flutter::BinaryMessenger {
 public:
  void Send(const std::string& channel, const uint8_t* data, size_t size,
            flutter::BinaryReply reply) const override {
    Check(channel == HostActivation::kChannel && !reply, "one-way activation");
    auto call = flutter::StandardMethodCodec::GetInstance().DecodeMethodCall(data, size);
    Check(call && call->method_name() == "activate" && call->arguments(),
          "activation method schema");
    events.push_back(std::get<EncodableMap>(*call->arguments()));
  }
  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    Check(channel == HostActivation::kChannel, "activation handler channel");
    handler_ = std::move(handler);
  }
  struct Reply {
    EncodableValue value;
    int successes = 0, errors = 0, unimplemented = 0, replies = 0;
  };
  Reply Call(const std::string& method,
             std::unique_ptr<EncodableValue> args = nullptr) {
    Check(static_cast<bool>(handler_), "attached activation handler");
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const auto encoded = codec.EncodeMethodCall(
        flutter::MethodCall<EncodableValue>(method, std::move(args)));
    Reply response;
    flutter::MethodResultFunctions<EncodableValue> result(
        [&](const EncodableValue* value) {
          ++response.successes;
          if (value) response.value = *value;
        },
        [&](const std::string&, const std::string&, const EncodableValue*) {
          ++response.errors;
        }, [&]() { ++response.unimplemented; });
    handler_(encoded->data(), encoded->size(), [&](const uint8_t* data, size_t size) {
      ++response.replies;
      if (size == 0) result.NotImplemented();
      else Check(codec.DecodeAndProcessResponseEnvelope(data, size, &result),
                 "valid activation response envelope");
    });
    Check(response.replies == 1, "exactly one activation reply");
    return response;
  }
  EncodableList Connect() {
    const auto reply = Call("connect");
    Check(reply.successes == 1, "activation connect success");
    const auto& map = std::get<EncodableMap>(reply.value);
    Check(map.size() == 3 &&
          std::get<int32_t>(map.at(EncodableValue("schemaVersion"))) == 1 &&
          std::get<bool>(map.at(EncodableValue("singleInstance"))),
          "exact activation capability schema");
    return std::get<EncodableList>(map.at(EncodableValue("activations")));
  }
  Reply Ack(int64_t sequence) {
    return Call("ack", std::make_unique<EncodableValue>(EncodableMap{
        {EncodableValue("sequence"), EncodableValue(sequence)}}));
  }
  flutter::BinaryMessageHandler handler_;
  mutable std::vector<EncodableMap> events;
};

void Activation(const EncodableValue& value, int64_t sequence,
                const std::optional<std::string>& uri) {
  const auto& map = std::get<EncodableMap>(value);
  Check(map.size() == 2 &&
        std::get<int64_t>(map.at(EncodableValue("sequence"))) == sequence,
        "exact activation sequence schema");
  const auto& actual = map.at(EncodableValue("uri"));
  Check(uri ? std::get<std::string>(actual) == *uri : actual.IsNull(),
        "activation URI or focus-only null");
}

Handle* OpenRaw(const std::wstring& name) {
  const ULONGLONG deadline = GetTickCount64() + 3000;
  do {
    HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                             nullptr, OPEN_EXISTING, SECURITY_SQOS_PRESENT |
                                 SECURITY_IDENTIFICATION, nullptr);
    if (pipe != INVALID_HANDLE_VALUE) return new Handle(pipe);
    const DWORD error = GetLastError();
    Check(error == ERROR_PIPE_BUSY || error == ERROR_FILE_NOT_FOUND,
          "raw child pipe open");
    WaitNamedPipeW(name.c_str(), 25);
    if (error == ERROR_FILE_NOT_FOUND) Sleep(10);
  } while (GetTickCount64() < deadline);
  throw std::runtime_error("raw child connection deadline");
}

// These synchronous calls only run in a child with an external 7-second bound.
// No unbounded pipe operation runs on the harness's parent/window thread.
int RawChild(const std::wstring& endpoint, const std::string& mode,
             uint8_t nonce, const std::string& uri) {
  std::unique_ptr<Handle> pipe(OpenRaw(endpoint));
  std::vector<uint8_t> request(24 + uri.size());
  request[0] = 'R'; request[1] = 'Y'; request[2] = 'A'; request[3] = 1;
  const uint32_t length = mode == "oversize" ? 4097 : static_cast<uint32_t>(uri.size());
  std::memcpy(request.data() + 4, &length, sizeof(length));
  request[8] = mode == "zero-nonce" ? 0 : nonce;
  if (mode == "version") request[3] = 2;
  std::copy(uri.begin(), uri.end(), request.begin() + 24);
  DWORD bytes = 0;
  Check(WriteFile(pipe->get(), request.data(), static_cast<DWORD>(request.size()),
                  &bytes, nullptr) && bytes == request.size(), "raw child request");
  uint8_t reply = 0;
  Check(ReadFile(pipe->get(), &reply, 1, &bytes, nullptr) && bytes == 1,
        "raw child response");
  WriteFile(pipe->get(), &reply, 1, &bytes, nullptr);
  return static_cast<int>(reply);
}

std::wstring EventName() {
  static unsigned counter = 0;
  return L"Local\\Ryhze.Activation.Test." + std::to_wstring(GetCurrentProcessId()) +
      L"." + std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++counter);
}

void SignalReady(const std::wstring& name) {
  Handle event(OpenEventW(EVENT_MODIFY_STATE, FALSE, name.c_str()));
  Check(event && SetEvent(event.get()), "child readiness event");
}

int FakeServerChild(const std::wstring& endpoint, const std::wstring& ready) {
  constexpr wchar_t prefix[] = L"\\\\.\\pipe\\";
  Check(endpoint.rfind(prefix, 0) == 0, "local pipe endpoint");
  const std::wstring mutex_name = L"Local\\" + endpoint.substr(std::size(prefix) - 1);
  Handle mutex(CreateMutexW(nullptr, FALSE, mutex_name.c_str()));
  Handle pipe(CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX |
      FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_WAIT |
      PIPE_REJECT_REMOTE_CLIENTS, 1, 8192, 8192, 2000, nullptr));
  Check(mutex && pipe, "fake server owns endpoint");
  SignalReady(ready);
  Check(ConnectNamedPipe(pipe.get(), nullptr) || GetLastError() == ERROR_PIPE_CONNECTED,
        "fake server client connected");
  uint8_t first = 0;
  DWORD count = 0;
  const BOOL read = ReadFile(pipe.get(), &first, 1, &count, nullptr);
  Check(!read && count == 0, "client sends no URI bytes to wrong executable");
  return 0;
}

void TestUriGate() {
  const std::pair<std::string, std::string> valid[] = {
    {"ryhze://app/activity", "ryhze://app/activity"},
    {"RYHZE://APP/activity", "ryhze://app/activity"},
    {"ryhze://app/games/store", "ryhze://app/games/store"},
    {"RYHZE://APP/films/store?item=Film_1.2-x", "ryhze://app/films/store?item=Film_1.2-x"},
    {"ryhze://app/games/library", "ryhze://app/games/library"},
    {"ryhze://app/games/library?provider=steam&item=%34%32", "ryhze://app/games/library?item=42&provider=steam"},
  };
  for (const auto& entry : valid)
    Check(HostActivation::CanonicalizeUri(entry.first) == entry.second, "valid URI");
  const std::vector<std::string> invalid = {
    "", "https://app/games/store", "ryhze://other/games/store",
    "ryhze://app/activity?", "ryhze://app/activity#", "ryhze://app/activity?item=x", "ryhze://app/activity?provider=x",
    "ryhze://app/activity?execute=x", "ryhze://app/activity/other",
    "ryhze://app:99/games/store", "ryhze://user@app/games/store",
    "ryhze://app/games/store/", "ryhze://app/Games/store", "ryhze://app/install",
    "ryhze://app/games/store#", "ryhze://app/games/store?",
    "ryhze://app/games/store?item=x&", "ryhze://app/games/store?item=x&item=y",
    "ryhze://app/games/store?item=", "ryhze://app/games/store?item=.x",
    "ryhze://app/games/store?item=x%2Fy", "ryhze://app/games/store?item=x%00",
    "ryhze://app/games/store?item=x+y", "ryhze://app/games/store?item=x=y",
    "ryhze://app/games/store?item=%GG", "ryhze://app/games/store?execute=x",
    "ryhze://app/games/store?provider=steam", "ryhze://app/games/library?item=x",
    "ryhze://app/games/library?provider=steam", "ryhze://app/films/store?provider=x&item=y",
    "ryhze://app/games\\store", "ryhze://app/games/store?item=" + std::string(129, 'a'),
    "ryhze://app/games/store?item=" + std::string(4096, 'a'),
  };
  for (const auto& uri : invalid) {
    Check(!HostActivation::CanonicalizeUri(uri), "reject invalid URI");
    HostActivation host;
    Check(host.Start(uri) == HostActivation::StartResult::invalidArguments,
          "invalid URI does not create transport");
    Check(host.pipe_name().empty(), "invalid URI has no endpoint");
  }
}

void TestSnapshotAndAcknowledgement() {
  Window window;
  Messenger messenger;
  HostActivation host;
  const std::string uri = "ryhze://app/games/store?item=42";
  Check(host.Start(uri) == HostActivation::StartResult::primary, "cold primary");
  int focuses = 0;
  host.Attach(&messenger, window.get(), [&]() { ++focuses; });
  Check(messenger.Call("connect", std::make_unique<EncodableValue>(EncodableMap{})).errors == 1,
        "connect rejects non-null arguments");
  Check(messenger.Ack(1).errors == 1, "ack before connect rejected");
  Check(messenger.Call("execute").unimplemented == 1, "no command dispatch");
  auto values = messenger.Connect();
  Check(values.size() == 1, "one cold activation");
  Activation(values.front(), 1, uri);
  host.HandleMessage(HostActivation::kMessage, 0x1234, 0x5678);
  Check(messenger.events.empty() && focuses == 0, "forged wakeup adds no action");
  Check(!host.HandleMessage(WM_CLOSE, 0, 0), "Flutter close remains unhandled");
  Check(messenger.Connect() == values, "unacknowledged snapshot retained");
  Check(messenger.Ack(0).errors == 1 && messenger.Ack(2).errors == 1,
        "ack must refer to delivered sequence");
  Check(messenger.Call("ack", std::make_unique<EncodableValue>(EncodableMap{
      {EncodableValue("sequence"), EncodableValue(int64_t{1})},
      {EncodableValue("extra"), EncodableValue(true)}})).errors == 1,
      "ack exact schema");
  host.Detach();
  Check(!messenger.handler_, "detach removes handler");
  host.Attach(&messenger, window.get(), []() {});
  Check(messenger.Connect() == values, "reattach retains unacknowledged sequence");
  Check(messenger.Ack(1).successes == 1 && messenger.Connect().empty(),
        "ack retires snapshot");
}

void TestActivityTransport() {
  Window window;
  Messenger messenger;
  HostActivation host;
  const std::string uri = "ryhze://app/activity";
  Check(host.Start(uri) == HostActivation::StartResult::primary, "Activity cold primary");
  host.Attach(&messenger, window.get(), []() {});
  const auto snapshot = messenger.Connect();
  Check(snapshot.size() == 1, "Activity cold snapshot retained");
  Activation(snapshot.front(), 1, uri);
  Check(messenger.Ack(1).successes == 1, "Activity cold acknowledgement");
  Check(Run({L"--client", Wide(uri)}) == 21, "Activity forwarded by real same-executable child");
  host.HandleMessage(HostActivation::kMessage, 0, 0);
  Check(messenger.events.size() == 1, "Activity warm platform-thread delivery");
  Activation(EncodableValue(messenger.events.back()), 2, uri);
  Check(messenger.Ack(2).successes == 1, "Activity warm acknowledgement");
}

void TestRealForwardingAndReplay() {
  Window window;
  Messenger messenger;
  HostActivation host;
  Check(host.Start() == HostActivation::StartResult::primary, "warm primary");
  int focuses = 0;
  host.Attach(&messenger, window.get(), [&]() { ++focuses; });
  Check(messenger.Connect().empty(), "normal cold launch has no activation");
  const std::string uri = "ryhze://app/games/library?item=42&provider=steam";
  Check(Run({L"--client", Wide(uri)}) == 21, "same executable child forwarded");
  Check(messenger.events.empty(), "worker never calls Flutter messenger");
  host.HandleMessage(HostActivation::kMessage, 0, 0);
  Check(messenger.events.size() == 1 && focuses == 1, "window thread delivers warm activation");
  Activation(EncodableValue(messenger.events.back()), 1, uri);
  Check(messenger.Ack(1).successes == 1, "warm acknowledgement");
  Check(Run({L"--client", L"--focus"}) == 21, "second normal launch forwards focus");
  host.HandleMessage(HostActivation::kMessage, 0, 0);
  Check(messenger.events.size() == 2 && focuses == 2, "focus delivery once");
  Activation(EncodableValue(messenger.events.back()), 2, std::nullopt);
  Check(messenger.Ack(2).successes == 1, "focus acknowledgement");
  const auto raw = [&](const std::string& mode, const std::string& value) {
    return Run({L"--raw", host.pipe_name(), Wide(mode), L"7", Wide(value)});
  };
  Check(raw("valid", uri) == 1 && raw("valid", uri) == 1, "retry same nonce accepted");
  Check(raw("valid", "ryhze://app/films/store") == 2, "nonce cannot change payload");
  auto values = messenger.Connect();
  Check(values.size() == 1, "nonce replay queues one activation");
  Activation(values.front(), 3, uri);
  Check(messenger.Ack(3).successes == 1, "replay ack");
  Check(raw("valid", uri) == 1 && messenger.Connect().empty(),
        "retry after ack remains deduplicated within replay window");
  Check(Run({L"--client", Wide(uri)}) == 21, "fresh nonce permits same URI again");
  values = messenger.Connect();
  Check(values.size() == 1, "fresh nonce creates new activation");
  Activation(values.front(), 4, uri);
}

void TestMalformedFramesAndQueueBound() {
  Window window;
  Messenger messenger;
  HostActivation host;
  Check(host.Start() == HostActivation::StartResult::primary, "bounded primary");
  host.Attach(&messenger, window.get(), []() {});
  for (const auto& mode : {L"version", L"oversize", L"zero-nonce"})
    Check(Run({L"--raw", host.pipe_name(), mode, L"9", L""}) == 2,
          "malformed frame rejected");
  Check(Run({L"--raw", host.pipe_name(), L"valid", L"9", L"ryhze://app/execute"}) == 2,
        "unknown route rejected in actual transport");
  Check(messenger.Connect().empty(), "malformed frames cannot enqueue");
  for (size_t i = 0; i < HostActivation::kQueueLimit; ++i)
    Check(Run({L"--client", L"ryhze://app/games/store"}) == 21, "fill bounded queue");
  Check(Run({L"--client", L"--focus"}) == 23, "full queue fails closed");
  const auto values = messenger.Connect();
  Check(values.size() == HostActivation::kQueueLimit, "queue exact capacity");
  for (size_t i = 0; i < values.size(); ++i)
    Activation(values[i], static_cast<int64_t>(i + 1), "ryhze://app/games/store");
  Check(messenger.Ack(static_cast<int64_t>(values.size())).successes == 1,
        "cumulative acknowledgement frees queue");
  Check(Run({L"--client", L"--focus"}) == 21, "queue accepts after acknowledgement");
  const auto next = messenger.Connect();
  Check(next.size() == 1, "queue contains only new event");
  Activation(next.front(), 17, std::nullopt);
}

void TestExecutableIsolationAndPeerValidation() {
  CopyOfExecutable copy;
  std::wstring endpoint;
  {
    Window window;
    Messenger messenger;
    HostActivation host;
    Check(host.Start() == HostActivation::StartResult::primary, "isolation primary");
    host.Attach(&messenger, window.get(), []() {});
    endpoint = host.pipe_name();
    Check(Run({L"--client", L"ryhze://app/games/store"}, copy.path()) == 20,
          "copied executable has an isolated primary endpoint");
    Check(Run({L"--raw", endpoint, L"valid", L"12", L"ryhze://app/games/store"}, copy.path()) == 2,
          "server rejects same-user peer from another executable");
    Check(messenger.Connect().empty(), "foreign executable cannot enqueue");
  }
  const std::wstring ready_name = EventName();
  Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
  Check(static_cast<bool>(ready), "fake server ready handle");
  Child server(copy.path(), {L"--fake-server", endpoint, ready_name});
  Check(WaitForSingleObject(ready.get(), 3000) == WAIT_OBJECT_0, "fake server ready");
  HostActivation secondary;
  Check(secondary.Start("ryhze://app/games/store?item=42") ==
            HostActivation::StartResult::unavailable,
        "client rejects wrong executable endpoint owner");
  Check(server.Wait() == 0, "wrong server received no URI bytes");
}

void TestShutdownWithPartialClient() {
  auto host = std::make_unique<HostActivation>();
  Check(host->Start() == HostActivation::StartResult::primary, "shutdown primary");
  const std::wstring ready_name = EventName();
  Handle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
  Check(static_cast<bool>(ready), "stall readiness handle");
  Child client(Executable(), {L"--stall", host->pipe_name(), ready_name});
  Check(WaitForSingleObject(ready.get(), 3000) == WAIT_OBJECT_0, "partial client connected");
  const ULONGLONG started = GetTickCount64();
  host.reset();
  Check(GetTickCount64() - started < 3500, "shutdown cancels partial request within bound");
  Check(client.Wait() == 0, "partial child completes");
  HostActivation restarted;
  Check(restarted.Start() == HostActivation::StartResult::primary,
        "shutdown releases endpoint and mutex");
}
}  // namespace

int wmain(int argc, wchar_t** argv) {
  try {
    if (argc >= 2) {
      const std::wstring mode = argv[1];
      if (mode == L"--client" && argc == 3) {
        HostActivation host;
        const std::optional<std::string> uri = std::wstring(argv[2]) == L"--focus"
            ? std::nullopt : std::optional<std::string>(Narrow(argv[2]));
        return 20 + static_cast<int>(host.Start(uri));
      }
      if (mode == L"--raw" && argc == 6)
        return RawChild(argv[2], Narrow(argv[3]),
                        static_cast<uint8_t>(std::stoul(argv[4])), Narrow(argv[5]));
      if (mode == L"--fake-server" && argc == 4)
        return FakeServerChild(argv[2], argv[3]);
      if (mode == L"--stall" && argc == 4) {
        std::unique_ptr<Handle> pipe(OpenRaw(argv[2]));
        uint8_t partial = 'R';
        DWORD bytes = 0;
        Check(WriteFile(pipe->get(), &partial, 1, &bytes, nullptr) && bytes == 1,
              "partial client writes one header byte");
        SignalReady(argv[3]);
        Sleep(1200);
        return 0;
      }
      throw std::runtime_error("invalid harness child arguments");
    }
    TestUriGate();
    TestSnapshotAndAcknowledgement();
    TestActivityTransport();
    TestRealForwardingAndReplay();
    TestMalformedFramesAndQueueBound();
    TestExecutableIsolationAndPeerValidation();
    TestShutdownWithPartialClient();
    std::cout << "PASS: 7 native activation suites (URI, codec/ack, Activity IPC, process IPC/replay, bounds, identity, shutdown)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 100;
  }
}
