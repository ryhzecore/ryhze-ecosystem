// Test-only executable: real Flutter channels and native file workers, exclusively
// rooted in a new pinned temporary directory. Never compiled into ryhze.exe.
#include <windows.h>
#include <bcrypt.h>
#include <flutter/dart_project.h>
#include <flutter/flutter_view_controller.h>
#include <flutter/standard_method_codec.h>
#include <array>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include "content_transfer.h"
#include "host_content_staging.h"
#include "host_content_transfer.h"
#include "secure_files.h"
#include "utils.h"
#include "win32_window.h"

namespace {
namespace fs = ryhze::secure_files;
using flutter::EncodableMap;
using flutter::EncodableValue;
struct Isolation {
  std::wstring path;
  std::string nonce, root_identity, device_id;
  std::optional<fs::Root> parent, root;
  std::optional<fs::DirectoryLease> created;
  bool Create() {
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (!length || length >= std::size(temporary)) return false;
    std::wstring base(temporary, length);
    while (base.size() > 3 && base.back() == L'\\') base.pop_back();
    fs::Error error;
    parent = fs::Root::Open(base, false, &error);
    if (!parent) return false;
    std::array<unsigned char, 32> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    constexpr char hex[] = "0123456789abcdef";
    for (const auto byte : random) { nonce += hex[byte >> 4]; nonce += hex[byte & 15]; }
    const std::wstring name = L"ryhze-transfer-journey-" + std::wstring(nonce.begin(), nonce.end());
    created = parent->CreateDirectoryExclusive(name, &error);
    if (!created) return false;  // An existing name is never reused.
    root_identity = ryhze::ContentTransferIdentity(created->info().identity);
    path = base + L"\\" + name;
    root = fs::Root::Open(path, false, &error);
    if (!root || !root->EnsureDirectory(L"incoming", &error) ||
        !root->EnsureDirectory(L"content-staging", &error) ||
        !root->EnsureDirectory(L"journal", &error)) return false;
    fs::FileInfo info;
    if (!root->CreateExclusive(L"isolation.txt", std::vector<uint8_t>(nonce.begin(), nonce.end()),
                              &info, &error) ||
        !root->Inspect(L"content-staging", &info, &error) || !info.is_directory) return false;
    device_id = ryhze::ContentTransferIdentity(info.identity);
    return true;
  }
  // Pins outlive both hosts. Files and journals are retained; no recursive cleanup.
};

class JourneyWindow final : public Win32Window {
 public:
  explicit JourneyWindow(Isolation& isolation)
      : isolation_(isolation), transfer_(isolation.path), staging_(isolation.path) {}
 protected:
  bool OnCreate() override {
    if (!Win32Window::OnCreate()) return false;
    const auto frame = GetClientArea();
    flutter::DartProject project(L"data");
    controller_ = std::make_unique<flutter::FlutterViewController>(
        frame.right - frame.left, frame.bottom - frame.top, project);
    if (!controller_->engine() || !controller_->view()) return false;
    auto messenger = controller_->engine()->messenger();
    transfer_.Attach(messenger, GetHandle());
    staging_.Attach(messenger, GetHandle());
    receipt_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
        "com.ryhze.test/transfer-isolation.v1", &flutter::StandardMethodCodec::GetInstance());
    receipt_->SetMethodCallHandler([this](const auto& call, auto result) {
      if (call.method_name() != "receipt") { result->NotImplemented(); return; }
      if (call.arguments() && !call.arguments()->IsNull()) { result->Error("invalidData"); return; }
      result->Success(EncodableValue(EncodableMap{
        {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
        {EncodableValue("fixture"), EncodableValue("exclusive-native-transfer-journey-v1")},
        {EncodableValue("processId"), EncodableValue(static_cast<int64_t>(GetCurrentProcessId()))},
        {EncodableValue("nonce"), EncodableValue(isolation_.nonce)},
        {EncodableValue("rootIdentity"), EncodableValue(isolation_.root_identity)},
        {EncodableValue("deviceId"), EncodableValue(isolation_.device_id)},
        {EncodableValue("rootDirectory"), EncodableValue(Utf8FromUtf16(isolation_.path.c_str()))}
      }));
    });
    SetChildContent(controller_->view()->GetNativeWindow());
    controller_->engine()->SetNextFrameCallback([this]() { Show(); });
    controller_->ForceRedraw();
    return true;
  }
  void OnDestroy() override {
    if (receipt_) receipt_->SetMethodCallHandler(nullptr);
    receipt_.reset();
    transfer_.Detach();
    staging_.Detach();
    controller_.reset();
    Win32Window::OnDestroy();
  }
  LRESULT MessageHandler(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept override {
    if (auto value = transfer_.HandleMessage(message, wparam, lparam)) return *value;
    if (auto value = staging_.HandleMessage(message, wparam, lparam)) return *value;
    if (controller_) {
      if (auto value = controller_->HandleTopLevelWindowProc(window, message, wparam, lparam)) return *value;
    }
    return Win32Window::MessageHandler(window, message, wparam, lparam);
  }
 private:
  Isolation& isolation_;
  HostContentTransfer transfer_;
  HostContentStaging staging_;
  std::unique_ptr<flutter::FlutterViewController> controller_;
  std::unique_ptr<flutter::MethodChannel<EncodableValue>> receipt_;
};
}  // namespace

int APIENTRY wWinMain(HINSTANCE, HINSTANCE, wchar_t*, int) {
  AttachConsole(ATTACH_PARENT_PROCESS);
  // No root command line, environment override, activation mutex or account APIs.
  if (!GetCommandLineArguments().empty()) return EXIT_FAILURE;
  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return EXIT_FAILURE;
  int code = EXIT_FAILURE;
  {
    Isolation isolation;
    if (isolation.Create()) {
      JourneyWindow window(isolation);
      if (window.Create(L"Ryhze - isolated transfer acceptance", {10, 10}, {1280, 800})) {
        window.SetQuitOnClose(true);
        MSG message{};
        BOOL result;
        while ((result = GetMessage(&message, nullptr, 0, 0)) > 0) {
          TranslateMessage(&message);
          DispatchMessage(&message);
        }
        code = result < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
      }
    }
  }
  CoUninitialize();
  return code;
}
