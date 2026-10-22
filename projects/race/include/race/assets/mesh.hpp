#pragma once
#include "race/assets/assets.hpp"

namespace race::assets::mesh {
// Native .rasset registry entry: RACEAST1 / container 1 / static mesh kind 1 /
// schema 1 (position/normal) or 2 (also UV). Existing containers are independent.
inline constexpr std::size_t header_bytes = 256, checksum_bytes = 32, vertex_stride = 48;
inline constexpr std::size_t uv_vertex_stride = 64;
inline constexpr std::uint32_t portable_cpu_target = 1;
enum class Representation : std::uint32_t { Imported = 1, Cooked = 2 };
struct Stamp {
  Representation representation{Representation::Imported};
  std::uint32_t target{};
  Digest source{}, settings{}, importer{}, cooker{};
  // Schema 1 has no external dependencies; this must be SHA-256(empty).
  Digest dependencies{digest({})};
  friend bool operator==(const Stamp &, const Stamp &) = default;
};
struct Vertex {
  std::array<double, 3> position{}, normal{};
  std::array<double, 2> uv{}; // Schema2: top-left origin; finite, abs<=1e12.
  friend bool operator==(const Vertex &, const Vertex &) = default;
};
struct Bounds {
  std::array<double, 3> minimum{}, maximum{};
  friend bool operator==(const Bounds &, const Bounds &) = default;
};
struct Limits {
  std::size_t vertices{65536}, indices{196608}, product_bytes{16 * 1024 * 1024};
  std::size_t assets{64}, retained_revisions{128}, retained_bytes{64 * 1024 * 1024};
};
Result<void> validate_limits(const Limits &) noexcept;
Result<void> validate_stamp(const Stamp &) noexcept;

// A validated borrowed view. The caller keeps the original bytes alive and
// unchanged for its entire use. Store publication copies bytes before validation.
class View {
  AssetId id_{};
  Stamp stamp_{};
  Digest revision_{};
  Bounds bounds_{};
  std::size_t vertices_{}, indices_{};
  bool has_uv_{};
  std::span<const std::byte> bytes_;
  View(AssetId, Stamp, Digest, Bounds, std::size_t, std::size_t,
       std::span<const std::byte>,bool);
  friend Result<View> validate(std::span<const std::byte>, const Limits &);
public:
  AssetId id() const noexcept { return id_; }
  const Stamp &stamp() const noexcept { return stamp_; }
  Digest revision() const noexcept { return revision_; }
  Bounds bounds() const noexcept { return bounds_; }
  std::size_t vertex_count() const noexcept { return vertices_; }
  std::size_t index_count() const noexcept { return indices_; }
  bool has_uv() const noexcept { return has_uv_; }
  std::span<const std::byte> bytes() const noexcept { return bytes_; }
  Result<Vertex> vertex(std::size_t) const noexcept;
  Result<std::uint32_t> index(std::size_t) const noexcept;
};
Result<View> validate(std::span<const std::byte>, const Limits & = {});
Result<std::vector<std::byte>> encode(AssetId, const Stamp &, std::span<const Vertex>,
                                    std::span<const std::uint32_t>, const Limits & = {},bool has_uv=false);

struct Accounting;
struct StoreState;
class Revision {
  std::shared_ptr<Accounting> accounting_;
  OwnedBlock block_;
  View view_;
  Revision(std::shared_ptr<Accounting>, OwnedBlock, View);
  friend class Store;
public:
  ~Revision();
  Revision(const Revision &) = delete;
  const View &view() const noexcept { return view_; }
};
using ResourceHandle = Handle<const Revision>;
using Lease = AccessScope<const Revision>;
class Update {
  std::weak_ptr<StoreState> owner_;
  AssetId id_{};
  Stamp stamp_{};
  std::uint64_t serial_{};
  Update(std::weak_ptr<StoreState>, AssetId, Stamp, std::uint64_t);
  friend class Store;
public:
  AssetId id() const noexcept { return id_; }
  const Stamp &stamp() const noexcept { return stamp_; }
  std::uint64_t serial() const noexcept { return serial_; }
};
struct Stats {
  std::size_t retained_bytes{}, peak_retained_bytes{}, retained_revisions{},
      peak_retained_revisions{}, entry_capacity{}, handle_capacity{};
};
// Admission/publication/current/cancel/retire are owner-thread operations.
// acquire and stats may run concurrently with owner operations. Leases can
// outlive replacement, retirement and the Store itself. This owns no workers:
// executable composition must retain any worker inputs through quiescence.
class Store {
  std::shared_ptr<StoreState> state_;
  explicit Store(std::shared_ptr<StoreState>);
public:
  static Result<std::unique_ptr<Store>> create(Limits, TrackedAllocator);
  Store(const Store &) = delete;
  ~Store();
  Result<Update> begin_update(AssetId, const Stamp &);
  Result<ResourceHandle> publish(const Update &, std::span<const std::byte>);
  Result<void> cancel(const Update &);
  Result<ResourceHandle> current(AssetId) const;
  Result<Lease> acquire(ResourceHandle) const;
  Result<void> retire(AssetId);
  Stats stats() const;
};
} // namespace race::assets::mesh
