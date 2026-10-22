#include "test.hpp"
#include "race/runtime/lifecycle.hpp"
#include <atomic>
#include <thread>
using namespace race;
using namespace std::chrono_literals;
namespace {
struct Trace { std::vector<std::string> events; std::atomic<bool> active{false}, release{false}; };
class Mock final : public runtime::Module {
  runtime::ModuleDescriptor descriptor_;
  Trace& trace_;
  int failure_;
  runtime::ModuleContext context_;
  std::optional<OwnedBlock> resource_;
public:
  Mock(std::string name, std::vector<std::string> dependencies, Trace& trace, int failure = 0)
    : descriptor_{std::move(name),1,std::move(dependencies),0,runtime::Memory|runtime::Jobs}, trace_(trace), failure_(failure) {}
  auto& edit() { return descriptor_; }
  const runtime::ModuleDescriptor& descriptor() const noexcept override { return descriptor_; }
  void event(const char* text) { trace_.events.push_back(descriptor_.id + text); }
  Result<void> validate() noexcept override { event(".validate"); if (failure_ == 1) return fail(ErrorCode::InvalidInput,"injected validation"); return {}; }
  Result<void> initialize(runtime::ModuleContext c) noexcept override {
    context_ = c; CHECK(c.memory && c.jobs && !c.clock && !c.diagnostics); event(".initialize");
    auto block = c.memory->allocate({64}); if (!block) return block.error(); resource_.emplace(std::move(block).value());
    if (failure_ == 2) return fail(ErrorCode::Exhausted,"injected initialize");
    return {};
  }
  Result<void> start() noexcept override {
    event(".start"); if (failure_ == 3) return fail(ErrorCode::InvalidInput,"injected start");
    if (failure_ == 4) {
      auto alias = std::shared_ptr<void>(&trace_, [](void*){});
      core::JobDesc job; job.owned_context = alias;
      job.function = [](const core::JobContext& ctx)->Result<void> {
        auto& t = *static_cast<Trace*>(ctx.context()); t.active.store(true);
        while (!t.release.load()) std::this_thread::yield();
        t.active.store(false); return {};
      };
      auto submitted = context_.jobs->submit(context_.task_group, job); if (!submitted) return submitted.error();
      while (!trace_.active.load()) std::this_thread::yield();
    }
    return {};
  }
  void request_stop() noexcept override { event(".stop"); }
  Result<void> quiesce(std::chrono::milliseconds) noexcept override {
    CHECK(!trace_.active.load()); event(".quiesce");
    if (failure_ == 5 && !trace_.release.load()) return fail(ErrorCode::Timeout,"external consumer pending");
    return {};
  }
  void shutdown() noexcept override { CHECK(resource_); event(".shutdown"); resource_.reset(); }
  void rollback_initialization() noexcept override { event(".rollback"); resource_.reset(); }
};
}
void lifecycle_tests() {
  auto p = platform::make_services(); CHECK(p); auto services = std::move(p).value();
  auto a = TrackedAllocator::create({4096}); CHECK(a); auto memory = std::move(a).value();
  auto s = core::Scheduler::create(*services.threads, {16,2,8}); CHECK(s); auto jobs = std::move(s).value();
  core::Diagnostics diagnostics(*services.clock);
  for (int bad_module = 0; bad_module < 2; ++bad_module) for (int phase = 0; phase < 4; ++phase) {
    Trace trace;
    runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
    CHECK(runtime.add(std::make_unique<Mock>("consumer",std::vector<std::string>{"producer"},trace,bad_module==1 ? phase : 0)));
    CHECK(runtime.add(std::make_unique<Mock>("producer",std::vector<std::string>{},trace,bad_module==0 ? phase : 0)));
    auto started = runtime.start(); CHECK(static_cast<bool>(started) == (phase == 0));
    CHECK(runtime.stop(100ms)); auto events = trace.events; CHECK(runtime.stop(0ms)); CHECK(trace.events == events);
    CHECK(memory.snapshot().total.live == 0);
    auto pos = [&](const char* name) { return std::find(trace.events.begin(),trace.events.end(),name)-trace.events.begin(); };
    if (phase == 0) {
      CHECK(pos("producer.start") < pos("consumer.start"));
      CHECK(pos("consumer.shutdown") < pos("producer.stop"));
    }
    if (phase == 2) CHECK(pos(bad_module ? "consumer.rollback":"producer.rollback") < static_cast<long>(trace.events.size()));
  }
  for (int invalid = 0; invalid < 4; ++invalid) {
    Trace trace; runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
    auto mock = std::make_unique<Mock>("a",std::vector<std::string>{},trace);
    if (invalid == 0) mock->edit().dependencies = {"missing"};
    if (invalid == 1) mock->edit().dependencies = {"a"};
    if (invalid == 2) mock->edit().contract_version = 2;
    if (invalid == 3) mock->edit().required_capabilities = 1;
    CHECK(runtime.add(std::move(mock))); CHECK(!runtime.start()); CHECK(trace.events.empty());
  }
  {
    Trace trace; runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
    CHECK(runtime.add(std::make_unique<Mock>("a",std::vector<std::string>{"b"},trace)));
    CHECK(runtime.add(std::make_unique<Mock>("b",std::vector<std::string>{"a"},trace)));
    CHECK_ERROR(runtime.start(),ErrorCode::Cycle);
  }
  {
    Trace trace; runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
    CHECK(runtime.add(std::make_unique<Mock>("active",std::vector<std::string>{},trace,4)));
    CHECK(runtime.start()); CHECK_ERROR(runtime.stop(0ms),ErrorCode::Timeout);
    CHECK(memory.snapshot().total.live == 64); CHECK(trace.active.load());
    CHECK(runtime.state("active").value() == runtime::ModuleState::Stopping);
    trace.release.store(true); CHECK(runtime.stop(1s)); CHECK(memory.snapshot().total.live == 0);
    auto events = trace.events; CHECK(runtime.stop(1s)); CHECK(trace.events == events);
  }
  {
    Trace trace; runtime::Runtime runtime(memory,*jobs,*services.clock,diagnostics);
    CHECK(runtime.add(std::make_unique<Mock>("dependency",std::vector<std::string>{},trace)));
    CHECK(runtime.add(std::make_unique<Mock>("external",std::vector<std::string>{"dependency"},trace,5)));
    CHECK(runtime.start());
    std::thread wrong([&]{ CHECK_ERROR(runtime.stop(1ms),ErrorCode::WrongThread); }); wrong.join();
    CHECK_ERROR(runtime.stop(0ms),ErrorCode::Timeout);
    CHECK(memory.snapshot().total.live == 128);
    CHECK(runtime.state("dependency").value() == runtime::ModuleState::Running);
    trace.release.store(true); CHECK(runtime.stop(1s)); CHECK(memory.snapshot().total.live == 0);
  }
  CHECK(jobs->shutdown(core::ShutdownMode::Drain,1s));
}
