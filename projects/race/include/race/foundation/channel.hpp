#pragma once
#include "race/foundation/result.hpp"
#include <cstddef>
#include <mutex>
#include <optional>
#include <type_traits>
#include <vector>

namespace race {
struct ChannelSnapshot { std::size_t queued{}, capacity{}, high_water{}, full_count{}; bool closed{}; };
// Bounded MPMC FIFO. Mutex acquisition linearizes accepted messages. No implicit
// blocking, retries or drops. Construction may throw before service activation.
template<class T> class BoundedChannel {
  static_assert(std::is_nothrow_move_constructible_v<T>);
  mutable std::mutex mutex_;
  std::vector<std::optional<T>> slots_;
  std::size_t head_{}, count_{}, high_water_{}, full_count_{};
  bool closed_{};
public:
  explicit BoundedChannel(std::size_t capacity) : slots_(capacity) {}
  Result<void> send(T&& message) {
    std::lock_guard lock(mutex_);
    if (closed_) return fail(ErrorCode::Stopping, "channel closed");
    if (count_ == slots_.size()) {
      if (full_count_ != static_cast<std::size_t>(-1)) ++full_count_;
      return fail(ErrorCode::QueueFull, "channel full; caller retains message");
    }
    slots_[(head_ + count_) % slots_.size()].emplace(std::move(message));
    ++count_; if (count_ > high_water_) high_water_ = count_;
    return {};
  }
  Result<std::optional<T>> receive() {
    std::lock_guard lock(mutex_);
    if (!count_) return std::optional<T>{};
    std::optional<T> result(std::move(*slots_[head_]));
    slots_[head_].reset(); head_ = (head_ + 1) % slots_.size(); --count_;
    return result;
  }
  void close() { std::lock_guard lock(mutex_); closed_ = true; }
  ChannelSnapshot snapshot() const {
    std::lock_guard lock(mutex_);
    return {count_, slots_.size(), high_water_, full_count_, closed_};
  }
};
}
