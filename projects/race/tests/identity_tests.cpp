#include "test.hpp"
#include "race/foundation/identity.hpp"
#include "race/foundation/channel.hpp"
#include <array>
#include <atomic>
#include <thread>

using namespace race;
namespace {
template<class T> void error_is(const Result<T>& result, ErrorCode code) {
  CHECK(!result); CHECK(result.error().code == code);
}
struct Object {
  std::atomic<int>* destructions;
  int value{42};
  explicit Object(std::atomic<int>& count) : destructions(&count) {}
  ~Object() { ++*destructions; }
};
struct Message { std::size_t producer{}, sequence{}; };
}
void identity_tests() {
  PersistentRegistry registry(2); const PersistentId id{123, 456};
  CHECK(registry.add(id, "before")); CHECK(registry.rename(id, "after"));
  CHECK(registry.name(id).value() == "after");
  error_is(registry.add(id, "duplicate"), ErrorCode::Duplicate);
  error_is(registry.add({}, "invalid"), ErrorCode::InvalidInput);
  error_is(registry.rename(id, ""), ErrorCode::InvalidInput);
  error_is(registry.rename({9, 9}, "absent"), ErrorCode::NotFound);
  CHECK(registry.add({1, 2}, "second"));
  error_is(registry.add({1, 3}, "third"), ErrorCode::Exhausted);
  CHECK(registry.name(id).value() == "after");

  std::atomic<int> destructions{};
  HandleTable<Object> table(1), other(1);
  auto handle = table.insert(std::make_shared<Object>(destructions)); CHECK(handle);
  error_is(other.resolve(handle.value()), ErrorCode::StaleHandle);
  {
    auto scope = table.resolve(handle.value()); CHECK(scope); CHECK(scope.value()->value == 42);
    CHECK(table.erase(handle.value())); CHECK(destructions == 0);
    error_is(table.resolve(handle.value()), ErrorCode::StaleHandle);
    auto replacement = table.insert(std::make_shared<Object>(destructions)); CHECK(replacement);
    CHECK(replacement.value().slot == handle.value().slot);
    CHECK(replacement.value().generation != handle.value().generation);
    CHECK(scope.value()->value == 42); CHECK(table.erase(replacement.value()));
    CHECK(destructions == 1);
  }
  CHECK(destructions == 2);
  HandleTable<int, std::uint8_t> wrap_table(1);
  auto shared = std::make_shared<int>(42);
  Handle<int> first{};
  for (int i = 1; i <= 255; ++i) {
    auto current = wrap_table.insert(shared); CHECK(current);
    if (i == 1) first = current.value();
    CHECK(current.value().generation == static_cast<unsigned>(i));
    CHECK(wrap_table.erase(current.value()));
  }
  error_is(wrap_table.insert(shared), ErrorCode::Exhausted);
  error_is(wrap_table.resolve(first), ErrorCode::StaleHandle);
  error_is(wrap_table.erase(first), ErrorCode::StaleHandle);
  error_is(wrap_table.insert({}), ErrorCode::InvalidInput);

  BoundedChannel<std::unique_ptr<int>> bounded(1);
  auto first_message = std::make_unique<int>(10), second_message = std::make_unique<int>(20);
  CHECK(bounded.send(std::move(first_message))); CHECK(!first_message);
  error_is(bounded.send(std::move(second_message)), ErrorCode::QueueFull);
  CHECK(second_message && *second_message == 20);
  CHECK(bounded.snapshot().full_count == 1);
  auto received = bounded.receive(); CHECK(received); CHECK(received.value());
  CHECK(**received.value() == 10);
  CHECK(bounded.send(std::move(second_message))); bounded.close();
  auto after_close = bounded.receive(); CHECK(after_close.value()); CHECK(**after_close.value() == 20);
  CHECK(!bounded.receive().value());
  auto rejected = std::make_unique<int>(30);
  error_is(bounded.send(std::move(rejected)), ErrorCode::Stopping); CHECK(rejected);
  BoundedChannel<int> zero(0); int value = 1;
  error_is(zero.send(std::move(value)), ErrorCode::QueueFull);
  CHECK(!zero.receive().value());

  // MPMC implementation, two producers and one consumer exercise transfer and
  // per-producer order. Backpressure retries retain each original owner.
  BoundedChannel<std::unique_ptr<Message>> channel(7);
  constexpr std::size_t producers = 4, per_producer = 3000;
  std::vector<std::thread> senders;
  for (std::size_t producer = 0; producer < producers; ++producer)
    senders.emplace_back([&, producer] {
      for (std::size_t sequence = 0; sequence < per_producer; ++sequence) {
        auto message = std::make_unique<Message>(Message{producer, sequence});
        while (true) {
          auto result = channel.send(std::move(message));
          if (result) break;
          error_is(result, ErrorCode::QueueFull); CHECK(message);
          std::this_thread::yield();
        }
        CHECK(!message);
      }
    });
  std::array<std::size_t, producers> next{};
  for (std::size_t count = 0; count < producers * per_producer;) {
    auto message = channel.receive(); CHECK(message);
    if (!message.value()) { std::this_thread::yield(); continue; }
    auto& item = **message.value(); CHECK(item.producer < producers);
    CHECK(item.sequence == next[item.producer]++); ++count;
  }
  for (auto& sender : senders) sender.join();
  for (auto count : next) CHECK(count == per_producer);
  CHECK(channel.snapshot().queued == 0); CHECK(channel.snapshot().high_water <= 7);

  // Exercise the complete multi-producer/multi-consumer topology independently
  // of the single-consumer ordering check. Each accepted owner arrives once.
  BoundedChannel<std::unique_ptr<std::size_t>> mpmc(11);
  constexpr std::size_t each = 1000, threads = 4;
  std::vector<std::atomic<unsigned>> visits(each * threads);
  std::vector<std::thread> workers;
  for (std::size_t producer = 0; producer < threads; ++producer) workers.emplace_back([&, producer] {
    for (std::size_t sequence = 0; sequence < each; ++sequence) {
      auto message = std::make_unique<std::size_t>(producer * each + sequence);
      while (true) {
        auto result = mpmc.send(std::move(message));
        if (result) break;
        error_is(result, ErrorCode::QueueFull); CHECK(message); std::this_thread::yield();
      }
    }
  });
  for (std::size_t consumer = 0; consumer < threads; ++consumer) workers.emplace_back([&] {
    for (std::size_t received_count = 0; received_count < each;) {
      auto message = mpmc.receive(); CHECK(message);
      if (!message.value()) { std::this_thread::yield(); continue; }
      const auto index = **message.value(); CHECK(index < visits.size());
      CHECK(visits[index].fetch_add(1) == 0); ++received_count;
    }
  });
  for (auto& worker : workers) worker.join();
  for (const auto& count : visits) CHECK(count == 1);
  CHECK(mpmc.snapshot().queued == 0);

  // A consumer races erasure with resolution; retained scopes remain valid.
  HandleTable<int> concurrent(1); auto target = concurrent.insert(shared); CHECK(target);
  std::atomic<bool> finish{false}, reading{false};
  std::thread reader([&] {
    auto retained_scope = concurrent.resolve(target.value()); CHECK(retained_scope);
    reading.store(true);
    while (!finish.load()) {
      auto scope = concurrent.resolve(target.value());
      if (scope) CHECK(scope.value().get() == 42);
      else error_is(scope, ErrorCode::StaleHandle);
    }
    CHECK(retained_scope.value().get() == 42);
  });
  while (!reading.load()) std::this_thread::yield();
  CHECK(concurrent.erase(target.value())); finish.store(true); reader.join();
}
