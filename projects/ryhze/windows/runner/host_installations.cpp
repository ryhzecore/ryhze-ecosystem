#include "host_installations.h"
#include "steam_inventory.h"
#include <flutter/standard_method_codec.h>
#include <atomic>
#include <mutex>
#include <process.h>
#include <utility>

namespace {
using flutter::EncodableValue;
using flutter::EncodableMap;
using flutter::EncodableList;
void Reply(flutter::MethodResult<EncodableValue>* reply, const ryhze::SteamInventoryResult& value) {
  EncodableList items;
  for (const auto& item : value.values) {
    items.emplace_back(EncodableMap{
      {EncodableValue("appId"), EncodableValue(item.app_id)},
      {EncodableValue("title"), EncodableValue(item.title)},
      {EncodableValue("libraryRoot"), EncodableValue(item.library_root)},
      {EncodableValue("relativeDirectory"), EncodableValue(item.relative_directory)},
      {EncodableValue("stateFlags"), EncodableValue(item.state_flags)}});
  }
  const char* quality = value.quality == ryhze::SteamInventoryQuality::complete ? "complete"
      : value.quality == ryhze::SteamInventoryQuality::partial ? "partial" : "unavailable";
  reply->Success(EncodableValue(EncodableMap{
    {EncodableValue("quality"), EncodableValue(quality)},
    {EncodableValue("items"), EncodableValue(items)}}));
}
}
struct HostInstallations::WorkerState {
  std::optional<std::wstring> root;
  std::atomic_bool cancelled{false};
  std::mutex lock;
  std::optional<ryhze::SteamInventoryResult> completion;
};
HostInstallations::HostInstallations(std::optional<std::wstring> root) : root_(std::move(root)) {}
HostInstallations::~HostInstallations() { Detach(); if (worker_) CloseHandle(worker_); }
void HostInstallations::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach();
  window_ = window;
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.installations.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    if (call.method_name() != "probe" && call.method_name() != "inspectSteam" &&
        call.method_name() != "cancel") { result->NotImplemented(); return; }
    if (call.arguments() && !call.arguments()->IsNull()) { result->Error("invalidData"); return; }
    if (call.method_name() == "probe") {
      result->Success(EncodableValue(EncodableMap{
        {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
        {EncodableValue("readOnlyInventory"), EncodableValue(window_ != nullptr)},
        {EncodableValue("ownership"), EncodableValue(false)},
        {EncodableValue("launch"), EncodableValue(false)}}));
      return;
    }
    if (call.method_name() == "cancel") { Cancel(); result->Success(); return; }
    if (!window_) { result->Error("unsupported"); return; }
    if (pending_ || (worker_ && WaitForSingleObject(worker_, 0) != WAIT_OBJECT_0)) {
      result->Error("conflict"); return;
    }
    if (worker_) { CloseHandle(worker_); worker_ = nullptr; }
    auto next = std::make_shared<WorkerState>();
    next->root = root_;
    if (!SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr)) {
      result->Error("unsupported"); return;
    }
    state_ = next;
    pending_ = std::move(result);
    deadline_ = GetTickCount64() + 15000;
    auto* context = new std::shared_ptr<WorkerState>(next);
    const uintptr_t thread = _beginthreadex(nullptr, 0, Run, context, 0, nullptr);
    if (!thread) { delete context; Cancel(); return; }
    worker_ = reinterpret_cast<HANDLE>(thread);
  });
}
unsigned __stdcall HostInstallations::Run(void* context) {
  std::unique_ptr<std::shared_ptr<WorkerState>> owner(static_cast<std::shared_ptr<WorkerState>*>(context));
  const auto state = *owner;
  owner.reset();
  ryhze::SteamInventoryResult result;
  try { result = ryhze::InspectSteamInstallations(state->root, &state->cancelled); }
  catch (...) { result = {}; }
  std::lock_guard<std::mutex> guard(state->lock);
  if (!state->cancelled.load()) state->completion = std::move(result);
  return 0;
}
void HostInstallations::Cancel() {
  if (state_) state_->cancelled.store(true);
  if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
  if (worker_) CancelSynchronousIo(worker_);
  if (pending_) { auto reply = std::move(pending_); reply->Error("cancelled"); }
}
std::optional<LRESULT> HostInstallations::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  if (message != WM_TIMER || wparam != reinterpret_cast<UINT_PTR>(this)) return std::nullopt;
  if (!pending_ || !state_) return 0;
  std::optional<ryhze::SteamInventoryResult> completed;
  { std::lock_guard<std::mutex> guard(state_->lock); completed = state_->completion; }
  if (completed) {
    KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    auto reply = std::move(pending_);
    Reply(reply.get(), *completed);
  } else if (GetTickCount64() >= deadline_) Cancel();
  return 0;
}
void HostInstallations::Detach() {
  Cancel();
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset();
  window_ = nullptr;
  state_.reset();
  if (worker_ && WaitForSingleObject(worker_, 0) == WAIT_OBJECT_0) {
    CloseHandle(worker_); worker_ = nullptr;
  }
}
