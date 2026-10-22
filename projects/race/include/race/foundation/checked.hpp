#pragma once
#include "race/foundation/result.hpp"
#include <cstddef>
#include <concepts>
#include <limits>

namespace race {
template<std::unsigned_integral T> Result<T> checked_add(T a, T b) noexcept {
  if (b > std::numeric_limits<T>::max() - a)
    return fail(ErrorCode::Overflow, "unsigned addition overflow");
  return static_cast<T>(a + b);
}
template<std::unsigned_integral T> Result<T> checked_mul(T a, T b) noexcept {
  if (a && b > std::numeric_limits<T>::max() / a)
    return fail(ErrorCode::Overflow, "unsigned multiplication overflow");
  return static_cast<T>(a * b);
}
constexpr bool valid_alignment(std::size_t value) noexcept {
  return value != 0 && (value & (value - 1)) == 0;
}
}
