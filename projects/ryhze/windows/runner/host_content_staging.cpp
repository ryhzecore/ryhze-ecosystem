#include "host_content_staging.h"
#include "secure_files.h"
#include <flutter/standard_method_codec.h>
#include <shlobj.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <process.h>
#include <utility>

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
const EncodableValue* Get(const EncodableMap& map, const char* key) {
  auto found = map.find(EncodableValue(key));
  return found == map.end() ? nullptr : &found->second;
}
int64_t Number(const EncodableValue* value) {
  if (value) {
    if (auto n = std::get_if<int32_t>(value)) return *n;
    if (auto n = std::get_if<int64_t>(value)) return *n;
  }
  return -1;
}
bool Digest(const EncodableMap& map, const char* key, ryhze::ContentDigest* out) {
  const auto* value = Get(map, key);
  const auto* bytes = value ? std::get_if<std::vector<uint8_t>>(value) : nullptr;
  if (!bytes || bytes->size() != out->size()) return false;
  std::copy(bytes->begin(), bytes->end(), out->begin());
  return true;
}
bool Decode(const EncodableValue* args, ryhze::ContentStageRequest* out) {
  const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
  if (!map || map->size() != 9) return false;
  const auto slot = Number(Get(*map, "slot"));
  const auto size = Number(Get(*map, "size"));
  const auto minimum = Number(Get(*map, "minimumVerifiedBytes"));
  const auto* path_value = Get(*map, "sourceRelative");
  const auto* path = path_value ? std::get_if<std::string>(path_value) : nullptr;
  if (slot < 0 || slot >= ryhze::ContentStager::kSlotCount || size < 1 ||
      static_cast<uint64_t>(size) > ryhze::ContentStager::kMaximumBytes ||
      minimum < 0 || minimum > size ||
      (minimum != size && minimum % ryhze::ContentStager::kChunkBytes != 0) ||
      !path || path->empty() || path->size() > 512 ||
      !Digest(*map, "operationId", &out->operation_id) ||
      !Digest(*map, "manifestSha256", &out->manifest_sha256) ||
      !Digest(*map, "sha256", &out->expected_sha256)) return false;
  // UTF-8 decoding is strict. The stager/secure-files boundary additionally
  // enforces Windows relative-path, reserved-name and pinned-link rules.
  for (unsigned char c : *path) if (c < 32 || c >= 127) return false;
  size_t begin = 0;
  while (begin < path->size()) {
    const size_t end = path->find('/', begin);
    const auto part = path->substr(begin, end == std::string::npos ? end : end - begin);
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (part.empty() || part.size() > 255 || !alnum(part.front()) || part.back() == '.') return false;
    for (char c : part) if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
    if (end == std::string::npos) break;
    begin = end + 1;
    if (begin == path->size()) return false;
  }
  if (path->front() == '/' || path->find(':') != std::string::npos ||
      path->find('\\') != std::string::npos) return false;
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
      path->data(), static_cast<int>(path->size()), nullptr, 0);
  if (!count) return false;
  out->source_relative.resize(count);
  if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path->data(),
      static_cast<int>(path->size()), out->source_relative.data(), count)) return false;
  for (const auto* key : {"rootIdentity", "sourceIdentity"}) {
    const auto* value = Get(*map, key);
    const auto* text = value ? std::get_if<std::string>(value) : nullptr;
    if (!text || (!text->empty() && (text->size() != 49 || (*text)[16] != ':'))) return false;
    if (!text->empty()) for (size_t i = 0; i < text->size(); ++i) {
      if (i != 16 && !(((*text)[i] >= '0' && (*text)[i] <= '9') || ((*text)[i] >= 'a' && (*text)[i] <= 'f'))) return false;
    }
    if (std::string(key) == "rootIdentity") out->expected_root_identity = *text;
    else out->expected_source_identity = *text;
  }
  std::replace(out->source_relative.begin(), out->source_relative.end(), L'/', L'\\');
  out->slot = static_cast<uint32_t>(slot);
  out->expected_size = static_cast<uint64_t>(size);
  out->minimum_verified_bytes = static_cast<uint64_t>(minimum);
  return true;
}
std::optional<std::wstring> PrivateRoot() {
  PWSTR value = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &value))) return std::nullopt;
  const std::wstring base(value);
  CoTaskMemFree(value);
  ryhze::secure_files::Error error;
  auto root = ryhze::secure_files::Root::Open(base, false, &error);
  if (!root || !root->EnsureDirectory(L"Ryhze-Entertainment", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private\\incoming", &error) ||
      !root->EnsureDirectory(L"Ryhze-Entertainment\\native-private\\content-staging", &error)) return std::nullopt;
  return base + L"\\Ryhze-Entertainment\\native-private";
}
const char* State(ryhze::ContentStageState state) {
  switch (state) {
    case ryhze::ContentStageState::staged: return "staged";
    case ryhze::ContentStageState::checkpoint: return "checkpoint";
    case ryhze::ContentStageState::cancelled: return "cancelled";
    case ryhze::ContentStageState::invalid: return "invalid";
    case ryhze::ContentStageState::conflict: return "conflict";
    case ryhze::ContentStageState::recovery: return "recovery";
    case ryhze::ContentStageState::quota: return "quota";
    case ryhze::ContentStageState::unavailable: return "unavailable";
  }
  return "unavailable";
}
void Reply(flutter::MethodResult<EncodableValue>* reply, const ryhze::ContentStageResult& value) {
  reply->Success(EncodableValue(EncodableMap{
    {EncodableValue("state"), EncodableValue(State(value.state))},
    {EncodableValue("verifiedBytes"), EncodableValue(static_cast<int64_t>(value.verified_bytes))},
    {EncodableValue("verifiedChunks"), EncodableValue(static_cast<int64_t>(value.verified_chunks))},
    {EncodableValue("rootIdentity"), EncodableValue(value.root_identity)},
    {EncodableValue("sourceIdentity"), EncodableValue(value.source_identity)}}));
}
const char* DiscardState(ryhze::ContentDiscardState state) {
  switch (state) {
    case ryhze::ContentDiscardState::ready: return "ready";
    case ryhze::ContentDiscardState::completed: return "completed";
    case ryhze::ContentDiscardState::cancelled: return "cancelled";
    case ryhze::ContentDiscardState::invalid: return "invalid";
    case ryhze::ContentDiscardState::conflict: return "conflict";
    case ryhze::ContentDiscardState::recovery: return "recovery";
    case ryhze::ContentDiscardState::unavailable: return "unavailable";
  }
  return "unavailable";
}
void Reply(flutter::MethodResult<EncodableValue>* reply, const ryhze::ContentDiscardResult& value) {
  reply->Success(EncodableValue(EncodableMap{
    {EncodableValue("state"), EncodableValue(DiscardState(value.state))},
    {EncodableValue("removedBytes"), EncodableValue(static_cast<int64_t>(value.removed_bytes))},
    {EncodableValue("removedFiles"), EncodableValue(static_cast<int64_t>(value.removed_files))}}));
}
void Reply(flutter::MethodResult<EncodableValue>* reply, const ryhze::ContentDiscardPlanResult& value) {
  reply->Success(EncodableValue(EncodableMap{
    {EncodableValue("state"), EncodableValue(State(value.state))},
    {EncodableValue("plan"), EncodableValue(value.plan)},
    {EncodableValue("verifiedBytes"), EncodableValue(static_cast<int64_t>(value.verified_bytes))},
    {EncodableValue("verifiedChunks"), EncodableValue(static_cast<int64_t>(value.verified_chunks))}}));
}
enum class Operation { stage, inspect, plan_discard, discard, inspect_discard };
}
struct HostContentStaging::WorkerState {
  explicit WorkerState(const std::wstring& root) : stager(root) {}
  ryhze::ContentStager stager;
  ryhze::ContentStageRequest request;
  std::atomic<bool> cancelled{false};
  Operation operation = Operation::inspect;
  std::vector<uint8_t> discard_plan;
  std::mutex lock;
  HWND window = nullptr;
  ryhze::ContentStageResult result;
  ryhze::ContentDiscardPlanResult plan_result;
  ryhze::ContentDiscardResult discard_result;
};
HostContentStaging::HostContentStaging(std::optional<std::wstring> root) : root_(std::move(root)) {}
HostContentStaging::~HostContentStaging() {
  Detach();
  if (worker_) CloseHandle(worker_); // Worker owns all native state until exit.
}
void HostContentStaging::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach();
  window_ = window;
  closing_ = false;
  if (!root_) root_ = PrivateRoot();
  device_id_.clear();
  if (root_ && !root_->empty()) {
    ryhze::secure_files::Error error;
    auto root = ryhze::secure_files::Root::Open(*root_, false, &error);
    ryhze::secure_files::FileInfo info;
    if (root && root->Inspect(L"content-staging", &info, &error) && info.is_directory) {
      constexpr char hex[] = "0123456789abcdef";
      for (int shift = 60; shift >= 0; shift -= 4) device_id_ += hex[(info.identity.volume >> shift) & 15];
      device_id_ += ':';
      for (auto byte : info.identity.id) { device_id_ += hex[byte >> 4]; device_id_ += hex[byte & 15]; }
    }
  }
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.content-staging.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto reply) {
    Poll();
    const auto& method = call.method_name();
    const auto* args = call.arguments();
    if (method == "probe" || method == "probeDiscard" || method == "cancel" || method == "drain" || method == "close") {
      if (args && !args->IsNull()) { reply->Error("invalidData"); return; }
      if (method == "probe") {
        reply->Success(EncodableValue(EncodableMap{
          {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("staging"), EncodableValue(!device_id_.empty() && window_ && !closing_)},
          {EncodableValue("activation"), EncodableValue(false)},
          {EncodableValue("authenticity"), EncodableValue(false)},
          {EncodableValue("deviceId"), device_id_.empty() ? EncodableValue() : EncodableValue(device_id_)}}));
      } else if (method == "probeDiscard") {
        reply->Success(EncodableValue(EncodableMap{
          {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("discard"), EncodableValue(!device_id_.empty() && window_ && !closing_)}}));
      } else if (method == "cancel") {
        Cancel("cancelled"); reply->Success();
      } else {
        if (method == "close") { closing_ = true; Cancel("cancelled"); }
        if (drains_.size() >= 8) { reply->Error("conflict"); return; }
        if (!worker_) reply->Success();
        else {
          drains_.push_back(std::move(reply));
          if (!SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr)) {
            auto failed = std::move(drains_.back()); drains_.pop_back(); failed->Error("recovery");
          }
        }
      }
      return;
    }
    const bool plan_discard = method == "planDiscard";
    const bool execute_discard = method == "discard";
    const bool inspect_discard = method == "inspectDiscard";
    if (method != "stage" && method != "inspect" && !plan_discard && !execute_discard && !inspect_discard) { reply->NotImplemented(); return; }
    ryhze::ContentStageRequest request;
    std::vector<uint8_t> discard_plan;
    if (execute_discard || inspect_discard) {
      const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
      const auto* value = map ? Get(*map, "plan") : nullptr;
      const auto* bytes = value ? std::get_if<std::vector<uint8_t>>(value) : nullptr;
      if (!map || map->size() != 1 || !bytes || bytes->empty() || bytes->size() > 512 * 1024) { reply->Error("invalidData"); return; }
      discard_plan = *bytes;
    } else if (!Decode(args, &request) || ((method == "stage" || plan_discard) &&
        (request.expected_root_identity.empty() || request.expected_source_identity.empty()))) { reply->Error("invalidData"); return; }
    if (closing_ || device_id_.empty() || !root_ || root_->empty() || !window_) {
      if (plan_discard) Reply(reply.get(), ryhze::ContentDiscardPlanResult{});
      else if (execute_discard || inspect_discard) Reply(reply.get(), ryhze::ContentDiscardResult{});
      else Reply(reply.get(), ryhze::ContentStageResult{});
      return;
    }
    if (worker_ || !drains_.empty()) { reply->Error("conflict"); return; }
    if (!SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr)) { reply->Error("unsupported"); return; }
    auto next = std::make_shared<WorkerState>(*root_);
    next->window = window_;
    next->request = std::move(request);
    next->operation = plan_discard ? Operation::plan_discard : execute_discard ? Operation::discard :
        inspect_discard ? Operation::inspect_discard : method == "stage" ? Operation::stage : Operation::inspect;
    next->discard_plan = std::move(discard_plan);
    state_ = next;
    pending_ = std::move(reply);
    deadline_ = GetTickCount64() + 120000;
    auto* context = new std::shared_ptr<WorkerState>(next);
    const auto thread = _beginthreadex(nullptr, 0, Run, context, 0, nullptr);
    if (!thread) {
      delete context;
      auto failed = std::move(pending_); failed->Error("unsupported");
      state_.reset(); KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    } else worker_ = reinterpret_cast<HANDLE>(thread);
  });
}
unsigned __stdcall HostContentStaging::Run(void* context) {
  std::unique_ptr<std::shared_ptr<WorkerState>> owner(static_cast<std::shared_ptr<WorkerState>*>(context));
  auto state = *owner; owner.reset();
  try {
    auto cancelled = [state] { return state->cancelled.load(); };
    switch (state->operation) {
      case Operation::stage: state->result = state->stager.Stage(state->request, cancelled); break;
      case Operation::inspect: state->result = state->stager.Inspect(state->request, cancelled); break;
      case Operation::plan_discard: state->plan_result = state->stager.PlanDiscard(state->request, cancelled); break;
      case Operation::discard: state->discard_result = state->stager.Discard(state->discard_plan, cancelled); break;
      case Operation::inspect_discard: state->discard_result = state->stager.InspectDiscard(state->discard_plan, cancelled); break;
    }
  } catch (...) {
    state->result.state = ryhze::ContentStageState::recovery;
    state->plan_result.state = ryhze::ContentStageState::recovery;
    state->plan_result.plan.clear();
    state->discard_result.state = ryhze::ContentDiscardState::recovery;
  }
  std::lock_guard<std::mutex> guard(state->lock);
  if (state->window) PostMessageW(state->window, kMessage, 0, 0);
  return 0;
}
void HostContentStaging::Cancel(const char* error) {
  if (state_) state_->cancelled.store(true);
  if (worker_) CancelSynchronousIo(worker_);
  if (pending_) { auto reply = std::move(pending_); reply->Error(error); }
  // Keep timer/worker ownership alive: cancellation is not settlement.
}
void HostContentStaging::Poll() {
  if (worker_ && WaitForSingleObject(worker_, 0) == WAIT_OBJECT_0) {
    CloseHandle(worker_); worker_ = nullptr;
    const auto completion = std::move(state_);
    auto pending = std::move(pending_);
    auto drained = std::move(drains_);
    drains_.clear();
    if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    if (pending) {
      if (completion->operation == Operation::plan_discard) Reply(pending.get(), completion->plan_result);
      else if (completion->operation == Operation::discard || completion->operation == Operation::inspect_discard)
        Reply(pending.get(), completion->discard_result);
      else Reply(pending.get(), completion->result);
    }
    for (auto& reply : drained) reply->Success();
  } else if (pending_ && GetTickCount64() >= deadline_) Cancel("recovery");
}
std::optional<LRESULT> HostContentStaging::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  if (message != kMessage && !(message == WM_TIMER && wparam == reinterpret_cast<UINT_PTR>(this))) return std::nullopt;
  Poll(); return 0;
}
void HostContentStaging::Detach() {
  Cancel("cancelled");
  if (state_) { std::lock_guard<std::mutex> guard(state_->lock); state_->window = nullptr; }
  auto drained = std::move(drains_);
  drains_.clear();
  for (auto& reply : drained) reply->Error("recovery");
  if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset(); window_ = nullptr;
  // A detached worker retains state and forbids replacement until it exits.
  Poll();
}
