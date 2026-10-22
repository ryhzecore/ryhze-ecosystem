#pragma once
#include "race/assets/assets.hpp"
#include "test.hpp"
#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
using namespace race;
using namespace race::assets;
using namespace std::chrono_literals;
inline AssetId aid(std::uint64_t n) { return {77, n}; }
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool entered{}, open{};
  void arrive() {
    std::unique_lock lock(mutex);
    entered = true;
    changed.notify_all();
    changed.wait(lock, [&] { return open; });
  }
  void wait() {
    std::unique_lock lock(mutex);
    CHECK(changed.wait_for(lock, 10s, [&] { return entered; }));
  }
  void release() {
    std::lock_guard lock(mutex);
    open = true;
    changed.notify_all();
  }
};
struct FakeFiles : platform::RootedFiles {
  mutable std::mutex mutex;
  std::map<std::string, std::vector<std::byte>> data;
  std::shared_ptr<Gate> gate;
  std::string gated;
  std::size_t reads{}, writes{}, fail_write = SIZE_MAX;
  bool throw_read{};
  Result<std::vector<std::byte>> read(std::string_view n, std::size_t limit) override {
    std::shared_ptr<Gate> wait;
    {
      std::lock_guard lock(mutex);
      ++reads;
      if (n == gated)
        wait = gate;
    }
    if (wait)
      wait->arrive();
    std::lock_guard lock(mutex);
    if (throw_read)
      throw std::bad_alloc();
    auto it = data.find(std::string(n));
    if (it == data.end())
      return fail(ErrorCode::NotFound, "fixture missing");
    if (it->second.size() > limit)
      return fail(ErrorCode::Exhausted, "fixture read limit");
    return it->second;
  }
  Result<void> publish_immutable(std::string_view n, std::span<const std::byte> b) override {
    std::lock_guard lock(mutex);
    if (writes++ == fail_write)
      return fail(ErrorCode::IoFailure, "fixture interrupted product");
    auto it = data.find(std::string(n));
    if (it != data.end()) {
      if (std::equal(b.begin(), b.end(), it->second.begin(), it->second.end()))
        return {};
      return fail(ErrorCode::Duplicate, "fixture immutable collision");
    }
    data[std::string(n)] = {b.begin(), b.end()};
    return {};
  }
  Result<void> replace(std::string_view n, std::span<const std::byte> b,
                       platform::Durability) override {
    std::lock_guard lock(mutex);
    if (writes++ == fail_write)
      return fail(ErrorCode::IoFailure, "fixture interrupted catalog");
    data[std::string(n)] = {b.begin(), b.end()};
    return {};
  }
  void reset_reads() {
    std::lock_guard lock(mutex);
    reads = 0;
  }
  std::size_t read_count() {
    std::lock_guard lock(mutex);
    return reads;
  }
};
inline std::vector<Input> diamond() {
  return {{aid(1), 1, 32, 0, 1, {aid(2), aid(3)}, {aid(99), {}}},
          {aid(2), 2, 32, 0, 1, {aid(4)}, {}},
          {aid(3), 3, 32, 0, 1, {aid(4)}, {}},
          {aid(4), 4, 32, 0, 1, {}, {}}};
}
struct Fixture {
  platform::Services platform = std::move(platform::make_services().value());
  TrackedAllocator memory = TrackedAllocator::create({64 * 1024 * 1024}).value();
  std::unique_ptr<core::Scheduler> jobs =
      std::move(core::Scheduler::create(*platform.threads, {32, 2, 16}).value());
  std::shared_ptr<FakeFiles> files = std::make_shared<FakeFiles>();
  Catalog catalog;
  std::unique_ptr<Service> service;
  explicit Fixture(std::vector<Input> inputs = diamond(), Limits limits = {}) {
    auto c = cook(inputs, *files, limits);
    CHECK(c);
    catalog = c.value();
    files->reset_reads();
    auto s = Service::create(catalog, limits, memory, files, *platform.threads, *jobs);
    CHECK(s);
    service = std::move(s.value());
  }
  Receipt wait(RequestHandle h) {
    auto start = std::chrono::steady_clock::now();
    for (;;) {
      CHECK(service->drain());
      auto r = service->poll(h);
      CHECK(r);
      if (r.value().state != RequestState::Pending)
        return r.value();
      CHECK(std::chrono::steady_clock::now() - start < 10s);
      std::this_thread::yield();
    }
  }
  void empty() {
    CHECK(service->stop(10s));
    CHECK(jobs->stats().occupied == 0);
    CHECK(service->stats().temporary_bytes == 0);
  }
};
void asset_wire_tests();
void asset_runtime_tests();
void asset_fault_tests();
void asset_fail_after(std::size_t);
void asset_fail_disable();
void asset_decode_gate_arm(std::size_t);
bool asset_decode_gate_entered();
void asset_decode_gate_open();

void asset_fail_payload_once(std::size_t);
