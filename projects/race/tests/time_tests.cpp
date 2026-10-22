#include "race/core/time.hpp"
#include "test.hpp"
#include <limits>

namespace {
race::core::Rational ratio(std::uint64_t n, std::uint64_t d = 1) {
  auto result = race::core::Rational::make(n, d); CHECK(result); return result.value();
}
class FakeClock final : public race::platform::Clock {
public:
  std::uint64_t ticks = 0, domain = 4;
  race::platform::MonotonicTime now() const noexcept override { return {ticks, domain}; }
};
void check_paused_interpolation(std::uint64_t initial_ns, std::uint64_t previous_tick,
                                race::core::Rational previous_backlog,
                                std::uint64_t pending_whole_steps,
                                race::core::Rational interpolation) {
  using namespace race; using namespace race::core;
  FakeClock clock;
  const auto step = ratio(1, 2);
  auto stepper = FixedStepper::create(step, {1, 3, OverloadAction::Pause}).value();
  CHECK(stepper.advance(clock.now()));
  clock.ticks = initial_ns;
  auto report = stepper.advance(clock.now()); CHECK(report);
  CHECK(report.value().steps == previous_tick);
  CHECK(report.value().pending_whole_steps == pending_whole_steps);
  CHECK(report.value().interpolation == interpolation);
  CHECK(stepper.simulation_time().tick_index == previous_tick);
  CHECK(stepper.backlog() == previous_backlog);

  clock.ticks += 3'000'000'000;
  report = stepper.advance(clock.now()); CHECK(report);
  CHECK(report.value().paused); CHECK(report.value().overloaded); CHECK(stepper.paused());
  CHECK(report.value().first_tick == previous_tick); CHECK(report.value().steps == 0);
  CHECK(report.value().pending_whole_steps == pending_whole_steps);
  CHECK(report.value().interpolation == interpolation);
  CHECK(report.value().rejected_elapsed == ratio(3));
  CHECK(stepper.simulation_time().tick_index == previous_tick);
  CHECK(stepper.backlog() == previous_backlog);
  CHECK_ERROR(stepper.advance(clock.now()), ErrorCode::Busy);

  // Resume reanchors the clock: none of the ten paused seconds is accumulated.
  clock.ticks += 10'000'000'000;
  CHECK(stepper.resume(clock.now())); CHECK(!stepper.paused());
  report = stepper.advance(clock.now()); CHECK(report);
  CHECK(!report.value().paused); CHECK(!report.value().overloaded);
  CHECK(report.value().first_tick == previous_tick);
  CHECK(report.value().steps == pending_whole_steps);
  CHECK(report.value().pending_whole_steps == 0);
  CHECK(report.value().interpolation == interpolation);
  CHECK(report.value().rejected_elapsed == ratio(0));
  CHECK(stepper.simulation_time().tick_index == previous_tick + pending_whole_steps);
  CHECK(stepper.backlog() == interpolation.multiply(step).value());
}
void pause_and_rejection_regressions() {
  using namespace race; using namespace race::core;
  // Fractional-only, whole-plus-fractional, whole-only and empty retained work.
  // A half-second step distinguishes fractional step units from raw seconds.
  check_paused_interpolation(250'000'000, 0, ratio(1, 4), 0, ratio(1, 2));
  check_paused_interpolation(1'250'000'000, 1, ratio(3, 4), 1, ratio(1, 2));
  check_paused_interpolation(1'000'000'000, 1, ratio(1, 2), 1, ratio(0));
  check_paused_interpolation(0, 0, ratio(0), 0, ratio(0));

  FakeClock clock;
  auto reject = FixedStepper::create(ratio(1, 2), {1, 3, OverloadAction::RejectElapsed}).value();
  CHECK(reject.advance(clock.now()));
  clock.ticks = 1'250'000'000;
  auto report = reject.advance(clock.now()); CHECK(report); CHECK(report.value().steps == 1);
  CHECK(reject.backlog() == ratio(3, 4));
  clock.ticks += 3'000'000'000;
  CHECK_ERROR(reject.advance(clock.now()), ErrorCode::Exhausted);
  CHECK(!reject.paused()); CHECK(reject.simulation_time().tick_index == 1);
  CHECK(reject.backlog() == ratio(3, 4));
  // The rejected sample must not become the anchor. An earlier valid sample
  // contributes only 250 ms to the retained 750 ms, then admits one 500 ms tick.
  clock.ticks = 1'500'000'000;
  report = reject.advance(clock.now()); CHECK(report);
  CHECK(report.value().first_tick == 1); CHECK(report.value().steps == 1);
  CHECK(report.value().pending_whole_steps == 1);
  CHECK(report.value().interpolation == ratio(0));
  CHECK(report.value().rejected_elapsed == ratio(0)); CHECK(!report.value().paused);
  CHECK(reject.simulation_time().tick_index == 2); CHECK(reject.backlog() == ratio(1, 2));
}
}
void time_tests() {
  using namespace race; using namespace race::core;
  CHECK(ratio(2, 4) == ratio(1, 2)); CHECK(ratio(0, 17) == ratio(0));
  CHECK(!Rational::make(1, 0)); CHECK(!ratio(1).divide(ratio(0)));
  CHECK(ratio(1, 3).add(ratio(1, 6)).value() == ratio(1, 2));
  CHECK(ratio(1, 2).subtract(ratio(1, 3)).value() == ratio(1, 6));
  CHECK(!ratio(0).subtract(ratio(1)));
  constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
  CHECK(!ratio(maximum).add(ratio(1))); CHECK(!ratio(maximum).multiply(ratio(2)));
  CHECK(ratio(maximum, 2).multiply(ratio(2, maximum)).value() == ratio(1));
  CHECK(!FixedStepper::create(ratio(0), {1, 1, OverloadAction::Pause}));
  CHECK(!FixedStepper::create(ratio(1), {0, 1, OverloadAction::Pause}));
  FakeClock clock;
  auto stepper = FixedStepper::create(ratio(1, 60), {4, 8, OverloadAction::Pause}).value();
  CHECK(stepper.advance(clock.now()).value().steps == 0);
  clock.ticks = 25'000'000;
  auto report = stepper.advance(clock.now()); CHECK(report); CHECK(report.value().steps == 1);
  CHECK(report.value().interpolation == ratio(1, 2)); CHECK(stepper.backlog() == ratio(1, 120));
  auto wrong_domain = stepper.advance({clock.ticks, 5}); CHECK(!wrong_domain);
  CHECK(!stepper.advance({clock.ticks - 1, clock.domain}));
  CHECK(stepper.simulation_time().tick_index == 1);
  clock.ticks += 100'000'000;
  report = stepper.advance(clock.now()); CHECK(report); CHECK(report.value().steps == 4);
  CHECK(report.value().overloaded); CHECK(report.value().pending_whole_steps == 2);
  CHECK(report.value().interpolation == ratio(1, 2));
  report = stepper.advance(clock.now()); CHECK(report); CHECK(report.value().steps == 2);
  CHECK(!report.value().overloaded);
  const auto previous_tick = stepper.simulation_time().tick_index;
  const auto previous_backlog = stepper.backlog();
  clock.ticks += 1'000'000'000;
  report = stepper.advance(clock.now()); CHECK(report); CHECK(report.value().paused);
  CHECK(report.value().steps == 0); CHECK(report.value().rejected_elapsed == ratio(1));
  CHECK(stepper.simulation_time().tick_index == previous_tick); CHECK(stepper.backlog() == previous_backlog);
  CHECK(!stepper.advance(clock.now())); CHECK(!stepper.resume({clock.ticks, 5}));
  clock.ticks += 500'000'000; CHECK(stepper.resume(clock.now()));
  CHECK(stepper.advance(clock.now()).value().steps == 0);
  CHECK(!stepper.resume(clock.now()));
  auto reject = FixedStepper::create(ratio(1, 60), {4, 8, OverloadAction::RejectElapsed}).value();
  CHECK(reject.advance({0, 1})); CHECK(!reject.advance({1'000'000'000, 1}));
  CHECK(reject.simulation_time().tick_index == 0); CHECK(reject.backlog() == ratio(0));
  CHECK(reject.advance({100'000'000, 1}).value().steps == 4);
  pause_and_rejection_regressions();
  // Accumulation of 1ms intervals over 10 minutes remains exact: no doubles.
  auto long_run = FixedStepper::create(ratio(1, 60), {4, 8, OverloadAction::Pause}).value();
  CHECK(long_run.advance({0, 7}));
  for (std::uint64_t i = 1; i <= 600'000; ++i) CHECK(long_run.advance({i * 1'000'000, 7}));
  CHECK(long_run.simulation_time().tick_index == 36'000); CHECK(long_run.backlog() == ratio(0));
  CHECK(FilmTime{24, ratio(24), ratio(0)}.seconds().value() == ratio(1));
  CHECK(FilmTime{24, ratio(24000, 1001), ratio(0)}.seconds().value() == ratio(1001, 1000));
  CHECK(FilmTime{0, ratio(24), ratio(1, 2)}.seconds().value() == ratio(1, 48));
  CHECK(!FilmTime{0, ratio(24), ratio(1)}.seconds());
  CHECK(!FilmTime{maximum, ratio(1), ratio(1, 2)}.seconds());
  CheckpointIdentity identity{"source-assets-v1", "mock-v1", "config-v1", "frame-sample-v1", 37};
  OfflineRequest request{0, 3, 4, ratio(24000, 1001), identity};
  auto offline = OfflineSequence::create(request).value();
  auto wrong = identity; ++wrong.seed;
  CHECK(!offline.commit(wrong, true)); CHECK(!offline.commit(identity, false));
  CHECK(offline.next().value().frame_index == 0);
  for (std::uint64_t frame = 0; frame < 3; ++frame) {
    for (std::uint64_t sample = 0; sample < 4; ++sample) {
      auto next = offline.next(); CHECK(next); CHECK(next.value().frame_index == frame);
      CHECK(next.value().subsample == ratio(sample, 4)); CHECK(offline.commit(identity, true));
    }
  }
  CHECK(offline.finished()); CHECK(!offline.next()); CHECK(!offline.commit(identity, true));
  request.first_frame = 100;
  auto seek = OfflineSequence::create(request).value(); CHECK(!seek.next());
  CHECK(!seek.restore_checkpoint(wrong, 100)); CHECK(!seek.restore_checkpoint(identity, 99));
  CHECK(!seek.commit(identity, true)); CHECK(seek.restore_checkpoint(identity, 100));
  CHECK(seek.next().value().frame_index == 100); CHECK(seek.commit(identity, true));
  CHECK(!seek.restore_checkpoint(identity, 100));
  request.first_frame = maximum; CHECK(!OfflineSequence::create(request));
  request.first_frame = 0; request.samples_per_frame = 0; CHECK(!OfflineSequence::create(request));
  request.samples_per_frame = 1; request.identity.configuration.clear(); CHECK(!OfflineSequence::create(request));
}
