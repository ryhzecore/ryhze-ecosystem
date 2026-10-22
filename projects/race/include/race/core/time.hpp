#pragma once
#include "race/foundation/result.hpp"
#include "race/platform/platform.hpp"
#include <cstdint>
#include <string>

namespace race::core {
class Rational {
  std::uint64_t numerator_ = 0, denominator_ = 1;
  Rational(std::uint64_t n, std::uint64_t d) : numerator_(n), denominator_(d) {}
public:
  Rational() = default;
  static Result<Rational> make(std::uint64_t numerator, std::uint64_t denominator);
  std::uint64_t numerator() const noexcept { return numerator_; }
  std::uint64_t denominator() const noexcept { return denominator_; }
  Result<Rational> add(Rational) const;
  Result<Rational> subtract(Rational) const;
  Result<Rational> multiply(Rational) const;
  Result<Rational> divide(Rational) const;
  friend bool operator==(Rational, Rational) = default;
};
struct SimulationTime { std::uint64_t tick_index; Rational step; };
struct FilmTime {
  std::uint64_t frame_index;
  Rational rate;
  Rational subsample;
  Result<Rational> seconds() const;
};
enum class OverloadAction { RejectElapsed, Pause };
struct RealtimePolicy {
  std::uint64_t max_catch_up_steps;
  std::uint64_t max_backlog_steps;
  OverloadAction overload_action;
};
struct StepReport {
  std::uint64_t first_tick = 0, steps = 0, pending_whole_steps = 0;
  Rational interpolation, rejected_elapsed;
  bool overloaded = false, paused = false;
};
// Single owner thread. No simulation callbacks run here: callers execute each
// admitted tick in order, then publish state. An excessive interval is explicit.
class FixedStepper {
  Rational step_, accumulator_;
  RealtimePolicy policy_{};
  platform::MonotonicTime previous_{};
  std::uint64_t tick_ = 0;
  bool initialized_ = false, paused_ = false;
  FixedStepper(Rational step, RealtimePolicy policy) : step_(step), policy_(policy) {}
public:
  static Result<FixedStepper> create(Rational step, RealtimePolicy);
  Result<StepReport> advance(platform::MonotonicTime);
  Result<void> resume(platform::MonotonicTime);
  SimulationTime simulation_time() const noexcept { return {tick_, step_}; }
  Rational backlog() const noexcept { return accumulator_; }
  bool paused() const noexcept { return paused_; }
};
struct CheckpointIdentity {
  std::string source_assets, module_versions, configuration, time_convention;
  std::uint64_t seed = 0;
  friend bool operator==(const CheckpointIdentity&, const CheckpointIdentity&) = default;
};
struct OfflineRequest {
  std::uint64_t first_frame, frame_count, samples_per_frame;
  Rational rate;
  CheckpointIdentity identity;
};
// Ordered mock evaluation/commit protocol; no media output or stateful simulator.
// Nonzero seeking requires a checkpoint at the requested frame. Resimulation is
// a later consumer responsibility, never implied by assigning a new frame index.
class OfflineSequence {
  OfflineRequest request_;
  std::uint64_t frame_offset_ = 0, sample_ = 0;
  bool history_ready_ = false;
  explicit OfflineSequence(OfflineRequest request);
public:
  static Result<OfflineSequence> create(OfflineRequest);
  Result<void> restore_checkpoint(const CheckpointIdentity&, std::uint64_t frame);
  Result<FilmTime> next() const;
  Result<void> commit(const CheckpointIdentity&, bool required_stages_succeeded);
  bool finished() const noexcept { return frame_offset_ == request_.frame_count; }
};
}
