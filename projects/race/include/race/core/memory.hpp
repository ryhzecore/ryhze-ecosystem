#pragma once
#include "race/foundation/checked.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>

namespace race {
inline constexpr std::size_t memory_owner_count = 16;
inline constexpr std::size_t memory_category_count = 16;
template<std::size_t N> constexpr std::array<std::size_t, N> unlimited_limits() {
  std::array<std::size_t, N> limits{};
  limits.fill(std::numeric_limits<std::size_t>::max()); return limits;
}
struct BudgetConfig {
  std::size_t total{};
  std::array<std::size_t, memory_owner_count> owners = unlimited_limits<memory_owner_count>();
  std::array<std::size_t, memory_category_count> categories = unlimited_limits<memory_category_count>();
};
struct AllocationRequest {
  std::size_t bytes{};
  std::size_t alignment{alignof(std::max_align_t)};
  std::size_t owner{};
  std::size_t category{};
};
struct MemoryAccount {
  std::size_t requested{}; // Cumulative successful requested payload bytes.
  std::size_t live{};
  std::size_t peak{};
  std::size_t reserved{}; // Payload retained by this allocator; not OS pages.
  std::optional<std::size_t> committed; // Unavailable for standard allocation.
};
struct BudgetSnapshot {
  MemoryAccount total;
  std::array<MemoryAccount, memory_owner_count> owners{};
  std::array<MemoryAccount, memory_category_count> categories{};
};
struct AllocatorState;
class TrackedAllocator;
class OwnedBlock {
  std::shared_ptr<AllocatorState> state_;
  void* data_{};
  AllocationRequest request_{};
  std::size_t native_alignment_{};
  OwnedBlock(std::shared_ptr<AllocatorState>, void*, AllocationRequest, std::size_t) noexcept;
  friend class TrackedAllocator;
public:
  OwnedBlock() = default;
  ~OwnedBlock();
  OwnedBlock(const OwnedBlock&) = delete;
  OwnedBlock& operator=(const OwnedBlock&) = delete;
  OwnedBlock(OwnedBlock&&) noexcept;
  OwnedBlock& operator=(OwnedBlock&&) noexcept;
  void reset() noexcept;
  std::byte* data() const noexcept { return static_cast<std::byte*>(data_); }
  std::size_t size() const noexcept { return request_.bytes; }
  explicit operator bool() const noexcept { return data_ != nullptr; }
};
class TrackedAllocator {
  std::shared_ptr<AllocatorState> state_;
  explicit TrackedAllocator(std::shared_ptr<AllocatorState> state) : state_(std::move(state)) {}
public:
  static Result<TrackedAllocator> create(BudgetConfig config);
  Result<OwnedBlock> allocate(AllocationRequest request);
  Result<void> release(OwnedBlock&& block);
  BudgetSnapshot snapshot() const;
};

// Device defaults take an explicit policy fraction, rather than inventing a
// universal engine budget. A project override is still bounded by device bytes.
Result<std::size_t> automatic_memory_budget(std::size_t device_available,
  std::size_t numerator, std::size_t denominator,
  std::optional<std::size_t> project_override = std::nullopt);
Result<std::size_t> reconciled_physical_bytes(std::size_t cpu, std::size_t gpu,
                                           std::size_t shared);

struct ArenaState;
class ScopedArena;
class ArenaLease;
class ArenaToken {
  std::weak_ptr<ArenaState> state_;
  std::uint64_t epoch_{};
  std::size_t offset_{}, bytes_{};
  ArenaToken(std::weak_ptr<ArenaState>, std::uint64_t, std::size_t, std::size_t);
  friend class ArenaLease;
public:
  ArenaToken() = default;
  Result<ArenaLease> acquire() const;
};
class ArenaLease {
  std::shared_ptr<ArenaState> state_;
  std::uint64_t epoch_{};
  std::size_t offset_{}, bytes_{};
  ArenaLease(std::shared_ptr<ArenaState>, std::uint64_t, std::size_t, std::size_t);
  friend class ScopedArena;
  friend class ArenaToken;
public:
  ArenaLease() = default;
  ~ArenaLease();
  ArenaLease(const ArenaLease&) = delete;
  ArenaLease& operator=(const ArenaLease&) = delete;
  ArenaLease(ArenaLease&&) noexcept;
  ArenaLease& operator=(ArenaLease&&) noexcept;
  void release() noexcept;
  std::span<std::byte> bytes() const noexcept;
  ArenaToken token() const;
};
class ScopedArena {
  std::shared_ptr<ArenaState> state_;
  explicit ScopedArena(std::shared_ptr<ArenaState> state) : state_(std::move(state)) {}
public:
  ScopedArena(const ScopedArena&) = delete;
  ScopedArena& operator=(const ScopedArena&) = delete;
  ScopedArena(ScopedArena&&) noexcept = default;
  ScopedArena& operator=(ScopedArena&&) noexcept = default;
  static Result<ScopedArena> create(TrackedAllocator&, std::size_t capacity,
                                   std::size_t owner = 0, std::size_t category = 0);
  Result<ArenaLease> allocate(std::size_t bytes,
                             std::size_t alignment = alignof(std::max_align_t));
  Result<void> reset();
  std::size_t used() const;
};
}
