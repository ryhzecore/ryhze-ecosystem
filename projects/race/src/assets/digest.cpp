#include "race/assets/assets.hpp"
#include "race/foundation/digest.hpp"

namespace race::assets {
Digest digest(std::span<const std::byte> input) noexcept { return race::sha256(input); }
} // namespace race::assets
