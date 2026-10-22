#include "race/core/memory.hpp"
#include <algorithm>
#include <mutex>
#include <new>
#include <thread>

namespace race {
struct AllocatorState {
  explicit AllocatorState(BudgetConfig config) : config(std::move(config)) {}
  BudgetConfig config;
  std::mutex mutex;
  BudgetSnapshot accounting;
};
namespace {
void admitted(MemoryAccount& account, std::size_t bytes) {
  account.requested += bytes; account.live += bytes;
  account.reserved += bytes; account.peak = std::max(account.peak, account.live);
}
void released(MemoryAccount& account, std::size_t bytes) {
  if (account.live < bytes || account.reserved < bytes) std::abort();
  account.live -= bytes; account.reserved -= bytes;
}
bool permits(const MemoryAccount& account, std::size_t bytes, std::size_t limit) {
  return account.live <= limit && bytes <= limit - account.live;
}
}
OwnedBlock::OwnedBlock(std::shared_ptr<AllocatorState> state, void* data,
    AllocationRequest request, std::size_t alignment) noexcept
  : state_(std::move(state)), data_(data), request_(request), native_alignment_(alignment) {}
OwnedBlock::~OwnedBlock() { reset(); }
OwnedBlock::OwnedBlock(OwnedBlock&& other) noexcept { *this = std::move(other); }
OwnedBlock& OwnedBlock::operator=(OwnedBlock&& other) noexcept {
  if (this != &other) {
    reset(); state_ = std::move(other.state_); data_ = std::exchange(other.data_, nullptr);
    request_ = std::exchange(other.request_, {}); native_alignment_ = other.native_alignment_;
  }
  return *this;
}
void OwnedBlock::reset() noexcept {
  if (!data_) return;
  // Serialize free with admission: budget capacity is released only after the
  // allocation is no longer live. Retain state until unlocking its mutex.
  auto state = std::move(state_);
  {
    std::lock_guard lock(state->mutex);
    ::operator delete(data_, std::align_val_t(native_alignment_));
    released(state->accounting.total, request_.bytes);
    released(state->accounting.owners[request_.owner], request_.bytes);
    released(state->accounting.categories[request_.category], request_.bytes);
  }
  data_ = nullptr; request_ = {};
}
Result<TrackedAllocator> TrackedAllocator::create(BudgetConfig config) {
  if (!config.total) return fail(ErrorCode::InvalidInput, "zero memory budget");
  try { return TrackedAllocator(std::make_shared<AllocatorState>(std::move(config))); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "allocator state unavailable"); }
}
Result<OwnedBlock> TrackedAllocator::allocate(AllocationRequest request) {
  if (!request.bytes || !valid_alignment(request.alignment) || request.owner >= memory_owner_count || request.category >= memory_category_count)
    return fail(ErrorCode::InvalidInput, "invalid allocation request");
  const auto alignment = std::max(request.alignment, alignof(std::max_align_t));
  if (!checked_add(request.bytes, alignment - 1))
    return fail(ErrorCode::Overflow, "allocation alignment arithmetic overflow");
  std::lock_guard lock(state_->mutex);
  auto& accounts = state_->accounting;
  if (!permits(accounts.total, request.bytes, state_->config.total) ||
      !permits(accounts.owners[request.owner], request.bytes, state_->config.owners[request.owner]) ||
      !permits(accounts.categories[request.category], request.bytes, state_->config.categories[request.category]))
    return fail(ErrorCode::Exhausted, "allocation budget exhausted");
  if (!checked_add(accounts.total.requested, request.bytes) ||
      !checked_add(accounts.owners[request.owner].requested, request.bytes) ||
      !checked_add(accounts.categories[request.category].requested, request.bytes))
    return fail(ErrorCode::Overflow, "allocation accounting overflow");
  auto* data = ::operator new(request.bytes, std::align_val_t(alignment), std::nothrow);
  if (!data) return fail(ErrorCode::Exhausted, "standard allocation failed");
  admitted(accounts.total, request.bytes);
  admitted(accounts.owners[request.owner], request.bytes);
  admitted(accounts.categories[request.category], request.bytes);
  return OwnedBlock(state_, data, request, alignment);
}
Result<void> TrackedAllocator::release(OwnedBlock&& block) {
  if (block.state_ && block.state_ != state_) return fail(ErrorCode::InvalidInput, "foreign allocation owner");
  block.reset(); return {};
}
BudgetSnapshot TrackedAllocator::snapshot() const {
  std::lock_guard lock(state_->mutex); return state_->accounting;
}
Result<std::size_t> automatic_memory_budget(std::size_t available, std::size_t numerator,
    std::size_t denominator, std::optional<std::size_t> override) {
  if (!available || !numerator || !denominator || numerator > denominator)
    return fail(ErrorCode::InvalidInput, "invalid device budget policy");
  if (override) {
    if (!*override || *override > available) return fail(ErrorCode::InvalidInput, "project budget exceeds device allowance");
    return *override;
  }
  auto product = checked_mul(available, numerator);
  if (!product) return product.error();
  const auto value = product.value() / denominator;
  if (!value) return fail(ErrorCode::Exhausted, "device budget rounds to zero");
  return value;
}
Result<std::size_t> reconciled_physical_bytes(std::size_t cpu, std::size_t gpu, std::size_t shared) {
  if (shared > cpu || shared > gpu) return fail(ErrorCode::InvalidInput, "invalid shared physical accounting");
  return checked_add(cpu - shared, gpu);
}
struct ArenaState {
  explicit ArenaState(OwnedBlock block) : backing(std::move(block)), owner(std::this_thread::get_id()) {}
  OwnedBlock backing;
  mutable std::mutex mutex;
  const std::thread::id owner;
  std::uint64_t epoch{1};
  std::size_t used{}, leases{};
};
ArenaToken::ArenaToken(std::weak_ptr<ArenaState> state, std::uint64_t epoch, std::size_t offset, std::size_t bytes)
  : state_(std::move(state)), epoch_(epoch), offset_(offset), bytes_(bytes) {}
Result<ArenaLease> ArenaToken::acquire() const {
  auto state = state_.lock();
  if (!state) return fail(ErrorCode::StaleHandle, "arena owner expired");
  std::lock_guard lock(state->mutex);
  if (state->epoch != epoch_) return fail(ErrorCode::StaleHandle, "stale arena epoch");
  if (state->leases == std::numeric_limits<std::size_t>::max()) return fail(ErrorCode::Overflow, "arena lease count overflow");
  ++state->leases; return ArenaLease(std::move(state), epoch_, offset_, bytes_);
}
ArenaLease::ArenaLease(std::shared_ptr<ArenaState> state, std::uint64_t epoch, std::size_t offset, std::size_t bytes)
  : state_(std::move(state)), epoch_(epoch), offset_(offset), bytes_(bytes) {}
ArenaLease::~ArenaLease() { release(); }
ArenaLease::ArenaLease(ArenaLease&& other) noexcept { *this = std::move(other); }
ArenaLease& ArenaLease::operator=(ArenaLease&& other) noexcept {
  if (this != &other) {
    release(); state_ = std::move(other.state_); epoch_ = other.epoch_;
    offset_ = other.offset_; bytes_ = std::exchange(other.bytes_, 0);
  }
  return *this;
}
void ArenaLease::release() noexcept {
  auto state = std::move(state_);
  if (state) {
    std::lock_guard lock(state->mutex);
    if (!state->leases || state->epoch != epoch_) std::abort();
    --state->leases;
  }
  bytes_ = 0;
}
std::span<std::byte> ArenaLease::bytes() const noexcept {
  if (!state_) return {};
  return {state_->backing.data() + offset_, bytes_};
}
ArenaToken ArenaLease::token() const { return ArenaToken(state_, epoch_, offset_, bytes_); }
Result<ScopedArena> ScopedArena::create(TrackedAllocator& allocator, std::size_t capacity,
    std::size_t owner, std::size_t category) {
  auto backing = allocator.allocate({capacity, alignof(std::max_align_t), owner, category});
  if (!backing) return backing.error();
  try { return ScopedArena(std::make_shared<ArenaState>(std::move(backing).value())); }
  catch (const std::bad_alloc&) { return fail(ErrorCode::Exhausted, "arena metadata unavailable"); }
}
Result<ArenaLease> ScopedArena::allocate(std::size_t bytes, std::size_t alignment) {
  if (std::this_thread::get_id() != state_->owner) return fail(ErrorCode::WrongThread, "arena allocation requires owner thread");
  if (!bytes || !valid_alignment(alignment)) return fail(ErrorCode::InvalidInput, "invalid arena request");
  std::lock_guard lock(state_->mutex);
  auto base = reinterpret_cast<std::uintptr_t>(state_->backing.data());
  auto current = checked_add(base, state_->used);
  if (!current) return current.error();
  auto padded = checked_add(current.value(), alignment - 1);
  if (!padded) return padded.error();
  const auto aligned = padded.value() & ~(alignment - 1);
  const auto offset = aligned - base;
  auto end = checked_add(offset, bytes);
  if (!end) return end.error();
  if (end.value() > state_->backing.size()) return fail(ErrorCode::Exhausted, "arena exhausted");
  if (state_->leases == std::numeric_limits<std::size_t>::max()) return fail(ErrorCode::Overflow, "arena lease count overflow");
  state_->used = end.value(); ++state_->leases;
  return ArenaLease(state_, state_->epoch, offset, bytes);
}
Result<void> ScopedArena::reset() {
  if (std::this_thread::get_id() != state_->owner) return fail(ErrorCode::WrongThread, "arena reset requires owner thread");
  std::lock_guard lock(state_->mutex);
  if (state_->leases) return fail(ErrorCode::Busy, "arena still has consumers");
  if (state_->epoch == std::numeric_limits<std::uint64_t>::max()) return fail(ErrorCode::Overflow, "arena epoch exhausted");
  ++state_->epoch; state_->used = 0; return {};
}
std::size_t ScopedArena::used() const { std::lock_guard lock(state_->mutex); return state_->used; }
}
