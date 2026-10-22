#include "race/assets/mesh.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <thread>

namespace race::assets::mesh {
struct Accounting {
  mutable std::mutex mutex;
  Limits limits;
  Stats counters;
  explicit Accounting(Limits l) : limits(l) {
    counters.entry_capacity = counters.handle_capacity = l.assets;
  }
  bool reserve(std::size_t bytes) {
    std::lock_guard lock(mutex);
    if (bytes > limits.retained_bytes - counters.retained_bytes ||
        counters.retained_revisions == limits.retained_revisions) return false;
    counters.retained_bytes += bytes; ++counters.retained_revisions;
    counters.peak_retained_bytes = std::max(counters.peak_retained_bytes, counters.retained_bytes);
    counters.peak_retained_revisions = std::max(counters.peak_retained_revisions, counters.retained_revisions);
    return true;
  }
  void release(std::size_t bytes) {
    std::lock_guard lock(mutex);
    counters.retained_bytes -= bytes; --counters.retained_revisions;
  }
};
namespace {
struct Reservation {
  std::shared_ptr<Accounting> accounting;
  std::size_t bytes;
  bool armed{true};
  ~Reservation() { if (armed) accounting->release(bytes); }
};
struct Entry {
  AssetId id{};
  std::uint64_t pending{};
  Stamp stamp{};
  ResourceHandle current{};
};
} // namespace
struct StoreState {
  Limits limits;
  TrackedAllocator allocator;
  std::thread::id owner{std::this_thread::get_id()};
  std::uint64_t next{1};
  std::shared_ptr<Accounting> accounting;
  std::vector<Entry> entries;
  HandleTable<const Revision> handles;
  StoreState(Limits l, TrackedAllocator a)
      : limits(l), allocator(std::move(a)), accounting(std::make_shared<Accounting>(l)),
        entries(l.assets), handles(l.assets) {}
  Entry *find(AssetId id) {
    for (auto &entry : entries) if (entry.id == id) return &entry;
    return nullptr;
  }
  bool owns_thread() const noexcept { return owner == std::this_thread::get_id(); }
};
Revision::Revision(std::shared_ptr<Accounting> accounting, OwnedBlock block, View view)
    : accounting_(std::move(accounting)), block_(std::move(block)), view_(view) {}
Revision::~Revision() {
  const auto bytes = block_.size(); block_.reset(); accounting_->release(bytes);
}
Update::Update(std::weak_ptr<StoreState> owner, AssetId id, Stamp stamp, std::uint64_t serial)
    : owner_(std::move(owner)), id_(id), stamp_(stamp), serial_(serial) {}
Store::Store(std::shared_ptr<StoreState> state) : state_(std::move(state)) {}
Store::~Store() = default;
Result<std::unique_ptr<Store>> Store::create(Limits limits, TrackedAllocator allocator) {
  auto valid = validate_limits(limits); if (!valid) return valid.error();
  try { return std::unique_ptr<Store>(new Store(std::make_shared<StoreState>(limits, std::move(allocator)))); }
  catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "mesh store allocation failed"); }
}
Result<Update> Store::begin_update(AssetId id, const Stamp &stamp) {
  if (!state_->owns_thread()) return fail(ErrorCode::WrongThread, "mesh update requires owner thread");
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "mesh asset identity is zero");
  auto valid = validate_stamp(stamp); if (!valid) return valid.error();
  if (state_->next == std::numeric_limits<std::uint64_t>::max())
    return fail(ErrorCode::Exhausted, "mesh update identities exhausted");
  auto *entry = state_->find(id);
  if (!entry) {
    entry = state_->find({});
    if (!entry) return fail(ErrorCode::Exhausted, "mesh asset slots exhausted");
  }
  entry->id = id; entry->stamp = stamp; entry->pending = state_->next++;
  return Update(state_, id, stamp, entry->pending);
}
Result<ResourceHandle> Store::publish(const Update &update, std::span<const std::byte> bytes) {
  if (!state_->owns_thread()) return fail(ErrorCode::WrongThread, "mesh publication requires owner thread");
  if (update.owner_.lock() != state_)
    return fail(ErrorCode::StaleHandle, "foreign or expired mesh update");
  auto *entry = state_->find(update.id_);
  if (!entry || !entry->pending || entry->pending != update.serial_ || entry->stamp != update.stamp_)
    return fail(ErrorCode::StaleHandle, "obsolete mesh update");
  // One terminal publication attempt. Failed preparation preserves the current
  // revision, but the failed request cannot subsequently install other bytes.
  entry->pending = 0;
  if (bytes.size() > state_->limits.product_bytes)
    return fail(ErrorCode::Exhausted, "mesh product budget exceeded");
  if (bytes.size() < header_bytes + checksum_bytes)
    return fail(ErrorCode::InvalidInput, "mesh product is truncated");
  auto accounting = state_->accounting;
  if (!accounting->reserve(bytes.size())) return fail(ErrorCode::Exhausted, "mesh residency budget exceeded");
  Reservation reservation{accounting, bytes.size()};
  try {
    auto storage = state_->allocator.allocate({bytes.size(), alignof(std::max_align_t), 3, 2});
    if (!storage) return storage.error();
    auto block = std::move(storage).value();
    std::memcpy(block.data(), bytes.data(), bytes.size());
    auto validated = validate({block.data(), block.size()}, state_->limits);
    if (!validated) return validated.error();
    if (validated.value().id() != update.id_ || validated.value().stamp() != update.stamp_)
      return fail(ErrorCode::InvalidInput, "mesh completion provenance differs from admission");
    auto object = std::unique_ptr<Revision>(new Revision(accounting, std::move(block), validated.value()));
    reservation.armed = false; // Revision now owns the charge, including control-block failure.
    std::shared_ptr<const Revision> revision(std::move(object));
    const std::array removed{entry->current};
    const std::array inserted{revision};
    auto replaced = state_->handles.replace_batch(
        std::span<const ResourceHandle>(removed).first(entry->current.table ? 1 : 0), inserted);
    if (!replaced) return replaced.error();
    entry->current = replaced.value()[0];
    return entry->current;
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "mesh publication allocation failed"); }
}
Result<void> Store::cancel(const Update &update) {
  if (!state_->owns_thread()) return fail(ErrorCode::WrongThread, "mesh cancellation requires owner thread");
  if (update.owner_.lock() != state_) return fail(ErrorCode::StaleHandle, "foreign or expired mesh update");
  auto *entry = state_->find(update.id_);
  if (!entry || !entry->pending || entry->pending != update.serial_)
    return fail(ErrorCode::StaleHandle, "obsolete mesh update");
  entry->pending = 0;
  return {};
}
Result<ResourceHandle> Store::current(AssetId id) const {
  if (!state_->owns_thread()) return fail(ErrorCode::WrongThread, "mesh current lookup requires owner thread");
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "mesh asset identity is zero");
  auto *entry = state_->find(id);
  if (!entry || !entry->current.table) return fail(ErrorCode::NotFound, "mesh has no current revision");
  return entry->current;
}
Result<Lease> Store::acquire(ResourceHandle handle) const { return state_->handles.resolve(handle); }
Result<void> Store::retire(AssetId id) {
  if (!state_->owns_thread()) return fail(ErrorCode::WrongThread, "mesh retirement requires owner thread");
  if (!id.valid()) return fail(ErrorCode::InvalidInput, "mesh asset identity is zero");
  auto *entry = state_->find(id);
  if (!entry) return fail(ErrorCode::NotFound, "mesh asset not found");
  if (entry->current.table) {
    auto erased = state_->handles.erase(entry->current); if (!erased) return erased;
  }
  *entry = {};
  return {};
}
Stats Store::stats() const {
  std::lock_guard lock(state_->accounting->mutex);
  return state_->accounting->counters;
}
} // namespace race::assets::mesh
