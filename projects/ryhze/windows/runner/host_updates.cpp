#include "host_updates.h"
#include <flutter/standard_method_codec.h>
#include <algorithm>
#include <mutex>
#include <process.h>
#include <utility>

namespace {
using flutter::EncodableValue;
using flutter::EncodableMap;
const EncodableValue* Get(const EncodableMap& map, const char* key) {
  const auto it = map.find(EncodableValue(key));
  return it == map.end() ? nullptr : &it->second;
}
const char* State(ryhze::UpdateStageState value) {
  switch (value) {
    case ryhze::UpdateStageState::unavailable: return "unavailable";
    case ryhze::UpdateStageState::invalid: return "invalid";
    case ryhze::UpdateStageState::untrusted: return "untrusted";
    case ryhze::UpdateStageState::staged: return "staged";
    case ryhze::UpdateStageState::recovery: return "recovery";
    case ryhze::UpdateStageState::conflict: return "conflict";
  }
  return "unavailable";
}
void Reply(flutter::MethodResult<EncodableValue>* reply, ryhze::UpdateStageResult value) {
  reply->Success(EncodableValue(EncodableMap{
    {EncodableValue("state"), EncodableValue(State(value.state))},
    {EncodableValue("bytes"), EncodableValue(static_cast<int64_t>(value.bytes))}}));
}
}
struct HostUpdates::WorkerState {
  WorkerState(const std::wstring& root, std::optional<ryhze::UpdateDigest> publisher)
      : stager(root, publisher) {}
  ryhze::UpdateStager stager;
  std::mutex lock;
  HWND window = nullptr;
  bool cancelled = false;
  bool stage = false;
  uint64_t size = 0;
  ryhze::UpdateDigest digest{};
  std::vector<uint8_t> bytes;
  std::optional<ryhze::UpdateStageResult> completion;
#ifdef RYHZE_UPDATES_NATIVE_TEST
  HANDLE test_entered = nullptr;
  HANDLE test_release = nullptr;
  ~WorkerState() {
    if (test_entered) CloseHandle(test_entered);
    if (test_release) CloseHandle(test_release);
  }
#endif
};

HostUpdates::HostUpdates(std::wstring private_root, std::optional<ryhze::UpdateDigest> publisher)
    : private_root_(std::move(private_root)), publisher_(publisher),
      configured_(publisher_.has_value() && !private_root_.empty()) {}
HostUpdates::~HostUpdates() {
  Detach();
  if (worker_) CloseHandle(worker_);
}
void HostUpdates::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach();
  window_ = window;
  closing_ = false;
  if (state_) {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->window = window_;
  }
  // Detach may leave a trust-provider call alive. Keep observing its actual
  // thread even when no further operation arrives on the replacement channel.
  if (worker_) ArmTimer();
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.updates.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto result) {
    Poll();
    const auto* args = call.arguments();
    const auto& method = call.method_name();
    if (method == "probe" || method == "cancel" || method == "drain" || method == "close") {
      if (args && !args->IsNull()) { result->Error("invalidData"); return; }
      if (method == "probe") {
        result->Success(EncodableValue(EncodableMap{
          {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
          {EncodableValue("staging"), EncodableValue(configured_ && window_ != nullptr && !closing_)},
          {EncodableValue("activation"), EncodableValue(false)}}));
      } else if (method == "cancel") {
        Cancel("cancelled");
        result->Success();  // Request acknowledgement, never settlement.
      } else {
        if (method == "close") { closing_ = true; Cancel("cancelled"); }
        if (!worker_) result->Success();
        else if (drains_.size() >= 8) result->Error("conflict");
        else if (!ArmTimer()) result->Error("recovery");
        else drains_.push_back(std::move(result));
      }
      return;
    }
    const bool stage = call.method_name() == "stage";
    if (!stage && call.method_name() != "inspect") { result->NotImplemented(); return; }
    const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
    if (!map || map->size() != 2) { result->Error("invalidData"); return; }
    const auto* hash_value = Get(*map, "sha256");
    const auto* hash = hash_value ? std::get_if<std::vector<uint8_t>>(hash_value) : nullptr;
    if (!hash || hash->size() != 32) { result->Error("invalidData"); return; }
    ryhze::UpdateDigest digest;
    std::copy(hash->begin(), hash->end(), digest.begin());
    const std::vector<uint8_t>* input = nullptr;
    uint64_t expected_size = 0;
    if (stage) {
      const auto* value = Get(*map, "bytes");
      const auto* bytes = value ? std::get_if<std::vector<uint8_t>>(value) : nullptr;
      if (!bytes || bytes->empty() || bytes->size() > ryhze::UpdateStager::kMaximumBytes) {
        result->Error("invalidData"); return;
      }
      input = bytes;
    } else {
      const auto* value = Get(*map, "size");
      int64_t size = 0;
      if (value) {
        if (const auto* big = std::get_if<int64_t>(value)) size = *big;
        else if (const auto* narrow_value = std::get_if<int32_t>(value)) size = *narrow_value;
      }
      if (size < 1 || size > ryhze::UpdateStager::kMaximumBytes) { result->Error("invalidData"); return; }
      expected_size = static_cast<uint64_t>(size);
    }
    if (!configured_ || !window_ || closing_) { Reply(result.get(), {}); return; }
    if (worker_ || pending_ || !drains_.empty()) {
      Reply(result.get(), {ryhze::UpdateStageState::conflict, 0});
      return;
    }
    auto next = std::make_shared<WorkerState>(private_root_, publisher_);
    next->window = window_;
    next->stage = stage;
    next->digest = digest;
    next->size = expected_size;
    if (input) next->bytes = *input;
#ifdef RYHZE_UPDATES_NATIVE_TEST
    for (const auto& pair : {std::make_pair(test_entered_, &next->test_entered),
                             std::make_pair(test_release_, &next->test_release)}) {
      if (pair.first && !DuplicateHandle(GetCurrentProcess(), pair.first,
          GetCurrentProcess(), pair.second, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        result->Error("unsupported"); return;
      }
    }
#endif
    // A message-only test window also works; neither worker nor timer pumps UI.
    if (!ArmTimer()) {
      result->Error("unsupported");
      return;
    }
    deadline_ = GetTickCount64() + 30000;
    state_ = next;
    pending_ = std::move(result);
    auto* context = new std::shared_ptr<WorkerState>(next);
    const uintptr_t thread = _beginthreadex(nullptr, 0, Run, context, 0, nullptr);
    if (!thread) {
      delete context;
      KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
      auto reply = std::move(pending_);
      state_.reset();
      reply->Error("unsupported");
      return;
    }
    worker_ = reinterpret_cast<HANDLE>(thread);
  });
}

unsigned __stdcall HostUpdates::Run(void* context) {
  std::unique_ptr<std::shared_ptr<WorkerState>> owner(
      static_cast<std::shared_ptr<WorkerState>*>(context));
  const auto state = *owner;
  owner.reset();
#ifdef RYHZE_UPDATES_NATIVE_TEST
  if (state->test_entered) SetEvent(state->test_entered);
  if (state->test_release) WaitForSingleObject(state->test_release, INFINITE);
#endif
  {
    std::lock_guard<std::mutex> guard(state->lock);
    if (state->cancelled) return 0;
  }
  ryhze::UpdateStageResult result;
  try {
    result = state->stage ? state->stager.Stage(state->bytes, state->digest)
        : state->stager.Inspect(state->digest, state->size);
  } catch (...) {
    // A write may have started. Preserve its staging record for inspection.
    result = {ryhze::UpdateStageState::recovery, 0};
  }
  state->bytes.clear();
  {
    std::lock_guard<std::mutex> guard(state->lock);
    state->completion = result;
    // Detach holds this same lock before severing the HWND. Parameters convey
    // no pointers: an old queued wakeup cannot act on a reused window handle.
    if (state->window) PostMessageW(state->window, kMessage, 0, 0);
  }
  return 0;
}

void HostUpdates::Cancel(const char* error) {
  if (state_) {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->cancelled = true;
  }
  if (worker_) CancelSynchronousIo(worker_);
  // MethodResult never crosses to the worker, including destruction.
  if (pending_) {
    auto reply = std::move(pending_);
    reply->Error(error);
  }
  // Preserve timer/thread ownership. WinVerifyTrust may outlive this response.
}

bool HostUpdates::ArmTimer() {
  // HWND timer dispatch contains only an opaque ID; no callback captures this.
  return window_ && SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 50, nullptr);
}

void HostUpdates::Poll() {
  if (worker_ && WaitForSingleObject(worker_, 0) == WAIT_OBJECT_0) {
    CloseHandle(worker_); worker_ = nullptr;
    auto completion = std::move(state_);
    auto reply = std::move(pending_);
    auto drained = std::move(drains_);
    drains_.clear();
    if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    if (reply) {
      if (completion && completion->completion) Reply(reply.get(), *completion->completion);
      else reply->Error("recovery");
    }
    for (auto& drain : drained) drain->Success();
  } else if (worker_ && pending_ && GetTickCount64() >= deadline_) {
    // WinVerifyTrust has no cooperative cancellation contract. Its worker may
    // outlive this response, but cannot retain Flutter objects or accept more
    // work. The next request is rejected until that one worker actually exits.
    Cancel("recovery");
  }
}

std::optional<LRESULT> HostUpdates::HandleMessage(UINT message, WPARAM wparam,
                                                LPARAM lparam) {
  (void)lparam;
  const bool timer = message == WM_TIMER && wparam == reinterpret_cast<UINT_PTR>(this);
  if (message != kMessage && !timer) return std::nullopt;
  Poll();
  return 0;
}

void HostUpdates::Detach() {
  closing_ = true;
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  Cancel("cancelled");
  if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
  if (state_) {
    std::lock_guard<std::mutex> guard(state_->lock);
    state_->window = nullptr;
  }
  auto drained = std::move(drains_);
  drains_.clear();
  for (auto& drain : drained) drain->Error("cancelled");
  channel_.reset();
  window_ = nullptr;
  // Poll never waits on the platform thread. A surviving worker retains native
  // state only; reattachment can await it, destruction does not claim a drain.
  Poll();
}

#ifdef RYHZE_UPDATES_NATIVE_TEST
void HostUpdates::SetWorkerGateForTesting(HANDLE entered, HANDLE release) {
  test_entered_ = entered;
  test_release_ = release;
}
void HostUpdates::ExpireForTesting() { deadline_ = GetTickCount64(); }
#endif
