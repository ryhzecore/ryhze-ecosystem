#include "test.hpp"
#include "race/core/memory.hpp"
#include <atomic>
#include <cstring>
#include <limits>
#include <thread>
#include <vector>

using namespace race;
namespace {
template<class T> void error_is(const Result<T>& result, ErrorCode code) {
  CHECK(!result); CHECK(result.error().code == code);
}
}
void memory_tests() {
  error_is(checked_add(std::numeric_limits<std::size_t>::max(), std::size_t{1}), ErrorCode::Overflow);
  error_is(checked_mul(std::numeric_limits<std::size_t>::max(), std::size_t{2}), ErrorCode::Overflow);
  CHECK(checked_mul(std::size_t{0}, std::numeric_limits<std::size_t>::max()).value() == 0);
  CHECK(automatic_memory_budget(1000, 1, 4).value() == 250);
  CHECK(automatic_memory_budget(1000, 1, 4, 700).value() == 700);
  error_is(automatic_memory_budget(1000, 1, 4, 1001), ErrorCode::InvalidInput);
  error_is(automatic_memory_budget(1, 1, 4), ErrorCode::Exhausted);
  error_is(automatic_memory_budget(1000, 1, 0), ErrorCode::InvalidInput);
  CHECK(reconciled_physical_bytes(100, 80, 60).value() == 120);
  CHECK(reconciled_physical_bytes(100, 100, 100).value() == 100);
  CHECK(reconciled_physical_bytes(std::numeric_limits<std::size_t>::max(), 1, 1).value() == std::numeric_limits<std::size_t>::max());
  error_is(reconciled_physical_bytes(10, 20, 11), ErrorCode::InvalidInput);
  error_is(reconciled_physical_bytes(std::numeric_limits<std::size_t>::max(), 1, 0), ErrorCode::Overflow);

  BudgetConfig config{2048}; config.owners[2] = 512; config.categories[3] = 128;
  auto allocator_result = TrackedAllocator::create(config); CHECK(allocator_result);
  auto allocator = std::move(allocator_result).value();
  error_is(TrackedAllocator::create({0}), ErrorCode::InvalidInput);
  const auto before = allocator.snapshot();
  error_is(allocator.allocate({0}), ErrorCode::InvalidInput);
  error_is(allocator.allocate({1, 3}), ErrorCode::InvalidInput);
  error_is(allocator.allocate({1, 0}), ErrorCode::InvalidInput);
  error_is(allocator.allocate({1, 16, memory_owner_count, 0}), ErrorCode::InvalidInput);
  error_is(allocator.allocate({1, 16, 0, memory_category_count}), ErrorCode::InvalidInput);
  error_is(allocator.allocate({std::numeric_limits<std::size_t>::max(), 64}), ErrorCode::Overflow);
  error_is(allocator.allocate({2049}), ErrorCode::Exhausted);
  error_is(allocator.allocate({513, 16, 2}), ErrorCode::Exhausted);
  error_is(allocator.allocate({129, 16, 0, 3}), ErrorCode::Exhausted);
  CHECK(allocator.snapshot().total.requested == before.total.requested);
  CHECK(allocator.snapshot().total.live == 0);
  CHECK(!allocator.snapshot().total.committed.has_value());
  for (std::size_t alignment : {1u, 2u, 8u, 16u, 64u, 256u, 4096u}) {
    auto block = allocator.allocate({32, alignment, 1, 2}); CHECK(block);
    CHECK(reinterpret_cast<std::uintptr_t>(block.value().data()) % alignment == 0);
    std::memset(block.value().data(), 0xab, block.value().size());
    CHECK(allocator.snapshot().owners[1].live == 32);
    CHECK(allocator.snapshot().categories[2].live == 32);
    OwnedBlock moved = std::move(block).value(); CHECK(!block.value()); CHECK(moved.size() == 32);
    auto other = TrackedAllocator::create({2048}); CHECK(other);
    error_is(other.value().release(std::move(moved)), ErrorCode::InvalidInput);
    CHECK(moved); CHECK(allocator.release(std::move(moved))); CHECK(!moved);
    CHECK(allocator.snapshot().total.live == 0);
  }
  {
    auto exact = allocator.allocate({2048}); CHECK(exact);
    const auto full = allocator.snapshot();
    error_is(allocator.allocate({1}), ErrorCode::Exhausted);
    CHECK(allocator.snapshot().total.requested == full.total.requested);
    CHECK(allocator.snapshot().total.peak == 2048);
    CHECK(allocator.snapshot().total.reserved == 2048);
  }
  CHECK(allocator.snapshot().total.live == 0);

  // The allocation retains its originating state after the facade is destroyed.
  OwnedBlock survivor;
  {
    auto temporary = TrackedAllocator::create({64}); CHECK(temporary);
    auto block = temporary.value().allocate({64}); CHECK(block);
    survivor = std::move(block).value();
  }
  std::memset(survivor.data(), 0x12, survivor.size()); survivor.reset();

  auto arena_result = ScopedArena::create(allocator, 1024); CHECK(arena_result);
  auto arena = std::move(arena_result).value();
  error_is(arena.allocate(0), ErrorCode::InvalidInput);
  error_is(arena.allocate(1, 3), ErrorCode::InvalidInput);
  auto lease_result = arena.allocate(32, 64); CHECK(lease_result);
  auto lease = std::move(lease_result).value();
  CHECK(reinterpret_cast<std::uintptr_t>(lease.bytes().data()) % 64 == 0);
  auto token = lease.token();
  auto second = token.acquire(); CHECK(second);
  error_is(arena.reset(), ErrorCode::Busy);
  lease.release(); error_is(arena.reset(), ErrorCode::Busy);
  std::thread consumer([owned = std::move(second).value()]() mutable {
    std::memset(owned.bytes().data(), 0xaa, owned.bytes().size()); owned.release();
  }); consumer.join();
  CHECK(arena.reset()); CHECK(arena.used() == 0);
  error_is(token.acquire(), ErrorCode::StaleHandle);
  auto exact_arena = arena.allocate(1024, 1); CHECK(exact_arena);
  error_is(arena.allocate(1), ErrorCode::Exhausted);
  CHECK(arena.used() == 1024);
  exact_arena.value().release(); CHECK(arena.reset());
  error_is(arena.allocate(std::numeric_limits<std::size_t>::max()), ErrorCode::Exhausted);
  CHECK(arena.used() == 0);
  std::thread wrong_owner([&] {
    error_is(arena.allocate(1), ErrorCode::WrongThread);
    error_is(arena.reset(), ErrorCode::WrongThread);
  }); wrong_owner.join();
  ArenaLease retained;
  ArenaToken expired;
  {
    auto short_arena = ScopedArena::create(allocator, 64); CHECK(short_arena);
    auto bytes = short_arena.value().allocate(32); CHECK(bytes);
    retained = std::move(bytes).value(); expired = retained.token();
  }
  retained.bytes()[0] = std::byte{42}; CHECK(retained.bytes()[0] == std::byte{42});
  retained.release(); error_is(expired.acquire(), ErrorCode::StaleHandle);

  // Concurrent admission at a fixed payload budget; every release is accounted.
  auto concurrent_result = TrackedAllocator::create({256}); CHECK(concurrent_result);
  auto concurrent = std::move(concurrent_result).value();
  std::atomic<bool> go{false};
  std::atomic<std::size_t> successes{}, exhaustions{};
  std::vector<std::thread> workers;
  for (std::size_t i = 0; i < 8; ++i) workers.emplace_back([&, i] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    for (int j = 0; j < 2000; ++j) {
      auto allocation = concurrent.allocate({64, 64, i, 0});
      if (allocation) {
        ++successes; std::memset(allocation.value().data(), 1, 64);
        CHECK(concurrent.snapshot().total.live <= 256);
        std::this_thread::yield();
      } else { error_is(allocation, ErrorCode::Exhausted); ++exhaustions; }
    }
  });
  go.store(true, std::memory_order_release);
  for (auto& thread : workers) thread.join();
  CHECK(successes > 0); CHECK(successes + exhaustions == 16000);
  const auto final = concurrent.snapshot();
  CHECK(final.total.live == 0); CHECK(final.total.reserved == 0);
  CHECK(final.total.peak <= 256); CHECK(final.total.requested == successes * 64);
  for (const auto& owner : final.owners) CHECK(owner.live == 0);
}
