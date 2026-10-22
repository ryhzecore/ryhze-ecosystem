#pragma once
#include "race/foundation/access_scope.hpp"
#include "race/foundation/persistent_id.hpp"
#include "race/foundation/result.hpp"
#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace race {
// Supplied IDs are preserved, never inferred from names or process addresses.
// This bounded fixture registry is not an asset database or ID generator.
class PersistentRegistry {
  struct Entry { PersistentId id; std::string name; };
  std::vector<Entry> entries_;
  std::size_t capacity_;
public:
  explicit PersistentRegistry(std::size_t capacity) : capacity_(capacity) { entries_.reserve(capacity); }
  Result<void> add(PersistentId id, std::string_view name) {
    if (!id.valid() || name.empty() || name.size() > 255)
      return fail(ErrorCode::InvalidInput, "invalid persistent record");
    for (const auto& e : entries_) if (e.id == id) return fail(ErrorCode::Duplicate, "duplicate persistent ID");
    if (entries_.size() == capacity_) return fail(ErrorCode::Exhausted, "persistent registry full");
    try { entries_.push_back({id, std::string(name)}); }
    catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "persistent record allocation failed"); }
    return {};
  }
  Result<void> rename(PersistentId id, std::string_view name) {
    if (name.empty() || name.size() > 255) return fail(ErrorCode::InvalidInput, "invalid persistent name");
    for (auto& e : entries_) if (e.id == id) {
      try { std::string candidate(name); e.name.swap(candidate); }
      catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "persistent rename allocation failed"); }
      return {};
    }
    return fail(ErrorCode::NotFound, "persistent ID not found");
  }
  Result<std::string_view> name(PersistentId id) const {
    for (const auto& e : entries_) if (e.id == id) return std::string_view(e.name);
    return fail(ErrorCode::NotFound, "persistent ID not found");
  }
};

template<class T> struct Handle {
  std::uint32_t slot{};
  std::uint64_t generation{};
  std::uint64_t table{};
  friend bool operator==(const Handle&, const Handle&) = default;
};
inline std::atomic<std::uint64_t> next_handle_table{1};

// Generation is configurable only to exercise wrap retirement in bounded tests.
// Erase invalidates future resolution; extant access scopes retain the object.
// The table lock protects identity/lifetime, not concurrent mutations of T.
template<class T, class Generation = std::uint32_t> class HandleTable {
  static_assert(std::is_unsigned_v<Generation>);
  struct Slot { std::shared_ptr<T> value; Generation generation{1}; bool retired{}; };
  mutable std::mutex mutex_;
  std::vector<Slot> slots_;
  std::uint64_t id_;
  bool matches(Handle<T> h) const noexcept {
    return h.table == id_ && h.slot < slots_.size() &&
           slots_[h.slot].value && slots_[h.slot].generation == h.generation;
  }
public:
  explicit HandleTable(std::size_t capacity) : slots_(capacity), id_(next_handle_table.fetch_add(1)) {
    // Exhausting table identities is an invariant, never silently wrap/reuse.
    if (!id_ || id_ == std::numeric_limits<std::uint64_t>::max() || capacity > std::numeric_limits<std::uint32_t>::max()) std::abort();
  }
  // Observation, not a reservation. A caller may preflight a batch only when it
  // exclusively owns every insertion/erasure of this table across publication.
  // Retired generations never count as reusable capacity.
  std::size_t available_slots() const {
    std::lock_guard lock(mutex_);
    std::size_t result = 0;
    for (const auto& slot : slots_) if (!slot.value && !slot.retired) ++result;
    return result;
  }
  // Atomic identity publication for an exclusive higher-layer transaction.
  // All validation/allocation precedes mutation; removed owners die outside the
  // table mutex. Retained scopes remain valid, but old handles never revive.
  Result<std::vector<Handle<T>>> replace_batch(
      std::span<const Handle<T>> erased, std::span<const std::shared_ptr<T>> inserted) {
    if (erased.size() > slots_.size() || inserted.size() > slots_.size())
      return fail(ErrorCode::Exhausted, "handle batch capacity exceeded");
    try {
      std::vector<bool> removed(slots_.size(), false);
      std::vector<Handle<T>> result; result.reserve(inserted.size());
      std::vector<std::shared_ptr<T>> retired; retired.reserve(erased.size());
      {
        std::lock_guard lock(mutex_);
        for (auto h : erased) {
          if (!matches(h)) return fail(ErrorCode::StaleHandle, "invalid erased handle");
          if (removed[h.slot]) return fail(ErrorCode::Duplicate, "duplicate erased handle");
          removed[h.slot] = true;
        }
        for (const auto& owner : inserted)
          if (!owner) return fail(ErrorCode::InvalidInput, "null batch owner");
        for (std::size_t i=0; i<slots_.size() && result.size()<inserted.size(); ++i) {
          const auto& slot=slots_[i];
          if (slot.retired || (slot.value && !removed[i]) ||
              (removed[i] && slot.generation==std::numeric_limits<Generation>::max())) continue;
          result.push_back({static_cast<std::uint32_t>(i),
              static_cast<std::uint64_t>(slot.generation)+(removed[i]?1U:0U),id_});
        }
        if (result.size()!=inserted.size()) return fail(ErrorCode::Exhausted, "handle batch exhausted");
        // From here onward every operation is noexcept and capacity is reserved.
        for (auto h : erased) {
          auto& slot=slots_[h.slot]; retired.push_back(std::move(slot.value));
          if (slot.generation==std::numeric_limits<Generation>::max()) slot.retired=true;
          else ++slot.generation;
        }
        for (std::size_t i=0; i<inserted.size(); ++i) slots_[result[i].slot].value=inserted[i];
      }
      return result;
    } catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "handle batch allocation failed"); }
  }
  Result<Handle<T>> insert(std::shared_ptr<T> owner) {
    if (!owner) return fail(ErrorCode::InvalidInput, "null handle owner");
    std::lock_guard lock(mutex_);
    for (std::size_t i = 0; i < slots_.size(); ++i) {
      auto& slot = slots_[i];
      if (!slot.value && !slot.retired) {
        slot.value = std::move(owner);
        return Handle<T>{static_cast<std::uint32_t>(i), slot.generation, id_};
      }
    }
    return fail(ErrorCode::Exhausted, "handle table exhausted");
  }
  Result<AccessScope<T>> resolve(Handle<T> handle) const {
    std::lock_guard lock(mutex_);
    if (!matches(handle)) return fail(ErrorCode::StaleHandle, "invalid runtime handle");
    return AccessScope<T>(slots_[handle.slot].value);
  }
  Result<void> erase(Handle<T> handle) {
    std::shared_ptr<T> retired_owner;
    {
      std::lock_guard lock(mutex_);
      if (!matches(handle)) return fail(ErrorCode::StaleHandle, "invalid runtime handle");
      auto& slot = slots_[handle.slot]; retired_owner = std::move(slot.value);
      if (slot.generation == std::numeric_limits<Generation>::max()) slot.retired = true;
      else ++slot.generation;
    }
    // User destruction executes outside the identity mutex.
    return {};
  }
};
}
