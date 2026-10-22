#include "host_lifecycle.h"

#include <flutter/standard_method_codec.h>

#include <limits>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
}  // namespace

HostLifecycle::~HostLifecycle() {
  Detach();
}

void HostLifecycle::Attach(flutter::BinaryMessenger* messenger) {
  Detach();
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(
      messenger, kChannel, &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler(
      [this](const flutter::MethodCall<EncodableValue>& call,
             std::unique_ptr<flutter::MethodResult<EncodableValue>> result) {
        if (call.method_name() != "connect") {
          result->NotImplemented();
          return;
        }
        if (call.arguments() && !call.arguments()->IsNull()) {
          result->Error("invalid-arguments", "connect takes no arguments");
          return;
        }
        // Native window messages and this handler share the window thread.
        // The returned snapshot catches events before Dart installed its handler.
        connected_ = true;
        result->Success(EncodableValue(Snapshot()));
      });
}

void HostLifecycle::Detach() {
  connected_ = false;
  if (channel_) {
    channel_->SetMethodCallHandler(nullptr);
    channel_.reset();
  }
}

EncodableMap HostLifecycle::Snapshot() const {
  return {{EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("powerEvents"), EncodableValue(true)},
          {EncodableValue("sequence"), EncodableValue(sequence_)},
          {EncodableValue("state"), EncodableValue(state_)}};
}

void HostLifecycle::Transition(const char* state) {
  if (state_ == state || sequence_ == std::numeric_limits<int64_t>::max()) {
    return;
  }
  state_ = state;
  ++sequence_;
  if (channel_ && connected_) {
    channel_->InvokeMethod(
        "lifecycle", std::make_unique<EncodableValue>(EncodableMap{
                         {EncodableValue("sequence"), EncodableValue(sequence_)},
                         {EncodableValue("state"), EncodableValue(state_)}}));
  }
}

std::optional<LRESULT> HostLifecycle::HandleMessage(UINT message, WPARAM wparam,
                                                   LPARAM lparam) {
  (void)lparam;
  switch (message) {
    case WM_POWERBROADCAST:
      switch (wparam) {
        case PBT_APMSUSPEND:
          Transition("suspended");
          return TRUE;
        case PBT_APMRESUMEAUTOMATIC:
        case PBT_APMRESUMESUSPEND:
        case PBT_APMRESUMECRITICAL:
          // Automatic resume can be followed by a user-present resume. Both
          // describe one interruption and must not repeat reconciliation.
          Transition("resumed");
          return TRUE;
      }
      return std::nullopt;
    case WM_QUERYENDSESSION:
      // A query may be canceled. Never destroy state or wait for Dart here.
      return TRUE;
    case WM_ENDSESSION:
      Transition(wparam ? "ending" : "resumed");
      return 0;
    default:
      return std::nullopt;
  }
}
