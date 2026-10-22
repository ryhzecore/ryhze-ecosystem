#include "host_security.h"
#include "secure_files.h"

#include <windows.h>
#include <flutter/method_result_functions.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <atomic>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
constexpr const char* kChannel = "com.ryhze.ryhze/windows.security.v1";
const std::vector<uint8_t> kSyntheticSecret = {'s','y','n','t','h','e','t','i','c','-','o','n','l','y','-','s','e','c','r','e','t'};

void Check(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
std::string Utf8(const std::wstring& value) {
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
  Check(count > 0, "fixture UTF8 sizing");
  std::string result(count, '\0');
  Check(WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
      static_cast<int>(value.size()), result.data(), count, nullptr, nullptr) == count,
      "fixture UTF8 conversion");
  return result;
}

struct Fixture {
  std::wstring base;
  std::wstring vault;
  Fixture() {
    wchar_t temp[32768]{};
    const auto count = GetTempPathW(static_cast<DWORD>(std::size(temp)), temp);
    Check(count > 0 && count < std::size(temp), "OS temporary fixture base");
    static std::atomic<unsigned> sequence{0};
    const auto created = std::wstring(temp) + L"ryhze-host-security-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) +
        L"-" + std::to_wstring(++sequence);
    Check(CreateDirectoryW(created.c_str(), nullptr) != FALSE, "create fresh fixture");
    HANDLE handle = CreateFileW(created.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    Check(handle != INVALID_HANDLE_VALUE, "fixture canonical handle");
    wchar_t canonical[32768]{};
    const DWORD length = GetFinalPathNameByHandleW(handle, canonical,
        static_cast<DWORD>(std::size(canonical)), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(handle);
    Check(length > 4 && length < std::size(canonical), "fixture canonical path");
    base = std::wstring(canonical, length);
    Check(base.rfind(L"\\\\?\\", 0) == 0, "fixture local namespace");
    base.erase(0, 4);
    vault = base + L"\\vault";
    Check(CreateDirectoryW(vault.c_str(), nullptr) != FALSE, "private injected vault fixture");
    std::wcout << L"FIXTURE retained: " << base << L"\n";
  }
};

class Messenger final : public flutter::BinaryMessenger {
 public:
  void Send(const std::string&, const uint8_t*, size_t, flutter::BinaryReply) const override {
    Check(false, "security bridge must not emit unsolicited payloads");
  }
  void SetMessageHandler(const std::string& channel, flutter::BinaryMessageHandler handler) override {
    Check(channel == kChannel, "security channel identity");
    handler_ = std::move(handler);
  }
  struct Reply {
    EncodableValue value;
    EncodableValue details;
    std::string code;
    std::string message;
    std::vector<uint8_t> wire;
    int successes = 0;
    int errors = 0;
    int unimplemented = 0;
    int replies = 0;
  };
  Reply Call(const std::string& method, std::unique_ptr<EncodableValue> arguments = nullptr) {
    Check(static_cast<bool>(handler_), "attached security handler");
    const auto& codec = flutter::StandardMethodCodec::GetInstance();
    const auto encoded = codec.EncodeMethodCall(flutter::MethodCall<EncodableValue>(method, std::move(arguments)));
    Reply reply;
    flutter::MethodResultFunctions<EncodableValue> decoder(
        [&](const EncodableValue* value) { ++reply.successes; if (value) reply.value = *value; },
        [&](const std::string& code, const std::string& message, const EncodableValue* details) {
          ++reply.errors; reply.code = code; reply.message = message; if (details) reply.details = *details;
        },
        [&] { ++reply.unimplemented; });
    handler_(encoded->data(), encoded->size(), [&](const uint8_t* data, size_t size) {
      ++reply.replies;
      if (size == 0) { decoder.NotImplemented(); return; }
      reply.wire.assign(data, data + size);
      Check(codec.DecodeAndProcessResponseEnvelope(data, size, &decoder), "valid security response envelope");
    });
    Check(reply.replies == 1, "exactly one security reply");
    return reply;
  }
  flutter::BinaryMessageHandler handler_;
};

std::unique_ptr<EncodableValue> Args(EncodableMap value) {
  return std::make_unique<EncodableValue>(std::move(value));
}
EncodableMap Scope(const std::string& account = "Account", const std::string& provider = "fixture") {
  return {{EncodableValue("provider"), EncodableValue(provider)},
          {EncodableValue("account"), EncodableValue(account)}};
}
EncodableMap Write(const std::vector<uint8_t>& secret = kSyntheticSecret,
                   const std::string& account = "Account") {
  auto args = Scope(account);
  args.emplace(EncodableValue("value"), EncodableValue(secret));
  return args;
}
void Error(const Messenger::Reply& reply, const char* code) {
  Check(reply.errors == 1 && reply.successes == 0 && reply.unimplemented == 0, "one closed failure");
  Check(reply.code == code && reply.message.empty() && reply.details.IsNull(), "redacted failure fields");
  Check(std::search(reply.wire.begin(), reply.wire.end(), kSyntheticSecret.begin(), kSyntheticSecret.end()) ==
        reply.wire.end(), "no plaintext in failure envelope");
}
void NullSuccess(const Messenger::Reply& reply) {
  Check(reply.successes == 1 && reply.errors == 0 && reply.value.IsNull(), "null success");
}

void SchemasAndLifecycle() {
  Fixture fixture;
  Messenger first, second;
  {
    HostSecurity bridge(fixture.vault);
    bridge.Attach(&first);
    auto probe = first.Call("probe");
    Check(probe.successes == 1, "probe supported");
    const auto& fields = std::get<EncodableMap>(probe.value);
    Check(fields.size() == 3 && std::get<int32_t>(fields.at(EncodableValue("schemaVersion"))) == 1 &&
          std::get<bool>(fields.at(EncodableValue("secureCredentials"))) &&
          std::get<bool>(fields.at(EncodableValue("fileIdentity"))), "typed capability probe");
    Error(first.Call("probe", Args({})), "invalidData");
    Error(first.Call("readCredential"), "invalidData");
    Error(first.Call("readCredential", std::make_unique<EncodableValue>(true)), "invalidData");
    Error(first.Call("readCredential", Args({})), "invalidData");
    auto extra = Scope(); extra.emplace(EncodableValue("root"), EncodableValue(Utf8(fixture.vault)));
    Error(first.Call("readCredential", Args(extra)), "invalidData");
    auto wrong_key = Scope(); wrong_key.erase(EncodableValue("provider"));
    wrong_key.emplace(EncodableValue(int32_t{1}), EncodableValue("fixture"));
    Error(first.Call("readCredential", Args(wrong_key)), "invalidData");
    auto wrong_type = Scope(); wrong_type[EncodableValue("account")] = EncodableValue(int64_t{1});
    Error(first.Call("deleteCredential", Args(wrong_type)), "invalidData");
    auto string_secret = Scope(); string_secret.emplace(EncodableValue("value"), EncodableValue("synthetic-only-secret"));
    Error(first.Call("writeCredential", Args(string_secret)), "invalidData");
    Error(first.Call("writeCredential", Args(Write({}))), "invalidData");
    Error(first.Call("writeCredential", Args(Write(std::vector<uint8_t>(ryhze::CredentialVault::kMaxSecretBytes + 1, 1)))), "invalidData");
    Error(first.Call("readCredential", Args(Scope(std::string("bad\0account", 11)))), "invalidData");
    Error(first.Call("readCredential", Args(Scope("Account", std::string("\xc0\xaf", 2)))), "invalidData");
    Check(first.Call("launch").unimplemented == 1 && first.Call("launch", Args({})).unimplemented == 1,
          "unknown method uniformly unimplemented");
    bridge.Attach(&second);
    Check(!first.handler_ && static_cast<bool>(second.handler_), "reattach removes previous callback");
    bridge.Detach();
    bridge.Detach();
    Check(!second.handler_, "idempotent detach");
    bridge.Attach(&first);
  }
  Check(!first.handler_, "destructor detaches callback");
}

void CredentialRoundTrip() {
  Fixture fixture;
  Messenger messenger;
  {
    HostSecurity bridge(fixture.vault);
    bridge.Attach(&messenger);
    NullSuccess(messenger.Call("readCredential", Args(Scope())));
    NullSuccess(messenger.Call("writeCredential", Args(Write())));
    auto read = messenger.Call("readCredential", Args(Scope()));
    Check(read.successes == 1 && std::get<std::vector<uint8_t>>(read.value) == kSyntheticSecret, "synthetic secret roundtrip");
    NullSuccess(messenger.Call("readCredential", Args(Scope("account"))));
    NullSuccess(messenger.Call("readCredential", Args(Scope("Account", "Fixture"))));
    NullSuccess(messenger.Call("writeCredential", Args(Write({7, 0, 9}))));
    NullSuccess(messenger.Call("writeCredential", Args(Write({6}, "other"))));
  }
  {
    HostSecurity reopened(fixture.vault);
    reopened.Attach(&messenger);
    auto read = messenger.Call("readCredential", Args(Scope()));
    Check(std::get<std::vector<uint8_t>>(read.value) == std::vector<uint8_t>({7, 0, 9}), "durable replacement across bridge lifetime");
    NullSuccess(messenger.Call("deleteCredential", Args(Scope())));
    NullSuccess(messenger.Call("deleteCredential", Args(Scope())));
    NullSuccess(messenger.Call("readCredential", Args(Scope())));
    auto other = messenger.Call("readCredential", Args(Scope("other")));
    Check(std::get<std::vector<uint8_t>>(other.value) == std::vector<uint8_t>({6}), "delete remains exact scope");
  }
}

void InspectionAndUnavailableRoot() {
  Fixture fixture;
  ryhze::secure_files::Error native;
  auto root = ryhze::secure_files::Root::Open(fixture.base, false, &native);
  Check(root.has_value(), "inspection fixture root");
  ryhze::secure_files::FileInfo info;
  Check(root->EnsureDirectory(L"inspect", &native) &&
        root->CreateExclusive(L"inspect\\item.bin", {1, 2}, &info, &native), "inspection fixture file");
  Messenger messenger;
  HostSecurity bridge(fixture.vault);
  bridge.Attach(&messenger);
  auto input = EncodableMap{{EncodableValue("root"), EncodableValue(Utf8(fixture.base))},
                            {EncodableValue("relative"), EncodableValue("inspect\\item.bin")}};
  auto response = messenger.Call("inspectFile", Args(input));
  Check(response.successes == 1, "inspect response");
  const auto& observed = std::get<EncodableMap>(response.value);
  Check(observed.size() == 4 && std::get<std::vector<uint8_t>>(observed.at(EncodableValue("identity"))).size() == 16 &&
        !std::get<bool>(observed.at(EncodableValue("directory"))) &&
        std::get<int64_t>(observed.at(EncodableValue("links"))) == 1 &&
        std::get<std::string>(observed.at(EncodableValue("volume"))) == std::to_string(info.identity.volume), "typed file observation only");
  auto missing = input; missing[EncodableValue("relative")] = EncodableValue("missing.bin");
  Error(messenger.Call("inspectFile", Args(missing)), "denied");
  for (const auto* invalid : {"..\\outside", "inspect\\item.bin:stream", "CON", "C:\\outside", "inspect/item.bin"}) {
    auto attempt = input; attempt[EncodableValue("relative")] = EncodableValue(invalid);
    Error(messenger.Call("inspectFile", Args(attempt)), "denied");
  }
  auto bad_utf8 = input; bad_utf8[EncodableValue("root")] = EncodableValue(std::string("\xc0\xaf", 2));
  Error(messenger.Call("inspectFile", Args(bad_utf8)), "invalidData");
  auto extra = input; extra.emplace(EncodableValue("execute"), EncodableValue(true));
  Error(messenger.Call("inspectFile", Args(extra)), "invalidData");
  bridge.Detach();
  HostSecurity unavailable(fixture.base + L"\\missing-private-root");
  unavailable.Attach(&messenger);
  const auto probe = messenger.Call("probe");
  Check(!std::get<bool>(std::get<EncodableMap>(probe.value).at(EncodableValue("secureCredentials"))), "unavailable injected root stays unavailable");
  Error(messenger.Call("writeCredential", Args(Write())), "unsupported");
  auto malformed = Scope(); malformed.emplace(EncodableValue("value"), EncodableValue("synthetic-only-secret"));
  Error(messenger.Call("writeCredential", Args(malformed)), "invalidData");
  Check(GetFileAttributesW((fixture.base + L"\\missing-private-root").c_str()) == INVALID_FILE_ATTRIBUTES,
        "unavailable test cannot create/fallback to other root");
}

void CorruptionDoesNotExposePlaintext() {
  Fixture fixture;
  Messenger messenger;
  HostSecurity bridge(fixture.vault);
  bridge.Attach(&messenger);
  NullSuccess(messenger.Call("writeCredential", Args(Write())));
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW((fixture.vault + L"\\*").c_str(), &found);
  Check(search != INVALID_HANDLE_VALUE, "synthetic scope enumeration");
  std::wstring scope;
  do {
    if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && std::wstring(found.cFileName).size() == 64) {
      scope = found.cFileName; break;
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
  Check(!scope.empty(), "one synthetic hashed scope");
  const auto path = fixture.vault + L"\\" + scope + L"\\primary.bin";
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  Check(file != INVALID_HANDLE_VALUE, "open only newly-created synthetic cipher");
  const uint8_t corrupt = 0;
  DWORD written = 0;
  const bool changed = WriteFile(file, &corrupt, 1, &written, nullptr) && written == 1 && FlushFileBuffers(file);
  CloseHandle(file);
  Check(changed, "corrupt synthetic cipher");
  Error(messenger.Call("readCredential", Args(Scope())), "recovery");
  Error(messenger.Call("writeCredential", Args(Write())), "recovery");
  Error(messenger.Call("deleteCredential", Args(Scope())), "recovery");
  Check(GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES, "unknown cipher retained");
}
}  // namespace

int main() {
  try {
    SchemasAndLifecycle();
    CredentialRoundTrip();
    InspectionAndUnavailableRoot();
    CorruptionDoesNotExposePlaintext();
    std::cout << "PASS: 4 host-security suites; synthetic injected roots only\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "FAIL: " << error.what() << "\nSynthetic fixtures retained.\n";
    return 1;
  }
}
