#include "host_lifecycle.h"

#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>

#include <iostream>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;

void Check(bool condition, const char* description) {
  if (!condition) throw std::runtime_error(description);
}

class Messenger final : public flutter::BinaryMessenger {
 public:
  void Send(const std::string& channel, const uint8_t* data, size_t size,
            flutter::BinaryReply reply) const override {
    Check(channel == HostLifecycle::kChannel, "event channel");
    Check(!reply, "events must not await Dart");
    auto call = flutter::StandardMethodCodec::GetInstance().DecodeMethodCall(
        data, size);
    Check(call && call->method_name() == "lifecycle", "event method");
    Check(call->arguments() != nullptr, "event arguments");
    events.push_back(std::get<EncodableMap>(*call->arguments()));
  }
  void SetMessageHandler(const std::string& channel,
                         flutter::BinaryMessageHandler handler) override {
    Check(channel == HostLifecycle::kChannel, "handler channel");
    handler_ = std::move(handler);
  }

  struct Reply {
    EncodableValue value;
    std::string error;
    int successes = 0;
    int errors = 0;
    int unimplemented = 0;
    int replies = 0;
  };
  Reply Call(const std::string& method,
             std::unique_ptr<EncodableValue> args = nullptr) {
    Check(static_cast<bool>(handler_), "connected native handler");
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const auto encoded = codec.EncodeMethodCall(
        flutter::MethodCall<EncodableValue>(method, std::move(args)));
    Reply response;
    flutter::MethodResultFunctions<EncodableValue> result(
        [&](const EncodableValue* value) {
          ++response.successes;
          if (value) response.value = *value;
        },
        [&](const std::string& code, const std::string&,
            const EncodableValue*) {
          ++response.errors;
          response.error = code;
        },
        [&]() { ++response.unimplemented; });
    handler_(encoded->data(), encoded->size(),
             [&](const uint8_t* data, size_t size) {
               ++response.replies;
               if (size == 0) {
                 result.NotImplemented();
               } else {
                 Check(codec.DecodeAndProcessResponseEnvelope(data, size,
                                                              &result),
                       "valid response envelope");
               }
             });
    Check(response.replies == 1, "exactly one reply");
    return response;
  }

  flutter::BinaryMessageHandler handler_;
  mutable std::vector<EncodableMap> events;
};

void State(const EncodableMap& map, int64_t sequence, const char* state,
           bool snapshot = false) {
  Check(map.size() == (snapshot ? 4u : 2u), "exact schema key count");
  Check(std::get<int64_t>(map.at(EncodableValue("sequence"))) == sequence,
        "sequence");
  Check(std::get<std::string>(map.at(EncodableValue("state"))) == state,
        "state");
  if (snapshot) {
    Check(std::get<int32_t>(map.at(EncodableValue("schemaVersion"))) == 1,
          "schema version");
    Check(std::get<bool>(map.at(EncodableValue("powerEvents"))),
          "power capability only");
  }
}

void TestConnectionAndSchema() {
  Messenger messenger;
  {
    HostLifecycle bridge;
    bridge.Attach(&messenger);
    auto invalid = messenger.Call(
        "connect", std::make_unique<EncodableValue>(EncodableMap{}));
    Check(invalid.errors == 1 && invalid.error == "invalid-arguments",
          "reject connect payload");
    Check(messenger.Call("exit").unimplemented == 1, "reject other commands");
    bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
    Check(messenger.events.empty(), "invalid connect cannot enable events");
    auto connected = messenger.Call("connect");
    Check(connected.successes == 1, "connect success");
    State(std::get<EncodableMap>(connected.value), 1, "suspended", true);
    State(std::get<EncodableMap>(messenger.Call("connect").value), 1,
          "suspended", true);
    Check(messenger.events.empty(), "connect never emits replay event");
  }
  Check(!messenger.handler_, "destructor unregisters handler");
}

void TestPowerEvents() {
  Messenger messenger;
  HostLifecycle bridge;
  bridge.Attach(&messenger);
  State(std::get<EncodableMap>(messenger.Call("connect").value), 0, "active",
        true);
  Check(bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0) == TRUE,
        "suspend handled");
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  Check(messenger.events.size() == 1, "duplicate suspend coalesced");
  State(messenger.events.back(), 1, "suspended");
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMECRITICAL, 0);
  Check(messenger.events.size() == 2, "resume variants coalesced");
  State(messenger.events.back(), 2, "resumed");
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMESUSPEND, 0);
  State(messenger.events.back(), 4, "resumed");
  Check(!bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMPOWERSTATUSCHANGE, 0),
        "unrelated power event remains unhandled");
  Check(messenger.events.size() == 4, "unrelated event cannot change state");
}

void TestRetentionAndReconnect() {
  Messenger messenger;
  HostLifecycle bridge;
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
  bridge.Attach(&messenger);
  Check(messenger.events.empty(), "pre-attach events retained only");
  State(std::get<EncodableMap>(messenger.Call("connect").value), 2, "resumed",
        true);
  bridge.Detach();
  Check(!messenger.handler_, "detach unregisters handler");
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMSUSPEND, 0);
  bridge.Attach(&messenger);
  State(std::get<EncodableMap>(messenger.Call("connect").value), 3, "suspended",
        true);
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
  Check(messenger.events.size() == 1, "one event after reconnect");
  State(messenger.events.back(), 4, "resumed");
}

void TestShutdownAndFlutterMessages() {
  Messenger messenger;
  HostLifecycle bridge;
  bridge.Attach(&messenger);
  messenger.Call("connect");
  Check(bridge.HandleMessage(WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF) == TRUE,
        "query immediately allows shutdown");
  Check(messenger.events.empty(), "query cannot claim ending");
  Check(bridge.HandleMessage(WM_ENDSESSION, FALSE, 0) == 0,
        "canceled shutdown handled");
  State(messenger.events.back(), 1, "resumed");
  bridge.HandleMessage(WM_ENDSESSION, TRUE,
                       ENDSESSION_LOGOFF | ENDSESSION_CRITICAL);
  bridge.HandleMessage(WM_ENDSESSION, TRUE, 0);
  Check(messenger.events.size() == 2, "confirmed ending coalesced");
  State(messenger.events.back(), 2, "ending");
  for (UINT message : {WM_CLOSE, WM_SIZE, WM_ACTIVATE, WM_SHOWWINDOW}) {
    Check(!bridge.HandleMessage(message, 0, 0),
          "Flutter close/visibility/focus unchanged");
  }
  Check(messenger.events.size() == 2, "Flutter messages emit no power state");
}

void TestMissedSuspendAndEarlyEnding() {
  Messenger messenger;
  HostLifecycle bridge;
  // Windows can resume without this process observing the suspend callback.
  bridge.HandleMessage(WM_POWERBROADCAST, PBT_APMRESUMEAUTOMATIC, 0);
  bridge.Attach(&messenger);
  State(std::get<EncodableMap>(messenger.Call("connect").value), 1, "resumed",
        true);
  bridge.Detach();
  bridge.HandleMessage(WM_ENDSESSION, TRUE, 0);
  bridge.Attach(&messenger);
  State(std::get<EncodableMap>(messenger.Call("connect").value), 2, "ending",
        true);
  Check(messenger.events.empty(), "early ending delivered through snapshot");
}
}  // namespace

int main() {
  try {
    TestConnectionAndSchema();
    TestPowerEvents();
    TestRetentionAndReconnect();
    TestShutdownAndFlutterMessages();
    TestMissedSuspendAndEarlyEnding();
    std::cout << "PASS: 5 native lifecycle suites (state, codec, mapping, lifetime, startup)\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
