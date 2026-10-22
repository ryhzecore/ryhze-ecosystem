#pragma once
#include "race/core/memory.hpp"
#include "race/platform/platform.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>

namespace race::fixtures {
// One request/one completion, on a dedicated Platform thread. This is an owned
// cancellation protocol fixture, not a disk/network implementation.
struct IoCompletion { OwnedBlock buffer; bool cancelled; };
class MockIo {
  struct State {
    std::mutex mutex;
    std::condition_variable changed;
    std::atomic<bool> cancel{false};
    OwnedBlock buffer;
    bool done = false, consumed = false, terminal_cancelled = false;
  };
  std::shared_ptr<State> state_;
  std::unique_ptr<platform::Thread> thread_;
  std::thread::id owner_ = std::this_thread::get_id();
  MockIo(std::shared_ptr<State> state, std::unique_ptr<platform::Thread> thread)
    : state_(std::move(state)), thread_(std::move(thread)) {}
public:
  static Result<std::unique_ptr<MockIo>> begin(platform::Threads& threads, OwnedBlock buffer) {
    if (!buffer) return fail(ErrorCode::InvalidInput,"mock I/O buffer required");
    try {
      auto state = std::make_shared<State>(); state->buffer = std::move(buffer);
      auto worker = threads.start([state] {
        std::unique_lock lock(state->mutex);
        // Models pending I/O without occupying a CPU worker. Cancellation wakes it.
        state->changed.wait_for(lock,std::chrono::milliseconds(2),[&]{return state->cancel.load();});
        if (!state->cancel.load()) state->buffer.data()[0] = std::byte{42};
        state->terminal_cancelled = state->cancel.load(); state->done = true; lock.unlock(); state->changed.notify_all();
      });
      if (!worker) return worker.error();
      return std::unique_ptr<MockIo>(new MockIo(state,std::move(worker).value()));
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted,"mock I/O owner allocation"); }
  }
  ~MockIo() { cancel(); auto joined = thread_->join(); if (!joined) std::abort(); }
  void cancel() noexcept { state_->cancel.store(true); state_->changed.notify_all(); }
  Result<void> wait(std::chrono::milliseconds timeout) {
    if (timeout.count() < 0) return fail(ErrorCode::InvalidInput,"negative mock I/O timeout");
    std::unique_lock lock(state_->mutex);
    if (!state_->changed.wait_for(lock,timeout,[&]{ return state_->done; })) return fail(ErrorCode::Timeout,"mock I/O pending");
    return {};
  }
  Result<std::optional<IoCompletion>> take() {
    if (std::this_thread::get_id() != owner_) return fail(ErrorCode::WrongThread,"completion belongs to dispatch owner");
    std::lock_guard lock(state_->mutex);
    if (!state_->done || state_->consumed) return std::optional<IoCompletion>{};
    state_->consumed = true;
    return std::optional<IoCompletion>(IoCompletion{std::move(state_->buffer),state_->terminal_cancelled});
  }
};
}
