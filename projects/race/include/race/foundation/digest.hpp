#pragma once
#include <array>
#include <cstddef>
#include <span>

namespace race {
using Digest256 = std::array<std::byte, 32>;
// SHA-256, allocation-free. Identity/integrity only; this is not authentication.
Digest256 sha256(std::span<const std::byte>) noexcept;
} // namespace race
