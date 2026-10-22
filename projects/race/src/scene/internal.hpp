#pragma once
#include "race/foundation/channel.hpp"
#include "race/scene/scene.hpp"
#include <algorithm>
#include <bit>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace race::scene {
constexpr std::uint32_t none = UINT32_MAX;
struct IdHash {
  std::size_t operator()(PersistentId id) const noexcept {
    auto x = id.low ^ (id.high + 0x9e3779b97f4a7c15ULL + (id.low << 6) + (id.low >> 2));
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    return static_cast<std::size_t>(x ^ (x >> 31));
  }
};
inline bool id_less(PersistentId a, PersistentId b) {
  return a.high < b.high || (a.high == b.high && a.low < b.low);
}
template <class T> class PoolArray {
  OwnedBlock block_;
  T *data_{};
  std::size_t count_{};

public:
  PoolArray() = default;
  PoolArray(const PoolArray &) = delete;
  PoolArray &operator=(const PoolArray &) = delete;
  PoolArray(PoolArray &&o) noexcept
      : block_(std::move(o.block_)), data_(std::exchange(o.data_, nullptr)),
        count_(std::exchange(o.count_, 0)) {}
  PoolArray &operator=(PoolArray &&o) noexcept {
    if (this != &o) {
      clear();
      block_ = std::move(o.block_);
      data_ = std::exchange(o.data_, nullptr);
      count_ = std::exchange(o.count_, 0);
    }
    return *this;
  }
  ~PoolArray() { clear(); }
  void clear() noexcept {
    for (std::size_t i = 0; i < count_; ++i)
      std::destroy_at(data_ + i);
    count_ = 0;
    data_ = nullptr;
    block_.reset();
  }
  Result<void> initialize(TrackedAllocator &a, std::size_t count) {
    auto bytes = checked_mul(count, sizeof(T));
    if (!bytes)
      return bytes.error();
    auto b = a.allocate({bytes.value(), alignof(T), 3, 0});
    if (!b)
      return b.error();
    block_ = std::move(b).value();
    data_ = reinterpret_cast<T *>(block_.data());
    for (; count_ < count; ++count_)
      std::construct_at(data_ + count_);
    return {};
  }
  T &operator[](std::size_t i) noexcept { return data_[i]; }
  const T &operator[](std::size_t i) const noexcept { return data_[i]; }
  T *data() noexcept { return data_; }
  const T *data() const noexcept { return data_; }
  std::size_t bytes() const noexcept { return block_.size(); }
};
struct PackedBinding {
  bool bound{};
  PersistentId instance;
  TemplateKey pinned;
  TemplatePath path;
  std::array<std::uint16_t, max_types> overrides{};
};
struct Node {
  bool alive{};
  ObjectHandle handle;
  PersistentId id, section;
  PackedBinding binding;
  Affine derived;
  std::uint32_t parent{none}, first_child{none}, next{none}, previous{none}, active_index{none},
      depth{};
};
struct IdEntry {
  PersistentId id;
  std::uint32_t slot{none};
  bool occupied{}, tombstone{};
};
struct Pool {
  PoolArray<ComponentRow> rows;
  PoolArray<std::uint32_t> sparse;
  std::size_t count{};
};
struct Delta {
  PersistentId id;
  std::optional<Object> before, after;
};
struct History {
  std::vector<Delta> delta;
  std::size_t bytes{};
};
struct Pending {
  std::uint64_t ticket{};
  Transaction transaction;
  std::size_t bytes{};
};
struct SectionRecord {
  SectionInfo info;
  std::uint64_t ticket{};
  bool preparing{}, cancelled{}, ready{};
  std::vector<Object> prepared;
  std::size_t bytes{};
};
inline std::atomic<std::uint64_t> next_world_identity{1};
struct WorldState {
  mutable std::mutex mutex;
  std::condition_variable readers_done;
  std::thread::id owner{std::this_thread::get_id()};
  std::shared_ptr<const Schemas> schemas;
  std::shared_ptr<const Templates> templates;
  Limits limits;
  HandleTable<ObjectIdentity, std::uint64_t> handles;
  PoolArray<Node> nodes;
  PoolArray<IdEntry> ids;
  std::size_t id_capacity{};
  std::vector<Pool> pools;
  std::vector<ObjectHandle> active;
  std::unique_ptr<BoundedChannel<Pending>> commands;
  std::vector<CommandCompletion> completions;
  std::size_t readers{}, queued{}, outstanding{}, pending_bytes{}, components{}, history_bytes{},
      prepared_bytes{}, pool_bytes{};
  std::vector<History> history;
  std::size_t cursor{};
  std::vector<SectionRecord> sections;
  std::vector<PersistentId> stopped_sections;
  const std::uint64_t identity{next_world_identity.fetch_add(1)};
  std::uint64_t next_preparation{1};
  std::uint64_t next_ticket{1}, generation{1}, recomputations{}, touched{}, publications{};
  LocalOrigin origin;
  bool stopping{}, publication_pending{}, frame_inflight{}, execution_pending{};
  WorldState(std::shared_ptr<const Schemas> s, Limits l, std::shared_ptr<const Templates> t)
      : schemas(std::move(s)), templates(std::move(t)), limits(l), handles(l.objects) {
    if (!identity || identity == UINT64_MAX)
      std::abort();
  }
  Result<void> initialize(TrackedAllocator &);
  std::uint32_t find(PersistentId) const noexcept;
  void index_insert(PersistentId, std::uint32_t) noexcept;
  void index_erase(PersistentId) noexcept;
  Result<std::uint32_t> resolve(ObjectHandle) const;
  Result<Object> snapshot(std::uint32_t) const;
  const ComponentRow *row(std::uint32_t, TypeId) const noexcept;
  SectionRecord *section(PersistentId) noexcept;
  const SectionRecord *section(PersistentId) const noexcept;
  bool view_gate_closed() const noexcept;
  Result<void> owner_check() const noexcept;
  Result<void> apply_locked(const Transaction &, bool record_history = true, bool overrides = true,
                            bool bypass_gate = false, bool administrative = false,
                            const std::vector<SectionRecord> *candidate_sections = nullptr);
};
Result<void> validate_limits(Limits);
Result<void> normalize(Object &, const Schemas &, Limits);
std::size_t object_bytes(const Object &) noexcept;
Result<std::size_t> transaction_bytes(const Transaction &, Limits) noexcept;
const Component *component(const Object &, TypeId) noexcept;
Component *component(Object &, TypeId) noexcept;
const Value *field(const Component &, FieldId) noexcept;
LocalTransform local_transform(const Component &);
Reference parent_reference(const Component &);
Result<PackedBinding> pack_binding(const std::optional<Binding> &, const Schemas &);
std::optional<Binding> unpack_binding(const PackedBinding &, const Schemas &);
struct Expanded {
  Object object;
  TemplatePath path;
};
Result<std::vector<Expanded>> expand_template(const Templates &, TemplateKey, const Schemas &,
                                              Limits);
Result<void> validate_candidate_bindings(const WorldState &, const std::vector<Delta> &);
Result<void> validate_document(const Document &, const Schemas &, Limits,
                               bool activation_dependencies = true);
} // namespace race::scene
