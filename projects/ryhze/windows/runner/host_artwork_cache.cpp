#include "host_artwork_cache.h"
#include <flutter/standard_method_codec.h>
#include <process.h>
#include <atomic>
#include <mutex>
#include <utility>

#ifdef RYHZE_ARTWORK_NATIVE_TEST
// Linked only by the native test target, to gate a real worker deterministically.
extern void RyhzeArtworkWorkerTestGate();
#endif

namespace {
using flutter::EncodableMap;
using flutter::EncodableValue;
const EncodableValue* Field(const EncodableMap& map, const char* name) {
  const auto item = map.find(EncodableValue(name));
  return item == map.end() ? nullptr : &item->second;
}
std::optional<int64_t> Integer(const EncodableValue* value) {
  if (value) {
    if (const auto* number = std::get_if<int32_t>(value)) return *number;
    if (const auto* number = std::get_if<int64_t>(value)) return *number;
  }
  return std::nullopt;
}
const char* Code(ryhze::ArtworkError error) {
  switch (error) {
    case ryhze::ArtworkError::invalid: return "invalidData";
    case ryhze::ArtworkError::unavailable: return "unavailable";
    case ryhze::ArtworkError::conflict: return "conflict";
    case ryhze::ArtworkError::integrity: return "integrity";
    case ryhze::ArtworkError::quota: return "quota";
    case ryhze::ArtworkError::cancelled: return "cancelled";
    case ryhze::ArtworkError::io: return "io";
    case ryhze::ArtworkError::none: return "io";
  }
  return "io";
}
}  // namespace
struct HostArtworkCache::Request {
  std::string method;
  std::string key;
  std::vector<uint8_t> bytes;
  int64_t timestamp = 0;
  std::unique_ptr<Result> reply;
};
struct HostArtworkCache::WorkerState {
  std::shared_ptr<ryhze::ArtworkCache> cache;
  std::unique_ptr<Request> request;
  std::atomic<bool> cancelled{false};
  std::mutex lock;
  HWND window = nullptr;
  ryhze::ArtworkError error = ryhze::ArtworkError::none;
  EncodableValue value;
};
HostArtworkCache::HostArtworkCache(std::optional<std::wstring> parent) : parent_(std::move(parent)) {}
HostArtworkCache::~HostArtworkCache() {
  Detach();
  if (worker_) CloseHandle(worker_);  // A detached worker retains its own native state.
}
void HostArtworkCache::Attach(flutter::BinaryMessenger* messenger, HWND window) {
  Detach();
  window_ = window;
  attached_ = true;
  if (!worker_) cache_ = std::make_shared<ryhze::ArtworkCache>(parent_);
  // Detach cancels notifications and removes the old timer, but cancellation
  // does not mean the OS worker has exited. Re-arm settlement on the new window
  // so an ordered drain cannot depend on another method call to make progress.
  else if (window_ && !SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 25, nullptr)) attached_ = false;
  channel_ = std::make_unique<flutter::MethodChannel<EncodableValue>>(messenger,
      "com.ryhze.ryhze/windows.artwork.v1", &flutter::StandardMethodCodec::GetInstance());
  channel_->SetMethodCallHandler([this](const auto& call, auto reply) {
    Poll();
    const auto& method = call.method_name();
    const auto* args = call.arguments();
    if (method != "probe" && method != "get" && method != "put" && method != "health" &&
        method != "clear" && method != "drain") { reply->NotImplemented(); return; }
    auto request = std::make_unique<Request>();
    request->method = method;
    if (method == "get" || method == "put") {
      const auto* map = args ? std::get_if<EncodableMap>(args) : nullptr;
      const auto* key_value = map ? Field(*map, "key") : nullptr;
      const auto* key = key_value ? std::get_if<std::string>(key_value) : nullptr;
      if (!map || map->size() != (method == "put" ? 3u : 1u) || !key || !ryhze::ArtworkCache::ValidKey(*key)) {
        reply->Error("invalidData"); return;
      }
      request->key = *key;
      if (method == "put") {
        const auto* bytes_value = Field(*map, "bytes");
        const auto* bytes = bytes_value ? std::get_if<std::vector<uint8_t>>(bytes_value) : nullptr;
        const auto timestamp = Integer(Field(*map, "storedAtMs"));
        if (!bytes || bytes->empty() || bytes->size() > ryhze::ArtworkCache::kMaximumEntry ||
            !timestamp || *timestamp < 0 || *timestamp > ryhze::ArtworkCache::kMaximumTimestamp) {
          reply->Error("invalidData"); return;
        }
        if (queued_bytes_ + bytes->size() > 16 * 1024 * 1024) { reply->Error("conflict"); return; }
        request->bytes = *bytes;
        request->timestamp = *timestamp;
      }
    } else if (args && !args->IsNull()) { reply->Error("invalidData"); return; }
    if (!attached_ || !window_ || !cache_) { reply->Error("unavailable"); return; }
    if (queue_.size() >= 8 || (method != "drain" && state_ && state_->cancelled.load())) { reply->Error("conflict"); return; }
    request->reply = std::move(reply);
    queued_bytes_ += request->bytes.size();
    queue_.push_back(std::move(request));
    StartNext();
  });
}
void HostArtworkCache::StartNext() {
  while (attached_ && !worker_ && !queue_.empty()) {
    auto request = std::move(queue_.front()); queue_.pop_front();
    queued_bytes_ -= request->bytes.size();
    // This barrier is reached only after the previous OS worker handle has
    // signalled: cancellation/timeout is never reported as a settled write.
    if (request->method == "drain") { request->reply->Success(); continue; }
    if (!SetTimer(window_, reinterpret_cast<UINT_PTR>(this), 25, nullptr)) {
      request->reply->Error("unavailable"); continue;
    }
    auto state = std::make_shared<WorkerState>();
    state->window = window_;
    state->cache = cache_;
    pending_ = std::move(request->reply);
    state->request = std::move(request);
    state_ = state;
    deadline_ = GetTickCount64() + 30000;
    auto* context = new std::shared_ptr<WorkerState>(state);
    const auto thread = _beginthreadex(nullptr, 0, Run, context, 0, nullptr);
    if (!thread) {
      delete context;
      auto failed = std::move(pending_); failed->Error("unavailable");
      state_.reset(); KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    } else worker_ = reinterpret_cast<HANDLE>(thread);
  }
}
unsigned __stdcall HostArtworkCache::Run(void* context) {
  std::unique_ptr<std::shared_ptr<WorkerState>> owner(static_cast<std::shared_ptr<WorkerState>*>(context));
  const auto state = *owner; owner.reset();
  try {
#ifdef RYHZE_ARTWORK_NATIVE_TEST
    RyhzeArtworkWorkerTestGate();
#endif
    const auto cancel = [state] { return state->cancelled.load(); };
    const auto& request = *state->request;
    auto* error = &state->error;
    if (request.method == "probe") {
      const bool available = !cancel() && state->cache->Ready(error);
      state->value = EncodableValue(EncodableMap{
        {EncodableValue("schemaVersion"), EncodableValue(int32_t{1})},
        {EncodableValue("cache"), EncodableValue(available)}});
      *error = ryhze::ArtworkError::none;
    } else if (request.method == "get") {
      std::optional<ryhze::ArtworkRecord> record;
      if (state->cache->Get(request.key, &record, error, cancel) && record) {
        state->value = EncodableValue(EncodableMap{
          {EncodableValue("bytes"), EncodableValue(std::move(record->bytes))},
          {EncodableValue("storedAtMs"), EncodableValue(record->stored_at_ms)}});
      }
    } else if (request.method == "put") {
      state->cache->Put(request.key, request.bytes, request.timestamp, error, cancel);
    } else if (request.method == "health") {
      ryhze::ArtworkHealth health;
      if (state->cache->Health(&health, error, cancel)) {
        state->value = EncodableValue(EncodableMap{
          {EncodableValue("entries"), EncodableValue(static_cast<int64_t>(health.entries))},
          {EncodableValue("bytes"), EncodableValue(static_cast<int64_t>(health.bytes))},
          {EncodableValue("preserved"), EncodableValue(static_cast<int64_t>(health.preserved))}});
      }
    } else if (request.method == "clear") {
      ryhze::ArtworkClear cleared;
      if (state->cache->Clear(&cleared, error, cancel)) {
        state->value = EncodableValue(EncodableMap{
          {EncodableValue("removed"), EncodableValue(static_cast<int64_t>(cleared.removed))},
          {EncodableValue("preserved"), EncodableValue(static_cast<int64_t>(cleared.preserved))}});
      }
    }
  } catch (...) { state->error = ryhze::ArtworkError::io; state->value = EncodableValue(); }
  std::lock_guard<std::mutex> guard(state->lock);
  if (state->window) PostMessageW(state->window, kMessage, 0, 0);
  return 0;
}
void HostArtworkCache::Poll() {
  if (worker_ && WaitForSingleObject(worker_, 0) == WAIT_OBJECT_0) {
    CloseHandle(worker_); worker_ = nullptr;
    const auto completion = std::move(state_);
    auto pending = std::move(pending_);
    if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
    if (pending) {
      if (completion->error == ryhze::ArtworkError::none) pending->Success(completion->value);
      else pending->Error(Code(completion->error));
    }
    StartNext();
  } else if (pending_ && GetTickCount64() >= deadline_) {
    state_->cancelled.store(true);
    CancelSynchronousIo(worker_);
    auto pending = std::move(pending_); pending->Error("cancelled");
    // Keep timer, OS worker and queued barriers until the worker really exits.
  }
}
std::optional<LRESULT> HostArtworkCache::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
  (void)lparam;
  if (message != kMessage && !(message == WM_TIMER && wparam == reinterpret_cast<UINT_PTR>(this))) return std::nullopt;
  Poll(); return 0;
}
void HostArtworkCache::Detach() {
  attached_ = false;
  if (state_) {
    state_->cancelled.store(true);
    std::lock_guard<std::mutex> guard(state_->lock); state_->window = nullptr;
  }
  if (worker_) CancelSynchronousIo(worker_);
  if (pending_) { auto pending = std::move(pending_); pending->Error("cancelled"); }
  auto queued = std::move(queue_); queue_.clear(); queued_bytes_ = 0;
  for (auto& request : queued) request->reply->Error("cancelled");
  if (window_) KillTimer(window_, reinterpret_cast<UINT_PTR>(this));
  if (channel_) channel_->SetMethodCallHandler(nullptr);
  channel_.reset(); window_ = nullptr;
  Poll();
}
