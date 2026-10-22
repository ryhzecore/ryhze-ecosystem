#include "test.hpp"
#include "../apps/mock_io.hpp"
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>

using namespace race;
using namespace std::chrono_literals;
namespace {
// The real platform worker is held before executing the mock request. This
// makes pre-completion cancellation and pending ownership checks deterministic.
class GatedThreads final : public platform::Threads {
  struct Gate { std::mutex mutex; std::condition_variable changed; bool open = false; };
  platform::Threads& native_;
  std::shared_ptr<Gate> gate_ = std::make_shared<Gate>();
public:
  explicit GatedThreads(platform::Threads& native) : native_(native) {}
  Result<std::unique_ptr<platform::Thread>> start(std::function<void()> function) override {
    return native_.start([gate = gate_, function = std::move(function)] {
      {
        std::unique_lock lock(gate->mutex);
        gate->changed.wait(lock, [&] { return gate->open; });
      }
      function();
    });
  }
  void open() {
    { std::lock_guard lock(gate_->mutex); gate_->open = true; }
    gate_->changed.notify_all();
  }
};
class FailingThreads final : public platform::Threads {
  Result<std::unique_ptr<platform::Thread>> start(std::function<void()>) override {
    return fail(ErrorCode::PlatformFailure, "injected thread start failure");
  }
};
}
void mock_io_tests() {
  auto created = platform::make_services(); CHECK(created);
  auto services = std::move(created).value();
  auto allocator = TrackedAllocator::create({512}); CHECK(allocator);
  auto memory = std::move(allocator).value();
  CHECK_ERROR(fixtures::MockIo::begin(*services.threads, {}), ErrorCode::InvalidInput);
  {
    FailingThreads failed_threads;
    auto block = memory.allocate({32}); CHECK(block);
    auto operation = fixtures::MockIo::begin(failed_threads, std::move(block).value());
    CHECK(!operation); CHECK(operation.error().code == ErrorCode::PlatformFailure);
    CHECK(memory.snapshot().total.live == 0);
  }
  {
    GatedThreads gated(*services.threads);
    auto block = memory.allocate({128}); CHECK(block);
    std::memset(block.value().data(), 0x11, block.value().size());
    auto operation = fixtures::MockIo::begin(gated, std::move(block).value()); CHECK(operation);
    CHECK(!block.value()); CHECK(memory.snapshot().total.live == 128);
    CHECK(!operation.value()->take().value());
    CHECK_ERROR(operation.value()->wait(0ms), ErrorCode::Timeout);
    CHECK_ERROR(operation.value()->wait(-1ms), ErrorCode::InvalidInput);
    std::thread wrong_dispatch([&] {
      CHECK_ERROR(operation.value()->take(), ErrorCode::WrongThread);
    }); wrong_dispatch.join();
    operation.value()->cancel(); operation.value()->cancel();
    CHECK(memory.snapshot().total.live == 128); // cancellation does not free I/O data
    CHECK_ERROR(operation.value()->wait(0ms), ErrorCode::Timeout);
    gated.open(); CHECK(operation.value()->wait(1s));
    auto completion = operation.value()->take(); CHECK(completion); CHECK(completion.value());
    CHECK(completion.value()->cancelled);
    CHECK(completion.value()->buffer.size() == 128);
    CHECK(completion.value()->buffer.data()[0] == std::byte{0x11});
    CHECK(!operation.value()->take().value());
    operation.value()->cancel(); CHECK(operation.value()->wait(0ms));
    CHECK(!operation.value()->take().value());
    operation.value().reset(); // delivered buffer outlives request and worker
    CHECK(memory.snapshot().total.live == 128);
    CHECK(completion.value()->buffer.data()[127] == std::byte{0x11});
    completion.value().reset(); CHECK(memory.snapshot().total.live == 0);
  }
  {
    auto block = memory.allocate({64}); CHECK(block);
    auto operation = fixtures::MockIo::begin(*services.threads, std::move(block).value()); CHECK(operation);
    CHECK(operation.value()->wait(1s));
    operation.value()->cancel(); // terminal success cannot become cancellation
    operation.value()->cancel();
    auto completion = operation.value()->take(); CHECK(completion); CHECK(completion.value());
    CHECK(!completion.value()->cancelled);
    CHECK(completion.value()->buffer.data()[0] == std::byte{42});
    CHECK(!operation.value()->take().value());
    CHECK(operation.value()->wait(0ms)); CHECK(!operation.value()->take().value());
    completion.value().reset(); CHECK(memory.snapshot().total.live == 0);
  }
  {
    // Destruction joins an undispatched request before releasing its input.
    auto block = memory.allocate({64}); CHECK(block);
    auto operation = fixtures::MockIo::begin(*services.threads, std::move(block).value()); CHECK(operation);
    operation.value().reset(); CHECK(memory.snapshot().total.live == 0);
  }
  CHECK(memory.snapshot().total.live == 0);
}
