#pragma once
#include <memory>
#include <utility>

namespace race {
template<class T> class AccessScope {
  std::shared_ptr<T> owner_;
public:
  explicit AccessScope(std::shared_ptr<T> owner) : owner_(std::move(owner)) {}
  T& get() const noexcept { return *owner_; }
  T* operator->() const noexcept { return owner_.get(); }
};
} // namespace race
