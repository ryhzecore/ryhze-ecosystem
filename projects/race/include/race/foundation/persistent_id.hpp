#pragma once
#include <cstdint>

namespace race {
struct PersistentId {
  std::uint64_t high{}, low{};
  constexpr bool valid() const noexcept { return high || low; }
  friend bool operator==(const PersistentId&, const PersistentId&) = default;
};
} // namespace race
