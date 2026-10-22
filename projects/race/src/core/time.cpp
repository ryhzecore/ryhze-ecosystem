#include "race/core/time.hpp"
#include <algorithm>
#include <limits>
#include <numeric>
#include <utility>

namespace race::core {
namespace {
constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
Result<std::uint64_t> product(std::uint64_t a, std::uint64_t b) {
  if (b != 0 && a > limit / b) return fail(ErrorCode::Overflow, "rational multiplication overflow");
  return a * b;
}
Result<Rational> combine(Rational a, Rational b, bool subtract) {
  const auto divisor = std::gcd(a.denominator(), b.denominator());
  auto left = product(a.numerator(), b.denominator() / divisor);
  auto right = product(b.numerator(), a.denominator() / divisor);
  auto denominator = product(a.denominator(), b.denominator() / divisor);
  if (!left) return left.error();
  if (!right) return right.error();
  if (!denominator) return denominator.error();
  if (subtract && left.value() < right.value()) return fail(ErrorCode::InvalidInput, "negative time is unsupported");
  if (!subtract && left.value() > limit - right.value()) return fail(ErrorCode::Overflow, "rational addition overflow");
  return Rational::make(subtract ? left.value() - right.value() : left.value() + right.value(), denominator.value());
}
bool valid_identity(const CheckpointIdentity& identity) {
  const auto valid = [](const std::string& value) { return !value.empty() && value.size() <= 8192; };
  return valid(identity.source_assets) && valid(identity.module_versions) &&
         valid(identity.configuration) && valid(identity.time_convention);
}
}
Result<Rational> Rational::make(std::uint64_t n, std::uint64_t d) {
  if (d == 0) return fail(ErrorCode::InvalidInput, "zero rational denominator");
  const auto divisor = std::gcd(n, d);
  return Rational(n / divisor, d / divisor);
}
Result<Rational> Rational::add(Rational b) const { return combine(*this, b, false); }
Result<Rational> Rational::subtract(Rational b) const { return combine(*this, b, true); }
Result<Rational> Rational::multiply(Rational b) const {
  const auto cross1 = std::gcd(numerator_, b.denominator_);
  const auto cross2 = std::gcd(b.numerator_, denominator_);
  auto n = product(numerator_ / cross1, b.numerator_ / cross2);
  auto d = product(denominator_ / cross2, b.denominator_ / cross1);
  if (!n) return n.error();
  if (!d) return d.error();
  return Rational::make(n.value(), d.value());
}
Result<Rational> Rational::divide(Rational b) const {
  if (b.numerator_ == 0) return fail(ErrorCode::InvalidInput, "division by zero rational");
  return multiply(Rational(b.denominator_, b.numerator_));
}
Result<Rational> FilmTime::seconds() const {
  if (subsample.numerator() >= subsample.denominator()) return fail(ErrorCode::InvalidInput, "subsample outside frame");
  auto frame = Rational::make(frame_index, 1).value().add(subsample);
  if (!frame) return frame.error();
  return frame.value().divide(rate);
}
Result<FixedStepper> FixedStepper::create(Rational step, RealtimePolicy policy) {
  if (step.numerator() == 0 || policy.max_catch_up_steps == 0 || policy.max_backlog_steps == 0)
    return fail(ErrorCode::InvalidInput, "invalid fixed-step policy");
  return FixedStepper(step, policy);
}
Result<StepReport> FixedStepper::advance(platform::MonotonicTime now) {
  if (!initialized_) {
    previous_ = now; initialized_ = true;
    return StepReport{};
  }
  if (now.domain != previous_.domain || now.nanoseconds < previous_.nanoseconds)
    return fail(ErrorCode::InvalidInput, "monotonic clock domain or order mismatch");
  if (paused_) return fail(ErrorCode::Busy, "fixed-step clock is paused");
  auto elapsed = Rational::make(now.nanoseconds - previous_.nanoseconds, 1'000'000'000).value();
  auto accumulated = accumulator_.add(elapsed);
  if (!accumulated) return accumulated.error();
  auto units = accumulated.value().divide(step_);
  if (!units) return units.error();
  const auto available = units.value().numerator() / units.value().denominator();
  const auto admitted = std::min(available, policy_.max_catch_up_steps);
  StepReport report;
  report.first_tick = tick_; report.steps = admitted;
  report.pending_whole_steps = available - admitted;
  report.overloaded = available > policy_.max_catch_up_steps;
  if (report.pending_whole_steps > policy_.max_backlog_steps) {
    if (policy_.overload_action == OverloadAction::RejectElapsed)
      return fail(ErrorCode::Exhausted, "elapsed interval exceeds backlog policy");
    // Explicit pause rejects this entire wall interval and preserves all already
    // admitted simulation backlog. No simulation tick is erased or advanced.
    auto prior_units = accumulator_.divide(step_);
    if (!prior_units) return prior_units.error();
    report.pending_whole_steps = prior_units.value().numerator() / prior_units.value().denominator();
    report.interpolation = Rational::make(prior_units.value().numerator() % prior_units.value().denominator(),
                                         prior_units.value().denominator()).value();
    report.paused = true; report.steps = 0; report.rejected_elapsed = elapsed;
    paused_ = true; previous_ = now;
    return report;
  }
  if (tick_ > limit - admitted) return fail(ErrorCode::Overflow, "simulation tick overflow");
  auto consumed = step_.multiply(Rational::make(admitted, 1).value());
  if (!consumed) return consumed.error();
  auto remaining = accumulated.value().subtract(consumed.value());
  if (!remaining) return remaining.error();
  report.interpolation = Rational::make(units.value().numerator() % units.value().denominator(), units.value().denominator()).value();
  accumulator_ = remaining.value(); tick_ += admitted; previous_ = now;
  return report;
}
Result<void> FixedStepper::resume(platform::MonotonicTime now) {
  if (!initialized_ || !paused_) return fail(ErrorCode::InvalidInput, "resume requires paused clock");
  if (now.domain != previous_.domain || now.nanoseconds < previous_.nanoseconds)
    return fail(ErrorCode::InvalidInput, "invalid resume clock");
  previous_ = now; paused_ = false;
  return {};
}
OfflineSequence::OfflineSequence(OfflineRequest request)
  : request_(std::move(request)), history_ready_(request_.first_frame == 0) {}
Result<OfflineSequence> OfflineSequence::create(OfflineRequest request) {
  if (request.frame_count == 0 || request.samples_per_frame == 0 || request.rate.numerator() == 0 || !valid_identity(request.identity))
    return fail(ErrorCode::InvalidInput, "invalid offline request");
  if (request.first_frame > limit - (request.frame_count - 1)) return fail(ErrorCode::Overflow, "offline frame range overflow");
  return OfflineSequence(std::move(request));
}
Result<void> OfflineSequence::restore_checkpoint(const CheckpointIdentity& identity, std::uint64_t frame) {
  if (frame_offset_ != 0 || sample_ != 0) return fail(ErrorCode::Busy, "offline sequence already progressed");
  if (identity != request_.identity || frame != request_.first_frame)
    return fail(ErrorCode::IncompatibleVersion, "checkpoint identity or frame mismatch");
  history_ready_ = true;
  return {};
}
Result<FilmTime> OfflineSequence::next() const {
  if (!history_ready_) return fail(ErrorCode::NotFound, "compatible checkpoint or resimulation required");
  if (finished()) return fail(ErrorCode::NotFound, "offline sequence complete");
  return FilmTime{request_.first_frame + frame_offset_, request_.rate, Rational::make(sample_, request_.samples_per_frame).value()};
}
Result<void> OfflineSequence::commit(const CheckpointIdentity& identity, bool success) {
  if (identity != request_.identity) return fail(ErrorCode::IncompatibleVersion, "offline commit identity mismatch");
  auto current = next();
  if (!current) return current.error();
  if (!success) return fail(ErrorCode::FailedPrerequisite, "offline required stage failed");
  ++sample_;
  if (sample_ == request_.samples_per_frame) { sample_ = 0; ++frame_offset_; }
  return {};
}
}
