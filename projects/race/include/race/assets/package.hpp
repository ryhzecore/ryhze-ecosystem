#pragma once
#include "race/assets/assets.hpp"

namespace race::assets::pak {
// RACEPAK1 v1: a bounded immutable snapshot, codec 0 (stored) only. Each
// product has one chunk (chunk ID 0). Indexed disk streaming is a later profile.
inline constexpr std::size_t header_bytes=64, entry_bytes=112, dependency_bytes=48;
enum class Kind : std::uint32_t { Mesh=1, Scene=2, Texture=3, Material=4 };
struct Key {
  AssetId id;
  Digest revision{}; // SHA-256 of the complete uncompressed product.
  friend bool operator==(const Key&,const Key&)=default;
};
bool less(const Key&,const Key&) noexcept;
struct Entry {
  Key key;
  Kind kind{Kind::Mesh};
  std::uint64_t offset{}, bytes{};
  std::vector<Key> dependencies;
  std::uint32_t schema{1}; // Native product schema; Mesh/Material support 1/2, Texture 1/2/3; Scene stays 1.
};
struct Input {
  AssetId id;
  Kind kind{Kind::Mesh};
  std::span<const std::byte> bytes;
  std::vector<Key> dependencies;
  std::uint32_t schema{1}; // Native product schema; Mesh/Material support 1/2, Texture 1/2/3; Scene stays 1.
};
struct Limits {
  std::size_t entries{128}, dependencies{127}, chunk_bytes{16*1024*1024},
      package_bytes{64*1024*1024};
};
Result<void> validate_limits(const Limits&) noexcept;
// Validates the envelope and every chunk digest before returning any index.
// Domain schemas remain the consumer's responsibility; packaging composition
// validates cooked Scene/mesh/material/texture semantics before admission to a desktop World.
Result<std::vector<Entry>> validate(std::span<const std::byte>,const Limits& = {});
Result<std::vector<std::byte>> encode(std::span<const Input>,const Limits& = {});
struct State;
class Lease {
  std::shared_ptr<const State> state_;
  std::size_t index_{};
  Lease(std::shared_ptr<const State>,std::size_t);
  friend class Package;
public:
  const Entry& entry() const noexcept;
  std::span<const std::byte> bytes() const noexcept;
};
class Package {
  std::shared_ptr<const State> state_;
  explicit Package(std::shared_ptr<const State>);
public:
  // Copies caller bytes, then validates the owned copy. Caller retains its input
  // unchanged for the call. Leases retain the whole bounded snapshot after close.
  static Result<Package> open(std::span<const std::byte>,const Limits& = {});
  // Transfers the entire buffer; the caller must relinquish every mutable alias.
  // Allocated vector capacity must also fit the configured package-byte limit.
  // On failure the transferred storage is released after validation quiesces.
  static Result<Package> open_owned(std::vector<std::byte>,const Limits& = {});
  std::span<const Entry> entries() const noexcept;
  // Encoded snapshot bytes only; index/capability metadata and consumer copies
  // are bounded separately and are not included in this observation.
  std::size_t retained_bytes() const noexcept;
  Result<Lease> acquire(const Key&) const;
  // Read-only canonical <revision>.rasset/.rscene leaf capability. Concurrent
  // reads are independent copies; both writes return Unsupported.
  Result<std::shared_ptr<platform::RootedFiles>> files() const;
};
} // namespace race::assets::pak
