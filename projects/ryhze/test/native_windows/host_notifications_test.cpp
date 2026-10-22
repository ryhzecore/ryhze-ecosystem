#include "host_notifications.h"
#include <roapi.h>
#include <winrt/Windows.Data.Xml.Dom.h>
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
class Backend final : public NotificationBackend {
 public:
  bool available = true, succeeds = true;
  int clears = 0;
  DWORD owner = GetCurrentThreadId();
  std::vector<std::wstring> tags, documents;
  bool Available() override { return available; }
  bool Submit(const std::wstring& tag, const std::wstring& xml) override {
    Check(GetCurrentThreadId() == owner, "notification uses platform thread");
    tags.push_back(tag); documents.push_back(xml); return succeeds;
  }
  bool Clear() override { ++clears; return available; }
};
class Messenger final : public flutter::BinaryMessenger {
 public:
  void Send(const std::string&, const uint8_t*, size_t, flutter::BinaryReply) const override {
    Check(false, "notifications cannot emit app commands");
  }
  void SetMessageHandler(const std::string& name, flutter::BinaryMessageHandler handler) override {
    Check(name == "com.ryhze.ryhze/windows.notifications.v1", "notification channel");
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
    Check(static_cast<bool>(handler_), "notifications attached");
    handler_(wire->data(), wire->size(), [&](const uint8_t* data, size_t size) {
      if (!size) decoder.NotImplemented();
      else Check(codec.DecodeAndProcessResponseEnvelope(data, size, &decoder), "real notification codec");
    });
    Check(reply.successes + reply.errors + reply.unknown == 1, "one notification reply");
    return reply;
  }
  static EncodableMap Args(const char* id = "fixture", const char* uri = "ryhze://app/games/store") {
    return {{EncodableValue("id"), EncodableValue(id)},
      {EncodableValue("kind"), EncodableValue("failed")},
      {EncodableValue("uri"), EncodableValue(uri)},
      {EncodableValue("private"), EncodableValue(true)}};
  }
  Reply Show(EncodableMap args = Args()) { return Call("show", std::make_unique<EncodableValue>(args)); }
  flutter::BinaryMessageHandler handler_;
};
void Result(const Messenger::Reply& reply, const char* expected) {
  Check(reply.successes == 1, "notification operation result");
  const auto& map = std::get<EncodableMap>(reply.value);
  Check(map.size() == 1 && std::get<std::string>(map.at(EncodableValue("state"))) == expected,
        "exact notification result schema");
}
void XmlAndPrivacy() {
  NotificationRequest request{"private-account-and-id", "handoff",
      "ryhze://app/games/library?item=title&provider=provider", true};
  Check(HostNotifications::Validate(request), "valid navigational request");
  const auto xml = HostNotifications::ToastXml(request);
  Check(xml.find(L"private-account") == std::wstring::npos &&
      xml.find(L"Ryhze activity is available.") != std::wstring::npos &&
      xml.find(L"handoff") == std::wstring::npos, "private toast excludes identifiers and outcome detail");
  winrt::Windows::Data::Xml::Dom::XmlDocument document;
  document.LoadXml(xml);
  const auto root = document.DocumentElement();
  Check(root.GetAttribute(L"activationType") == L"protocol" &&
      root.GetAttribute(L"launch") == L"ryhze://app/games/library?item=title&provider=provider",
      "actual Windows XML parser sees exact escaped navigation URI");
  Check(document.GetElementsByTagName(L"actions").Length() == 0,
        "no in-process or background toast commands");
  request.private_preview = false;
  Check(HostNotifications::ToastXml(request).find(L"Running state is not verified.") != std::wstring::npos,
        "handoff never claims observed running state");
  request.uri = "ryhze://app/activity";
  Check(HostNotifications::Validate(request), "Activity is a supported actionable surface");
  for (const auto* uri : {"ryhze://app/activity?item=x", "https://app/games/store", "ryhze://app/games/store?command=run",
      "ryhze://app/games/library?item=only", "ryhze://app/games/store?item=%22%3E"}) {
    request.uri = uri;
    Check(!HostNotifications::Validate(request) && HostNotifications::ToastXml(request).empty(),
          "unknown routes and commands cannot become actionable notifications");
  }
  request.uri = "ryhze://app/games/store";
  request.kind = "private freeform text";
  Check(!HostNotifications::Validate(request), "closed summary vocabulary");
  request.kind = "failed"; request.id = std::string(8193, 'a');
  Check(!HostNotifications::Validate(request), "bounded identifier");
  request.id = "line\nbreak";
  Check(!HostNotifications::Validate(request), "control characters refused");
  request.id = std::string(1, static_cast<char>(0xff));
  Check(!HostNotifications::Validate(request), "malformed UTF-8 refused");
  Check(HostNotifications::Tag("abc") == L"ungWv48Bz-pBQUDe" &&
      HostNotifications::Tag("abc").size() == 16 && HostNotifications::Tag("abc") != HostNotifications::Tag("abcd"),
      "real SHA256-derived tag is stable, bounded and payload-free");
}
void ProtocolAndBounds() {
  auto backend = std::make_unique<Backend>();
  auto* native = backend.get();
  Messenger messenger;
  ULONGLONG now = 100;
  HostNotifications host(std::move(backend), [&] { return now; });
  host.Attach(&messenger);
  const auto probe = messenger.Call("probe");
  const auto& map = std::get<EncodableMap>(probe.value);
  Check(map.size() == 2 && std::get<int32_t>(map.at(EncodableValue("schemaVersion"))) == 1 &&
      std::get<bool>(map.at(EncodableValue("notifications"))), "exact notification capability");
  Check(messenger.Call("probe", std::make_unique<EncodableValue>(EncodableMap{})).errors == 1,
        "probe has no arguments");
  Check(messenger.Call("show").errors == 1, "show requires arguments");
  auto invalid = Messenger::Args(); invalid.emplace(EncodableValue("title"), EncodableValue("untrusted"));
  Check(messenger.Show(invalid).errors == 1, "freeform title rejected");
  invalid = Messenger::Args(); invalid[EncodableValue("private")] = EncodableValue("true");
  Check(messenger.Show(invalid).errors == 1, "privacy cannot be omitted or coerced");
  Check(messenger.Show(Messenger::Args("fixture", "ryhze://app/activity?command=run")).errors == 1,
        "Activity never carries commands");
  Check(messenger.Call("launch").unknown == 1, "no execution operation");
  Result(messenger.Show(), "submitted");
  Result(messenger.Show(), "coalesced");
  Check(native->tags.size() == 1, "duplicate does not submit twice");
  for (int i = 0; i < 4; ++i) Result(messenger.Show(Messenger::Args(std::to_string(i).c_str())), "submitted");
  Result(messenger.Show(Messenger::Args("sixth")), "coalesced");
  Check(native->tags.size() == 5, "global burst bounded to five in a minute");
  now += 30000;
  Result(messenger.Show(), "coalesced");
  now += 30000;
  Result(messenger.Show(), "submitted");
  native->available = false;
  Result(messenger.Show(Messenger::Args("unavailable")), "unavailable");
  native->available = true; native->succeeds = false;
  Result(messenger.Show(Messenger::Args("failure")), "unavailable");
  native->succeeds = true;
  Result(messenger.Show(Messenger::Args("failure")), "submitted");
  Result(messenger.Call("clear"), "cleared");
  Check(native->clears == 1, "explicit privacy/account clear reaches owned backend");
  Result(messenger.Show(), "submitted");
  host.Detach();
  Check(!messenger.handler_ && native->clears == 1, "normal exit preserves protocol activation history");
}
void UninstalledIsUnavailable() {
  Messenger messenger;
  HostNotifications host;
  host.Attach(&messenger);
  const auto probe = messenger.Call("probe");
  Check(!std::get<bool>(std::get<EncodableMap>(probe.value).at(EncodableValue("notifications"))),
        "test executable cannot impersonate installed Ryhze identity");
  Result(messenger.Show(), "unavailable");
  Result(messenger.Call("clear"), "unavailable");
}
}  // namespace
int main() {
  const HRESULT initialized = RoInitialize(RO_INIT_SINGLETHREADED);
  try {
    Check(SUCCEEDED(initialized), "initialize Windows Runtime for XML-only fixture");
    XmlAndPrivacy(); ProtocolAndBounds(); UninstalledIsUnavailable();
    RoUninitialize();
    std::cout << "PASS: notification codec/XML/privacy/bounds; no toast displayed or registration modified\n";
    return 0;
  } catch (const std::exception& error) {
    if (SUCCEEDED(initialized)) RoUninitialize();
    std::cerr << "FAIL: " << error.what() << '\n'; return 1;
  }
}
