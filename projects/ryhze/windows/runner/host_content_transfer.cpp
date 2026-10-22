#include "host_content_transfer.h"
#include <flutter/standard_method_codec.h>
#include <shlobj.h>
#include <algorithm>
#include <mutex>
#include <process.h>
#include <utility>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
const EncodableValue* Get(const EncodableMap& map, const char* key) {
  const auto found = map.find(EncodableValue(key));
  return found == map.end() ? nullptr : &found->second;
}
int64_t Number(const EncodableValue* value) {
  if (value) {
    if (const auto n = std::get_if<int32_t>(value)) return *n;
    if (const auto n = std::get_if<int64_t>(value)) return *n;
  }
  return -1;
}
bool Text(const EncodableMap& map, const char* key, std::string* out) {
  const auto* value = Get(map, key);
  const auto* text = value ? std::get_if<std::string>(value) : nullptr;
  if (!text || text->size() > 128) return false;
  *out = *text; return true;
}
bool Decode(const EncodableValue* value, ryhze::ContentTransferRequest* out) {
  const auto* map = value ? std::get_if<EncodableMap>(value) : nullptr;
  if (!map || map->size() != 10) return false;
  const auto slot = Number(Get(*map, "slot")), attempt = Number(Get(*map, "attempt"));
  const auto size = Number(Get(*map, "expectedSize")), observed = Number(Get(*map, "observedBytes"));
  const auto* digest_value = Get(*map, "expectedSha256");
  const auto* digest = digest_value ? std::get_if<std::vector<uint8_t>>(digest_value) : nullptr;
  if (slot < 0 || slot >= ryhze::ContentTransfer::kSlotCount || attempt < 0 || attempt > 1 ||
      size < 1 || static_cast<uint64_t>(size) > ryhze::ContentTransfer::kMaximumBytes || observed < 0 || observed > size ||
      !digest || digest->size() != out->expected_sha256.size() ||
      !Text(*map, "operationId", &out->operation_id) ||
      !Text(*map, "rootIdentity", &out->root_identity) ||
      !Text(*map, "directoryIdentity", &out->directory_identity) ||
      !Text(*map, "sourceIdentity", &out->source_identity) ||
      !Text(*map, "observedSha256", &out->observed_sha256)) return false;
  out->slot = static_cast<uint32_t>(slot); out->attempt = static_cast<uint32_t>(attempt);
  out->expected_size = static_cast<uint64_t>(size); out->observed_bytes = static_cast<uint64_t>(observed);
  std::copy(digest->begin(), digest->end(), out->expected_sha256.begin());
  return ryhze::ValidContentTransferRequest(*out);
}
std::optional<std::wstring> PrivateRoot() {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) return {};
  const std::wstring base(value); CoTaskMemFree(value);
  ryhze::secure_files::Error error;
  auto root = ryhze::secure_files::Root::Open(base, false, &error);
  if (!root || !root->EnsureDirectory(L"Ryhze-Entertainment", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private\\incoming", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private\\content-staging", &error)) return {};
  return base + L"\\Ryhze-Entertainment\\native-private";
}
void Reply(flutter::MethodResult<EncodableValue>* result, const ryhze::ContentTransferResult& value) {
  if (!value.error.empty()) { result->Error(value.error); return; }
  result->Success(EncodableValue(EncodableMap{
    {EncodableValue("state"), EncodableValue(value.state)},
    {EncodableValue("rootIdentity"), EncodableValue(value.root_identity)},
    {EncodableValue("directoryIdentity"), EncodableValue(value.directory_identity)},
    {EncodableValue("sourceIdentity"), EncodableValue(value.source_identity)},
    {EncodableValue("sourceRelative"), EncodableValue(value.source_relative)},
    {EncodableValue("observedBytes"), EncodableValue(static_cast<int64_t>(value.observed_bytes))},
    {EncodableValue("observedSha256"), EncodableValue(value.observed_sha256)}}));
}
enum class Operation { begin, append, finish, inspect, discard, drain };
#if defined(RYHZE_TRANSFER_NATIVE_TEST)
std::mutex worker_gate_mutex;
HANDLE worker_gate_entered = nullptr, worker_gate_release = nullptr;
void AwaitWorkerGate() {
  HANDLE entered = nullptr, release = nullptr;
  {
    std::lock_guard<std::mutex> guard(worker_gate_mutex);
    entered = worker_gate_entered; release = worker_gate_release;
    worker_gate_entered = nullptr; worker_gate_release = nullptr;
  }
  if (entered && release) { SetEvent(entered); WaitForSingleObject(release, INFINITE); }
}
#endif
}  // namespace

struct HostContentTransfer::WorkerState {
  std::shared_ptr<ryhze::ContentTransfer> transfer;
  Operation operation = Operation::inspect;
  ryhze::ContentTransferRequest request;
  std::optional<ryhze::ContentTransferRequest> resume;
  std::vector<uint8_t> bytes;
  ryhze::ContentTransferResult result;
  std::mutex mutex;
  HWND window = nullptr;
};
HostContentTransfer::HostContentTransfer(std::optional<std::wstring> root) : root_(std::move(root)) {}
HostContentTransfer::~HostContentTransfer() { Detach(); if (worker_) CloseHandle(worker_); }
#if defined(RYHZE_TRANSFER_NATIVE_TEST)
void HostContentTransfer::HoldNextWorkerForTesting(HANDLE entered, HANDLE release) {
  std::lock_guard<std::mutex> guard(worker_gate_mutex);
  worker_gate_entered = entered; worker_gate_release = release;
}
#endif
void HostContentTransfer::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach(); window_ = window; closing_ = false;
  if (!root_) root_ = PrivateRoot();
  device_id_.clear();
  if (root_ && !root_->empty()) {
    ryhze::secure_files::Error error;
    auto root = ryhze::secure_files::Root::Open(*root_, false, &error);
    ryhze::secure_files::FileInfo info;
    if (root && root->Inspect(L"content-staging", &info, &error) && info.is_directory)
      device_id_ = ryhze::ContentTransferIdentity(info.identity);
    if (!transfer_) transfer_ = std::make_shared<ryhze::ContentTransfer>(*root_);
  }
  // A detached worker deliberately has no window to notify and may still be
  // settling. Its old timer was removed; the new owner must poll actual exit.
  // Failed polling closes admission, without claiming that worker has settled.
  if (worker_ && (!window_ ||
      !SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr))) closing_ = true;
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.content-transfer.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto reply) {
    Poll();
    const auto& method = call.method_name();
    const auto* args = call.arguments();
    if (method == "probe" || method == "cancel" || method == "drain" || method == "close") {
      if (args && !args->IsNull()) { reply->Error("invalidData"); return; }
      if (method == "probe") {
        reply->Success(EncodableValue(EncodableMap{
          {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("transfer"), EncodableValue(!device_id_.empty() && window_ && !closing_)},
          {EncodableValue("activation"), EncodableValue(false)},
          {EncodableValue("authenticity"), EncodableValue(false)},
          {EncodableValue("deviceId"), EncodableValue(device_id_)}}));
      } else if (method == "cancel") { Cancel("cancelled"); reply->Success(); }
      else {
        if (method == "close") closing_ = true;
        Cancel("cancelled");
        if (drains_.size() >= 8) { reply->Error("conflict"); return; }
        if (!transfer_) { reply->Success(); return; }
        if (worker_ && (!window_ ||
            !SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr))) {
          closing_ = true; reply->Error("recovery"); return;
        }
        drains_.push_back(std::move(reply));
        if (!worker_) StartDrain();
      }
      return;
    }
    if (method != "begin" && method != "append" && method != "finish" && method != "inspect" && method != "discard") {
      reply->NotImplemented(); return;
    }
    if (closing_ || device_id_.empty() || !transfer_ || !window_) { reply->Error("unsupported"); return; }
    if (worker_ || !drains_.empty()) { reply->Error("conflict"); return; }
    auto next = std::make_shared<WorkerState>(); next->transfer = transfer_;
    next->operation = method == "begin" ? Operation::begin : method == "append" ? Operation::append :
        method == "finish" ? Operation::finish : method == "inspect" ? Operation::inspect : Operation::discard;
    const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
    if (!map || map->size() != (method == "begin" || method == "append" ? 2 : 1) ||
        !Decode(Get(*map, "request"), &next->request)) { reply->Error("invalidData"); return; }
    if (method == "begin") {
      const auto* value = Get(*map, "resume");
      if (!value) { reply->Error("invalidData"); return; }
      if (!value->IsNull()) {
        ryhze::ContentTransferRequest resume;
        if (!Decode(value, &resume)) { reply->Error("invalidData"); return; }
        next->resume = std::move(resume);
      }
    } else if (method == "append") {
      const auto* value = Get(*map, "bytes");
      const auto* bytes = value ? std::get_if<std::vector<uint8_t>>(value) : nullptr;
      if (!bytes || bytes->empty() || bytes->size() > ryhze::ContentTransfer::kChunkBytes) { reply->Error("invalidData"); return; }
      next->bytes = *bytes;
    }
    transfer_->ResetCancellation();
    pending_ = std::move(reply);
    if (!Start(next)) { auto failed = std::move(pending_); failed->Error("unsupported"); }
  });
}
bool HostContentTransfer::Start(std::shared_ptr<WorkerState> next) {
  if (!window_ || !SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr)) return false;
  next->window = window_; state_ = next;
  deadline_ = GetTickCount64() + 120000;
  auto* context = new std::shared_ptr<WorkerState>(next);
  const auto thread = _beginthreadex(nullptr, 0, Run, context, 0, nullptr);
  if (!thread) { delete context; state_.reset(); KillTimer(window_, reinterpret_cast<UINT_PTR>(this)); return false; }
  worker_ = reinterpret_cast<HANDLE>(thread); return true;
}
void HostContentTransfer::StartDrain() {
  auto next = std::make_shared<WorkerState>(); next->transfer = transfer_; next->operation = Operation::drain;
  if (!Start(next)) {
    auto failed = std::move(drains_); drains_.clear();
    for (auto& reply : failed) reply->Error("recovery");
  }
}
unsigned __stdcall HostContentTransfer::Run(void* context) {
  std::unique_ptr<std::shared_ptr<WorkerState>> owner(static_cast<std::shared_ptr<WorkerState>*>(context));
  auto state = *owner; owner.reset();
#if defined(RYHZE_TRANSFER_NATIVE_TEST)
  AwaitWorkerGate();
#endif
  try {
    switch (state->operation) {
      case Operation::begin: state->result = state->transfer->Begin(state->request, state->resume); break;
      case Operation::append: state->result = state->transfer->Append(state->request, state->bytes); break;
      case Operation::finish: state->result = state->transfer->Finish(state->request); break;
      case Operation::inspect: state->result = state->transfer->Inspect(state->request); break;
      case Operation::discard: state->result = state->transfer->Discard(state->request); break;
      case Operation::drain: state->transfer->Drain(); break;
    }
  } catch (...) { state->transfer->Drain(); state->result.error = "recovery"; }
  std::lock_guard<std::mutex> guard(state->mutex);
  if (state->window) PostMessageW(state->window, kMessage, 0, 0);
  else state->transfer->Drain(); // A detached owner cannot retain an idle writer.
  return 0;
}
void HostContentTransfer::Cancel(const char* error) {
  if (transfer_) transfer_->Cancel();
  if (worker_) CancelSynchronousIo(worker_);
  if (pending_) { auto reply = std::move(pending_); reply->Error(error); }
}
void HostContentTransfer::Poll() {
  if (worker_ && WaitForSingleObject(worker_, 0) == WAIT_OBJECT_0) {
    CloseHandle(worker_); worker_ = nullptr;
    const auto completed = std::move(state_);
    if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    if (pending_) { auto reply = std::move(pending_); Reply(reply.get(), completed->result); }
    if (!drains_.empty()) {
      if (completed->operation != Operation::drain) { StartDrain(); return; }
      auto drained = std::move(drains_); drains_.clear();
      for (auto& reply : drained) {
        if (completed->result.error.empty()) reply->Success(); else reply->Error("recovery");
      }
    }
  } else if (pending_ && GetTickCount64() >= deadline_) Cancel("recovery");
}
std::optional<LRESULT> HostContentTransfer::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  if (message != kMessage && !(message == WM_TIMER && wparam == reinterpret_cast<UINT_PTR>(this))) return {};
  Poll(); return 0;
}
void HostContentTransfer::Detach() {
  Cancel("cancelled");
  if (state_) { std::lock_guard<std::mutex> guard(state_->mutex); state_->window = nullptr; }
  auto drained = std::move(drains_); drains_.clear();
  for (auto& reply : drained) reply->Error("recovery");
  if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset(); window_ = nullptr;
  Poll();
  // Idle destruction performs only handle release; never content work on UI.
  if (!worker_) transfer_.reset();
}
