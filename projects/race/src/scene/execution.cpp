#include "internal.hpp"
#include <functional>
#include <unordered_set>
namespace race::scene {
struct Execution::Impl {
  struct Frame {
    ReadView view;
    Evaluation evaluation;
    std::vector<System> systems;
    std::vector<std::vector<Command>> outputs;
    Limits limits;
    std::mutex mutex;
    std::size_t bytes{}, commands{};
    bool cancelled{};
  };
  struct Job {
    std::shared_ptr<Frame> frame;
    std::size_t index;
  };
  std::shared_ptr<WorldState> world;
  core::Scheduler *scheduler;
  std::vector<System> systems;
  std::vector<std::vector<std::size_t>> predecessors;
  std::vector<std::size_t> publication_order;
  core::FixedStepper stepper;
  core::Rational step;
  std::shared_ptr<Frame> frame;
  std::optional<core::GroupHandle> group;
  std::vector<core::JobHandle> jobs;
  std::uint64_t published{};
  std::optional<EvaluationMode> mode;
  bool realtime{};
  std::optional<core::StepReport> pending;
  std::uint64_t pending_done{};
  Impl(std::shared_ptr<WorldState> w, core::Scheduler &s, std::vector<System> sy,
       core::FixedStepper clock, core::Rational st)
      : world(std::move(w)), scheduler(&s), systems(std::move(sy)), stepper(clock), step(st) {}
  static Result<void> run(const core::JobContext &job) {
    auto &input = *static_cast<Job *>(job.context());
    auto &f = *input.frame;
    const auto &system = f.systems[input.index];
    if (job.cancelled())
      return fail(ErrorCode::Cancelled, "Scene evaluation cancelled");
    auto output = system.function(SystemContext{f.view, f.evaluation, job}, system.context.get());
    if (!output)
      return output.error();
    Transaction t{std::move(output).value(), EditMode::Simulation};
    auto bytes = transaction_bytes(t, f.limits);
    if (!bytes)
      return bytes.error();
    for (auto &c : t.commands) {
      TypeId key = 0;
      if (c.kind == CommandKind::SetField)
        key = c.type;
      else if (c.kind == CommandKind::Reparent)
        key = transform_type;
      bool allowed = false;
      for (auto access : system.access)
        if (access.mode == core::AccessMode::Write && (access.key == key || access.key == 0))
          allowed = true;
      if (!allowed)
        return fail(ErrorCode::InvalidInput, "system command exceeds declared write access");
    }
    std::lock_guard lock(f.mutex);
    if (bytes.value() > f.limits.transaction_bytes - f.bytes ||
        t.commands.size() > f.limits.commands_per_transaction - f.commands)
      return fail(ErrorCode::Exhausted, "evaluation command staging bound");
    f.bytes += bytes.value();
    f.commands += t.commands.size();
    f.outputs[input.index] = std::move(t.commands);
    return {};
  }
  Result<void> owner() const { return world->owner_check(); }
  void drop_frame() {
    if (frame) {
      frame->view.release();
      std::lock_guard lock(world->mutex);
      world->frame_inflight = world->execution_pending = false;
    }
    frame.reset();
    jobs.clear();
    group.reset();
  }
  Result<void> start(Evaluation evaluation) {
    auto own = owner();
    if (!own)
      return own;
    if (frame)
      return fail(ErrorCode::Busy, "evaluation already in flight");
    if (evaluation.mode != EvaluationMode::FixedTick &&
        evaluation.mode != EvaluationMode::ExactRational)
      return fail(ErrorCode::InvalidInput, "unknown evaluation mode");
    if (mode && *mode != evaluation.mode)
      return fail(ErrorCode::InvalidInput, "one explicit time mode per execution");
    if (published == UINT64_MAX)
      return fail(ErrorCode::Overflow, "evaluation index exhausted");
    if (evaluation.mode == EvaluationMode::FixedTick) {
      if (evaluation.tick.step != step || evaluation.tick.tick_index != published)
        return fail(ErrorCode::InvalidInput, "fixed tick must continue published sequence");
    } else {
      auto expected = step.multiply(core::Rational::make(published, 1).value());
      if (!expected)
        return expected.error();
      if (evaluation.exact_time != expected.value())
        return fail(ErrorCode::InvalidInput, "exact evaluation must continue rational sequence");
    }
    try {
      auto f = std::make_shared<Frame>();
      f->evaluation = evaluation;
      f->systems = systems;
      f->outputs.resize(systems.size());
      f->limits = world->limits;
      {
        std::lock_guard lock(world->mutex);
        if (world->stopping)
          return fail(ErrorCode::Stopping, "world stopping");
        if (world->view_gate_closed() || world->frame_inflight)
          return fail(ErrorCode::Busy, "world publication already reserved");
        if (world->readers == world->limits.views)
          return fail(ErrorCode::Exhausted, "view limit");
        ++world->readers;
        world->frame_inflight = true;
        f->view = ReadView(world);
      }
      frame = std::move(f);
      auto g = scheduler->create_group();
      if (!g) {
        drop_frame();
        return g.error();
      }
      group = g.value();
      std::vector<core::JobDesc> descriptors;
      descriptors.reserve(systems.size());
      for (std::size_t i = 0; i < systems.size(); ++i) {
        auto context = std::make_shared<Job>(Job{frame, i});
        core::JobDesc d{run, std::move(context)};
        d.batch_prerequisites = predecessors[i];
        d.accesses = systems[i].access;
        d.unknown_access = std::any_of(systems[i].access.begin(), systems[i].access.end(),
                                       [](auto a) { return a.key == 0; });
        descriptors.push_back(d);
      }
      auto submitted = scheduler->submit_batch(*group, descriptors);
      if (!submitted) {
        auto close = scheduler->close(*group);
        (void)close;
        auto release = scheduler->release(*group);
        if (release)
          drop_frame();
        else
          frame->cancelled = true;
        return submitted.error();
      }
      jobs = std::move(submitted).value();
      auto closed = scheduler->close(*group);
      if (!closed) {
        auto cancel = scheduler->cancel(*group);
        (void)cancel;
        frame->cancelled = true;
        return closed;
      }
      mode = evaluation.mode;
      return {};
    } catch (const std::bad_alloc &) {
      if (group) {
        auto c = scheduler->cancel(*group);
        (void)c;
        auto close = scheduler->close(*group);
        (void)close;
        if (frame)
          frame->cancelled = true;
      } else
        drop_frame();
      return fail(ErrorCode::Exhausted, "evaluation admission allocation");
    }
  }
  Result<void> finish(std::chrono::milliseconds timeout) {
    auto own = owner();
    if (!own)
      return own;
    if (timeout.count() < 0)
      return fail(ErrorCode::InvalidInput, "negative evaluation timeout");
    if (!frame)
      return fail(ErrorCode::NotFound, "no evaluation in flight");
    if (group) {
      auto waited = scheduler->wait(*group, timeout);
      if (!waited)
        return waited.error();
      std::optional<Error> failure;
      if (frame->cancelled || waited.value().cancelled)
        failure = fail(ErrorCode::Cancelled, "evaluation cancelled; nothing published");
      else if (waited.value().failed) {
        for (auto h : jobs) {
          auto c = scheduler->completion(h);
          if (!c)
            return c.error();
          if (c.value().state == core::JobState::Failed) {
            failure = c.value().error;
            break;
          }
        }
        if (!failure)
          failure = fail(ErrorCode::FailedPrerequisite, "evaluation failed; nothing published");
      }
      // Frame owns its immutable view and command outputs independently. Release
      // all Core terminal records before the Scene publication can become visible.
      auto released = scheduler->release(*group);
      if (!released)
        return released;
      group.reset();
      jobs.clear();
      if (failure) {
        drop_frame();
        return *failure;
      }
    }
    if (frame->cancelled) {
      drop_frame();
      return fail(ErrorCode::Cancelled, "evaluation cancelled before publication");
    }
    try {
      Transaction transaction;
      transaction.mode = EditMode::Simulation;
      transaction.commands.reserve(frame->commands);
      for (auto index : publication_order) {
        auto &out = frame->outputs[index];
        transaction.commands.insert(transaction.commands.end(), out.begin(), out.end());
      }
      std::unique_lock lock(world->mutex);
      world->execution_pending = true;
      if (world->readers != 1)
        return fail(ErrorCode::Busy, "other readers retain evaluation input");
      if (world->stopping) {
        lock.unlock();
        drop_frame();
        return fail(ErrorCode::Cancelled, "world stopped before evaluation publication");
      }
      auto retained = std::move(frame->view.state_);
      --world->readers;
      auto result = world->apply_locked(transaction, false, false, true);
      if (!result) {
        ++world->readers;
        frame->view.state_ = std::move(retained);
        world->execution_pending = false;
        lock.unlock();
        drop_frame();
        return result;
      }
      ++published;
      if (pending && frame->evaluation.mode == EvaluationMode::FixedTick)
        ++pending_done;
      world->execution_pending = world->frame_inflight = false;
      lock.unlock();
      world->readers_done.notify_all();
      frame.reset();
      return {};
    } catch (const std::bad_alloc &) {
      return fail(ErrorCode::Exhausted, "evaluation publication planning allocation");
    }
  }
};
Execution::Execution(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result<std::unique_ptr<Execution>> Execution::create(World &world, core::Scheduler &scheduler,
                                                     std::vector<System> systems,
                                                     core::Rational step,
                                                     core::RealtimePolicy policy) {
  auto clock = core::FixedStepper::create(step, policy);
  if (!clock)
    return clock.error();
  if (systems.empty() || systems.size() > 128)
    return fail(ErrorCode::InvalidInput, "bounded nonempty system schedule required");
  auto own = world.state_->owner_check();
  if (!own)
    return own.error();
  try {
    std::unordered_map<std::uint64_t, std::size_t> index;
    for (std::size_t i = 0; i < systems.size(); ++i) {
      auto &s = systems[i];
      if (!s.id || !s.function || s.predecessors.size() > core::Scheduler::max_dependencies ||
          s.access.size() > core::Scheduler::max_accesses)
        return fail(ErrorCode::InvalidInput, "invalid system declaration");
      if (!index.emplace(s.id, i).second)
        return fail(ErrorCode::Duplicate, "duplicate system identity");
      std::unordered_set<std::uint64_t> access;
      for (auto a : s.access) {
        if ((a.key && !world.state_->schemas->find(a.key)) ||
            (a.mode != core::AccessMode::Read && a.mode != core::AccessMode::Write))
          return fail(ErrorCode::InvalidInput, "invalid system component access");
        if (!access.insert(a.key).second)
          return fail(ErrorCode::Duplicate, "duplicate system access");
      }
      s.access = std::vector<core::ResourceAccess>(s.access.begin(), s.access.end());
      s.predecessors = std::vector<std::uint64_t>(s.predecessors.begin(), s.predecessors.end());
    }
    systems = std::vector<System>(systems.begin(), systems.end());
    auto impl =
        std::make_unique<Impl>(world.state_, scheduler, std::move(systems), clock.value(), step);
    impl->predecessors.resize(impl->systems.size());
    for (std::size_t i = 0; i < impl->systems.size(); ++i) {
      std::unordered_set<std::uint64_t> unique;
      for (auto id : impl->systems[i].predecessors) {
        auto at = index.find(id);
        if (at == index.end())
          return fail(ErrorCode::InvalidDependency, "system predecessor missing");
        if (!unique.insert(id).second)
          return fail(ErrorCode::Duplicate, "duplicate system predecessor");
        impl->predecessors[i].push_back(at->second);
      }
    }
    std::vector<unsigned char> visited(impl->systems.size());
    std::function<Result<void>(std::size_t)> visit = [&](std::size_t i) -> Result<void> {
      if (visited[i] == 1)
        return fail(ErrorCode::Cycle, "system schedule cycle");
      if (visited[i] == 2)
        return {};
      visited[i] = 1;
      for (auto p : impl->predecessors[i]) {
        auto r = visit(p);
        if (!r)
          return r;
      }
      visited[i] = 2;
      impl->publication_order.push_back(i);
      return {};
    };
    for (std::size_t i = 0; i < visited.size(); ++i) {
      auto r = visit(i);
      if (!r)
        return r.error();
    }
    return std::unique_ptr<Execution>(new Execution(std::move(impl)));
  } catch (const std::bad_alloc &) {
    return fail(ErrorCode::Exhausted, "system schedule allocation");
  }
}
Execution::~Execution() {
  if (!impl_->owner())
    std::abort();
  if (impl_->group) {
    auto cancelled = impl_->scheduler->cancel(*impl_->group);
    (void)cancelled;
    auto closed = impl_->scheduler->close(*impl_->group);
    (void)closed;
    auto waited = impl_->scheduler->wait(*impl_->group, std::chrono::milliseconds::max());
    if (!waited)
      std::abort();
    auto released = impl_->scheduler->release(*impl_->group);
    (void)released;
  }
  impl_->drop_frame();
}
Result<void> Execution::evaluate(Evaluation e, std::chrono::milliseconds timeout) {
  if (timeout.count() < 0)
    return fail(ErrorCode::InvalidInput, "negative evaluation timeout");
  if (impl_->realtime)
    return fail(ErrorCode::Busy, "realtime execution is driven by advance");
  auto started = impl_->start(e);
  if (!started)
    return started;
  return impl_->finish(timeout);
}
Result<void> Execution::poll(std::chrono::milliseconds timeout) { return impl_->finish(timeout); }
Result<void> Execution::cancel() {
  auto own = impl_->owner();
  if (!own)
    return own;
  if (!impl_->frame)
    return fail(ErrorCode::NotFound, "no evaluation in flight");
  if (impl_->group) {
    auto cancelled = impl_->scheduler->cancel(*impl_->group);
    if (!cancelled)
      return cancelled;
  }
  impl_->frame->cancelled = true;
  return {};
}

Result<core::StepReport> Execution::advance(platform::MonotonicTime now,
                                            std::chrono::milliseconds timeout) {
  auto own = impl_->owner();
  if (!own)
    return own.error();
  if (timeout.count() < 0)
    return fail(ErrorCode::InvalidInput, "negative evaluation timeout");
  if ((impl_->mode && *impl_->mode != EvaluationMode::FixedTick) ||
      (!impl_->realtime && (impl_->published || impl_->frame)))
    return fail(ErrorCode::InvalidInput, "execution is not in realtime mode");
  impl_->realtime = true;
  if (impl_->frame) {
    auto r = impl_->finish(timeout);
    if (!r)
      return r.error();
  }
  if (!impl_->pending) {
    auto report = impl_->stepper.advance(now);
    if (!report)
      return report.error();
    impl_->pending = report.value();
    impl_->pending_done = 0;
  }
  while (impl_->pending_done < impl_->pending->steps) {
    Evaluation e{EvaluationMode::FixedTick,
                 {impl_->pending->first_tick + impl_->pending_done, impl_->step},
                 {}};
    auto r = impl_->start(e);
    if (!r)
      return r.error();
    r = impl_->finish(timeout);
    if (!r)
      return r.error();
  }
  auto result = *impl_->pending;
  impl_->pending.reset();
  impl_->pending_done = 0;
  return result;
}
Result<void> Execution::resume(platform::MonotonicTime now) {
  auto own = impl_->owner();
  if (!own)
    return own;
  if (impl_->frame || impl_->pending)
    return fail(ErrorCode::Busy, "evaluation still pending");
  return impl_->stepper.resume(now);
}
std::uint64_t Execution::published_tick() const noexcept { return impl_->published; }
} // namespace race::scene
