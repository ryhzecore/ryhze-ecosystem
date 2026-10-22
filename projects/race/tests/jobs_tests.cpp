#include "race/core/jobs.hpp"
#include "test.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
using namespace race;
using namespace race::core;
using namespace std::chrono_literals;
namespace {
class TestThread final : public platform::Thread {
  std::thread thread_;
public:
  explicit TestThread(std::function<void()> f) : thread_(std::move(f)) {}
  ~TestThread() override { if (thread_.joinable()) thread_.join(); }
  Result<void> join() noexcept override { if (thread_.joinable()) thread_.join(); return {}; }
};
class TestThreads final : public platform::Threads {
public:
  int remaining = -1;
  Result<std::unique_ptr<platform::Thread>> start(std::function<void()> body) override {
    if (remaining == 0) return fail(ErrorCode::PlatformFailure, "injected thread failure");
    if (remaining > 0) --remaining;
    return std::unique_ptr<platform::Thread>(new TestThread(std::move(body)));
  }
};
template<class T> void error_is(const Result<T>& result, ErrorCode code) { CHECK(!result); CHECK(result.error().code == code); }
Result<void> noop(const JobContext&) { return {}; }
Result<void> count(const JobContext& context) { ++*static_cast<std::atomic<int>*>(context.context()); return {}; }
struct Gate {
  std::mutex mutex;
  std::condition_variable changed;
  bool started = false;
  bool proceed = false;
  void await_started() { std::unique_lock lock(mutex); CHECK(changed.wait_for(lock, 5s, [&] { return started; })); }
  void open() { std::lock_guard lock(mutex); proceed = true; changed.notify_all(); }
};
// A test platform join may remain pending after scheduler work is quiescent.
// The gate controls ordering; no elapsed-time assertion is involved.
class DelayedJoinThread final : public platform::Thread {
  std::thread thread_;
  std::shared_ptr<Gate> join_gate_;
public:
  DelayedJoinThread(std::function<void()> body, std::shared_ptr<Gate> gate)
      : thread_(std::move(body)), join_gate_(std::move(gate)) {}
  ~DelayedJoinThread() override { if (thread_.joinable()) thread_.join(); }
  Result<void> join() noexcept override {
    if (!thread_.joinable()) return {};
    std::unique_lock lock(join_gate_->mutex);
    join_gate_->started = true;
    join_gate_->changed.notify_all();
    join_gate_->changed.wait(lock, [&] { return join_gate_->proceed; });
    lock.unlock();
    thread_.join();
    return {};
  }
};
class DelayedJoinThreads final : public platform::Threads {
public:
  std::shared_ptr<Gate> join_gate = std::make_shared<Gate>();
  Result<std::unique_ptr<platform::Thread>> start(std::function<void()> body) override {
    return std::unique_ptr<platform::Thread>(new DelayedJoinThread(std::move(body), join_gate));
  }
};
// Deliberately blocked fault-injection callback, not an example of permitted production CPU work.
Result<void> gate_job(const JobContext& context) {
  auto& gate = *static_cast<Gate*>(context.context());
  std::unique_lock lock(gate.mutex);
  gate.started = true;
  gate.changed.notify_all();
  gate.changed.wait(lock, [&] { return gate.proceed; });
  return {};
}
Result<void> gated_cancellation_job(const JobContext& context) {
  auto result = gate_job(context);
  if (!result) return result;
  if (context.cancelled()) return fail(ErrorCode::Cancelled, "gated cooperative cancellation");
  return {};
}
Result<void> cooperative(const JobContext& context) {
  auto& gate = *static_cast<Gate*>(context.context());
  { std::lock_guard lock(gate.mutex); gate.started = true; gate.changed.notify_all(); }
  while (!context.cancelled()) std::this_thread::yield();
  return fail(ErrorCode::Cancelled, "cooperative cancellation");
}
Result<void> fail_job(const JobContext&) { return fail(ErrorCode::InvalidInput, "injected callback failure"); }
Result<void> throw_job(const JobContext&) { throw std::runtime_error("injected callback exception"); }
std::unique_ptr<Scheduler> make(TestThreads& threads, SchedulerConfig config = {64, 4, 16}) {
  auto result = Scheduler::create(threads, config);
  CHECK(result);
  return std::move(result).value();
}
GroupHandle group(Scheduler& s) { auto result = s.create_group(); CHECK(result); return result.value(); }
void finish(Scheduler& s, GroupHandle g, std::uint64_t expected) {
  CHECK(s.close(g));
  auto result = s.wait(g, 10s);
  CHECK(result);
  CHECK(result.value().succeeded == expected);
  CHECK(result.value().failed == 0);
  CHECK(result.value().cancelled == 0);
}
void admission_tests(TestThreads& threads) {
  auto s = make(threads, {3, 1, 2});
  auto g = group(*s);
  auto gate = std::make_shared<Gate>();
  std::weak_ptr<Gate> weak = gate;
  JobDesc blocking{gate_job, gate};
  auto first = s->submit(g, blocking); CHECK(first);
  gate->await_started();
  std::array<JobHandle, 1> prerequisite{first.value()};
  JobDesc second{noop}; second.prerequisites = prerequisite;
  auto two = s->submit(g, second); CHECK(two);
  auto three = s->submit(g, second); CHECK(three);
  error_is(s->submit(g, second), ErrorCode::QueueFull);
  error_is(s->wait(g, 0ms), ErrorCode::Busy);
  CHECK(s->close(g));
  error_is(s->wait(g, 0ms), ErrorCode::Timeout);
  error_is(s->release(g), ErrorCode::Busy);
  gate->open();
  gate.reset(); blocking.owned_context.reset();
  finish(*s, g, 3);
  CHECK(!weak.expired()); // Terminal results retain context owners until release.
  CHECK(s->stats().completed == 3);
  CHECK(s->release(g));
  CHECK(weak.expired());
  error_is(s->completion(first.value()), ErrorCode::StaleHandle);
  auto next = group(*s); CHECK(next != g);
  error_is(s->submit(next, second), ErrorCode::InvalidDependency);
  CHECK(s->close(next)); CHECK(s->release(next));
  CHECK(s->shutdown(ShutdownMode::Drain, 5s));
  CHECK(s->shutdown(ShutdownMode::Cancel, 5s));
  error_is(s->create_group(), ErrorCode::Stopping);
}
void graph_tests(TestThreads& threads) {
  auto s = make(threads);
  auto g = group(*s);
  auto counts = std::make_shared<std::atomic<int>>(0);
  std::array<std::size_t, 1> left{1}, right{0};
  std::array<JobDesc, 2> cyclic{JobDesc{count, counts}, JobDesc{count, counts}};
  cyclic[0].batch_prerequisites = left; cyclic[1].batch_prerequisites = right;
  error_is(s->submit_batch(g, cyclic), ErrorCode::Cycle);
  CHECK(counts->load() == 0 && s->stats().admitted == 0);
  left[0] = 99;
  error_is(s->submit_batch(g, cyclic), ErrorCode::InvalidDependency);
  CHECK(s->stats().occupied == 0);
  std::array<std::size_t, 1> root{0};
  std::array<std::size_t, 2> leaves{1, 2};
  std::array<JobDesc, 4> dag{JobDesc{count, counts}, JobDesc{count, counts}, JobDesc{count, counts}, JobDesc{count, counts}};
  dag[1].batch_prerequisites = root; dag[2].batch_prerequisites = root; dag[3].batch_prerequisites = leaves;
  for (auto& job : dag) job.unknown_access = false;
  auto result = s->submit_batch(g, dag); CHECK(result);
  finish(*s, g, 4); CHECK(counts->load() == 4);
  for (auto h : result.value()) CHECK(s->completion(h).value().state == JobState::Succeeded);
  CHECK(s->release(g));
  // Handle identity is not transferable between scheduler instances.
  auto other = make(threads);
  auto other_group = group(*other);
  std::array<JobHandle, 1> stale{result.value()[0]};
  JobDesc invalid{noop}; invalid.prerequisites = stale;
  error_is(other->submit(other_group, invalid), ErrorCode::InvalidDependency);
  CHECK(other->close(other_group)); CHECK(other->release(other_group));
}
void failures_tests(TestThreads& threads) {
  auto s = make(threads);
  auto g = group(*s);
  std::array<std::size_t, 1> first{0}, second{1};
  std::array<JobDesc, 4> jobs{JobDesc{fail_job}, JobDesc{noop}, JobDesc{noop}, JobDesc{throw_job}};
  jobs[1].batch_prerequisites = first; jobs[2].batch_prerequisites = second;
  auto result = s->submit_batch(g, jobs); CHECK(result); CHECK(s->close(g));
  auto done = s->wait(g, 5s); CHECK(done); CHECK(done.value().failed == 4);
  CHECK(s->completion(result.value()[0]).value().error.code == ErrorCode::InvalidInput);
  CHECK(s->completion(result.value()[1]).value().state == JobState::FailedPrerequisite);
  CHECK(s->completion(result.value()[2]).value().state == JobState::FailedPrerequisite);
  CHECK(s->completion(result.value()[3]).value().error.code == ErrorCode::PlatformFailure);
  CHECK(s->release(g));
  auto cancel_group = group(*s);
  auto gate = std::make_shared<Gate>();
  auto running = s->submit(cancel_group, JobDesc{cooperative, gate}); CHECK(running); gate->await_started();
  std::array<JobHandle, 1> dependency{running.value()};
  auto dependent_group = group(*s);
  JobDesc dependent{noop}; dependent.prerequisites = dependency;
  auto after = s->submit(dependent_group, dependent); CHECK(after);
  CHECK(s->close(dependent_group)); CHECK(s->close(cancel_group));
  CHECK(s->cancel(cancel_group)); CHECK(s->cancel(cancel_group));
  auto cancel_done = s->wait(cancel_group, 5s); CHECK(cancel_done); CHECK(cancel_done.value().cancelled == 1);
  CHECK(s->wait(dependent_group, 5s));
  CHECK(s->completion(after.value()).value().state == JobState::FailedPrerequisite);
  CHECK(s->release(cancel_group)); CHECK(s->release(dependent_group));
}
struct WaitContext { Scheduler* scheduler; GroupHandle group; std::atomic<bool> rejected{false}; };
Result<void> worker_wait(const JobContext& context) {
  auto& data = *static_cast<WaitContext*>(context.context());
  auto wait = data.scheduler->wait(data.group, 0ms);
  auto shutdown = data.scheduler->shutdown(ShutdownMode::Cancel, 0ms);
  data.rejected = !wait && wait.error().code == ErrorCode::WrongThread && !shutdown && shutdown.error().code == ErrorCode::WrongThread;
  return {};
}
void shutdown_tests(TestThreads& threads) {
  auto s = make(threads, {8, 2, 4});
  auto g = group(*s);
  auto waiter = std::make_shared<WaitContext>(); waiter->scheduler = s.get(); waiter->group = g;
  CHECK(s->submit(g, JobDesc{worker_wait, waiter})); finish(*s, g, 1); CHECK(waiter->rejected); CHECK(s->release(g));
  auto active = group(*s);
  auto gate = std::make_shared<Gate>(); std::weak_ptr<Gate> weak = gate;
  JobDesc blocking{gate_job, gate}; CHECK(s->submit(active, blocking)); gate->await_started();
  error_is(s->shutdown(ShutdownMode::Cancel, 0ms), ErrorCode::Timeout);
  CHECK(!weak.expired()); CHECK(s->stats().running == 1);
  gate->open(); gate.reset(); blocking.owned_context.reset();
  CHECK(s->shutdown(ShutdownMode::Cancel, 5s)); CHECK(!weak.expired());
  CHECK(s->release(active)); CHECK(weak.expired());
  CHECK(s->shutdown(ShutdownMode::Drain, 0ms));
  TestThreads failing; failing.remaining = 1;
  error_is(Scheduler::create(failing, {8, 3, 4}), ErrorCode::PlatformFailure);
}
void timeout_contract_tests(TestThreads& threads) {
  auto s = make(threads, {8, 1, 3});
  auto g = group(*s);
  auto gate = std::make_shared<Gate>();
  auto active = s->submit(g, JobDesc{gated_cancellation_job, gate}); CHECK(active);
  gate->await_started();
  error_is(s->wait(g, -1ms), ErrorCode::InvalidInput);
  error_is(s->shutdown(ShutdownMode::Drain, -1ms), ErrorCode::InvalidInput);
  error_is(s->shutdown(ShutdownMode::Cancel, -1ms), ErrorCode::InvalidInput);
  // Negative calls neither stop admission, close groups nor request cancellation.
  auto calls = std::make_shared<std::atomic<int>>(0);
  CHECK(s->submit(g, JobDesc{count, calls}));
  auto extra = group(*s); CHECK(s->close(extra)); CHECK(s->release(extra));
  error_is(s->wait(g, 0ms), ErrorCode::Busy);
  CHECK(s->close(g));
  error_is(s->wait(g, -1ms), ErrorCode::InvalidInput);
  error_is(s->wait(g, 0ms), ErrorCode::Timeout);
  CHECK(s->completion(active.value()).value().state == JobState::Running);
  CHECK(s->stats().pending == 2 && s->stats().running == 1);

  Gate caller_entered;
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    {
      std::lock_guard lock(caller_entered.mutex);
      caller_entered.started = true;
      caller_entered.changed.notify_all();
    }
    auto done = s->wait(g, std::chrono::milliseconds::max());
    CHECK(done && done.value().succeeded == 2 && done.value().failed == 0 && done.value().cancelled == 0);
    returned = true;
  });
  // This signal identifies caller entry, not the private condition-variable stage.
  caller_entered.await_started(); CHECK(!returned);
  gate->open(); waiter.join(); CHECK(returned);
  CHECK(calls->load() == 1);
  CHECK(s->completion(active.value()).value().state == JobState::Succeeded);
  auto completed = s->wait(g, 0ms);
  CHECK(completed && completed.value().succeeded == 2 && completed.value().failed == 0 && completed.value().cancelled == 0);
  CHECK(s->release(g));
  error_is(s->wait(g, 0ms), ErrorCode::StaleHandle);
  error_is(s->wait(g, std::chrono::milliseconds::max()), ErrorCode::StaleHandle);
  CHECK(s->shutdown(ShutdownMode::Drain, std::chrono::milliseconds::max()));
}
void drain_cancel_timeout_tests(TestThreads& threads) {
  auto s = make(threads, {4, 1, 1});
  auto g = group(*s);
  auto gate = std::make_shared<Gate>(); std::weak_ptr<Gate> weak = gate;
  auto active = s->submit(g, JobDesc{gated_cancellation_job, gate}); CHECK(active);
  gate->await_started();
  auto calls = std::make_shared<std::atomic<int>>(0);
  auto queued = s->submit(g, JobDesc{count, calls}); CHECK(queued);
  error_is(s->shutdown(ShutdownMode::Drain, 0ms), ErrorCode::Timeout);
  CHECK(s->completion(active.value()).value().state == JobState::Running);
  CHECK(s->completion(queued.value()).value().state == JobState::Waiting);
  error_is(s->wait(g, 0ms), ErrorCode::Timeout); // Drain closed the group.
  error_is(s->create_group(), ErrorCode::Stopping);
  error_is(s->submit(g, JobDesc{noop}), ErrorCode::Stopping);
  error_is(s->release(g), ErrorCode::Busy);
  CHECK(!weak.expired() && s->stats().occupied == 2 && s->stats().pending == 2);

  error_is(s->shutdown(ShutdownMode::Cancel, 0ms), ErrorCode::Timeout);
  CHECK(s->completion(active.value()).value().state == JobState::Running);
  CHECK(s->completion(queued.value()).value().state == JobState::Cancelled);
  CHECK(calls->load() == 0 && s->stats().pending == 1);
  CHECK(!weak.expired());
  gate->open(); gate.reset();
  CHECK(s->shutdown(ShutdownMode::Cancel, std::chrono::milliseconds::max()));
  CHECK(!weak.expired()); // Quiescence and joining retain results/context owners.
  auto done = s->wait(g, 0ms);
  CHECK(done && done.value().succeeded == 0 && done.value().failed == 0 && done.value().cancelled == 2);
  CHECK(s->completion(active.value()).value().state == JobState::Cancelled);
  CHECK(s->completion(queued.value()).value().state == JobState::Cancelled);
  CHECK(s->stats().occupied == 2 && s->stats().pending == 0 && s->stats().running == 0);
  CHECK(s->release(g)); CHECK(weak.expired());
  error_is(s->completion(active.value()), ErrorCode::StaleHandle);
  error_is(s->completion(queued.value()), ErrorCode::StaleHandle);
  CHECK(s->shutdown(ShutdownMode::Drain, 0ms));
}
void untimed_join_test() {
  DelayedJoinThreads threads;
  auto result = Scheduler::create(threads, {2, 1, 1}); CHECK(result);
  auto s = std::move(result).value();
  auto g = group(*s);
  CHECK(s->submit(g, JobDesc{noop})); finish(*s, g, 1);
  CHECK(s->stats().pending == 0 && s->stats().running == 0);
  std::atomic<bool> returned{false};
  std::thread stopper([&] {
    CHECK(s->shutdown(ShutdownMode::Drain, 0ms));
    returned = true;
  });
  threads.join_gate->await_started();
  CHECK(!returned); // Zero completion-wait timeout did not skip/interrupt join.
  CHECK(s->stats().pending == 0 && s->stats().running == 0);
  auto done = s->wait(g, 0ms); CHECK(done && done.value().succeeded == 1);
  threads.join_gate->open(); stopper.join(); CHECK(returned);
  CHECK(s->release(g));
  CHECK(s->shutdown(ShutdownMode::Drain, 0ms));
}
struct AccessContext {
  std::atomic<int> writers{0}, readers{0}, calls{0};
  std::atomic<bool> violation{false};
};
Result<void> write_resource(const JobContext& context) {
  auto& data = *static_cast<AccessContext*>(context.context());
  if (data.writers.fetch_add(1) != 0 || data.readers.load() != 0) data.violation = true;
  for (int i = 0; i < 30; ++i) std::this_thread::yield();
  --data.writers; ++data.calls;
  return {};
}
Result<void> read_resource(const JobContext& context) {
  auto& data = *static_cast<AccessContext*>(context.context());
  ++data.readers;
  if (data.writers.load()) data.violation = true;
  for (int i = 0; i < 30; ++i) std::this_thread::yield();
  if (data.writers.load()) data.violation = true;
  --data.readers; ++data.calls;
  return {};
}
void access_tests(TestThreads& threads) {
  auto s = make(threads, {64, 4, 4});
  auto context = std::make_shared<AccessContext>();
  std::array<ResourceAccess, 1> write{{{42, AccessMode::Write}}}, read{{{42, AccessMode::Read}}};
  auto g = group(*s);
  std::vector<JobDesc> jobs(64);
  for (std::size_t i = 0; i < jobs.size(); ++i) {
    jobs[i] = JobDesc{i % 2 ? read_resource : write_resource, context};
    jobs[i].accesses = i % 2 ? std::span<const ResourceAccess>(read) : std::span<const ResourceAccess>(write);
    jobs[i].unknown_access = false;
  }
  CHECK(s->submit_batch(g, jobs)); finish(*s, g, 64); CHECK(!context->violation); CHECK(context->calls == 64); CHECK(s->release(g));
  // Unknown access conflicts even when the other job declares no resources.
  auto unknown = group(*s);
  for (auto& job : jobs) { job.function = write_resource; job.accesses = {}; job.unknown_access = true; }
  CHECK(s->submit_batch(unknown, jobs)); finish(*s, unknown, 64); CHECK(!context->violation); CHECK(s->release(unknown));
}
void retention_and_fifo_tests(TestThreads& threads) {
  auto s = make(threads, {32, 1, 4});
  auto source = group(*s);
  auto prerequisite = s->submit(source, JobDesc{noop}); CHECK(prerequisite);
  finish(*s, source, 1);
  auto destination = group(*s);
  auto gate = std::make_shared<Gate>();
  CHECK(s->submit(destination, JobDesc{gate_job, gate})); gate->await_started();
  std::array<JobHandle, 1> required{prerequisite.value()};
  JobDesc dependent{noop}; dependent.prerequisites = required;
  CHECK(s->submit(destination, dependent));
  error_is(s->release(source), ErrorCode::Busy);
  gate->open(); finish(*s, destination, 2);
  CHECK(s->release(source)); CHECK(s->release(destination));

  auto blocking_group = group(*s);
  gate = std::make_shared<Gate>();
  CHECK(s->submit(blocking_group, JobDesc{gate_job, gate})); gate->await_started();
  auto cancelled_group = group(*s);
  auto calls = std::make_shared<std::atomic<int>>(0);
  CHECK(s->submit(cancelled_group, JobDesc{count, calls}));
  CHECK(s->cancel(cancelled_group));
  CHECK(s->close(cancelled_group));
  auto cancelled = s->wait(cancelled_group, 1s); CHECK(cancelled);
  CHECK(cancelled.value().cancelled == 1 && calls->load() == 0);
  CHECK(s->release(cancelled_group));

  struct Ordered { std::vector<int> order; };
  struct Entry { std::shared_ptr<Ordered> output; int index; };
  auto order = std::make_shared<Ordered>(); order->order.reserve(20);
  auto ordered_group = group(*s);
  std::array<JobDesc, 20> batch;
  for (std::size_t i = 0; i < batch.size(); ++i) {
    auto entry = std::make_shared<Entry>(Entry{order, static_cast<int>(i)});
    batch[i] = JobDesc{[](const JobContext& c) -> Result<void> {
      auto& entry = *static_cast<Entry*>(c.context());
      entry.output->order.push_back(entry.index); return {};
    }, entry};
  }
  CHECK(s->submit_batch(ordered_group, batch));
  gate->open(); finish(*s, blocking_group, 1); finish(*s, ordered_group, 20);
  for (std::size_t i = 0; i < batch.size(); ++i) CHECK(order->order[i] == static_cast<int>(i));
  CHECK(s->release(blocking_group)); CHECK(s->release(ordered_group));
  auto limited = make(threads, {8, 1, 1});
  auto only = group(*limited); error_is(limited->create_group(), ErrorCode::QueueFull);
  std::array<ResourceAccess, 17> too_many{};
  JobDesc invalid{noop}; invalid.accesses = too_many;
  error_is(limited->submit(only, invalid), ErrorCode::InvalidInput);
  CHECK(limited->stats().admitted == 0);
  CHECK(limited->close(only)); CHECK(limited->release(only));
}
void writer_fairness_test(TestThreads& threads) {
  auto s = make(threads, {16, 2, 4});
  std::array<ResourceAccess, 1> reads{{{7, AccessMode::Read}}}, writes{{{7, AccessMode::Write}}};
  auto reader_group = group(*s);
  auto gate = std::make_shared<Gate>();
  JobDesc first{gate_job, gate}; first.accesses = reads; first.unknown_access = false;
  CHECK(s->submit(reader_group, first)); gate->await_started();
  struct Fairness { std::atomic<bool> writer_ran{false}, violated{false}; };
  auto flags = std::make_shared<Fairness>();
  auto queued = group(*s);
  JobDesc writer{[](const JobContext& context) -> Result<void> {
    static_cast<Fairness*>(context.context())->writer_ran = true; return {};
  }, flags};
  writer.accesses = writes; writer.unknown_access = false;
  JobDesc younger_reader{[](const JobContext& context) -> Result<void> {
    auto& state = *static_cast<Fairness*>(context.context());
    if (!state.writer_ran.load()) state.violated = true;
    return {};
  }, flags};
  younger_reader.accesses = reads; younger_reader.unknown_access = false;
  std::array<JobDesc, 2> ordered{writer, younger_reader};
  CHECK(s->submit_batch(queued, ordered));
  auto independent_group = group(*s);
  JobDesc independent{noop}; independent.unknown_access = false;
  CHECK(s->submit(independent_group, independent)); finish(*s, independent_group, 1);
  CHECK(!flags->violated && !flags->writer_ran); // Worker has progressed past conflicting candidates.
  gate->open(); finish(*s, reader_group, 1); finish(*s, queued, 2);
  CHECK(flags->writer_ran && !flags->violated);
  CHECK(s->release(reader_group)); CHECK(s->release(queued)); CHECK(s->release(independent_group));
}
void stress_tests(TestThreads& threads) {
  auto s = make(threads, {96, 4, 16});
  auto calls = std::make_shared<std::atomic<int>>(0);
  std::atomic<int> successful{0}, cancelled{0};
  std::vector<std::thread> producers;
  for (int producer = 0; producer < 6; ++producer) producers.emplace_back([&, producer] {
    for (int round = 0; round < 50; ++round) {
      auto g = group(*s);
      std::array<JobDesc, 8> batch;
      for (auto& job : batch) { job = JobDesc{count, calls}; job.unknown_access = false; }
      for (;;) {
        auto result = s->submit_batch(g, batch);
        if (result) break;
        CHECK(result.error().code == ErrorCode::QueueFull); std::this_thread::yield();
      }
      if ((round + producer) % 3 == 0) CHECK(s->cancel(g));
      CHECK(s->close(g)); auto done = s->wait(g, 10s); CHECK(done);
      CHECK(done.value().failed == 0);
      successful += static_cast<int>(done.value().succeeded);
      cancelled += static_cast<int>(done.value().cancelled);
      CHECK(s->release(g));
    }
  });
  for (auto& producer : producers) producer.join();
  CHECK(successful + cancelled == 2400); CHECK(calls->load() == successful.load());
  CHECK(s->stats().completed == 2400 && s->stats().occupied == 0);
  CHECK(s->shutdown(ShutdownMode::Drain, 10s));
}
}
void jobs_tests() {
  TestThreads threads;
  error_is(Scheduler::create(threads, {0, 1, 1}), ErrorCode::InvalidInput);
  admission_tests(threads); graph_tests(threads); failures_tests(threads);
  shutdown_tests(threads); timeout_contract_tests(threads); drain_cancel_timeout_tests(threads); untimed_join_test();
  access_tests(threads); retention_and_fifo_tests(threads); writer_fairness_test(threads); stress_tests(threads);
}
