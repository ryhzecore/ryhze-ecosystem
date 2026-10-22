#pragma once
#include "race/platform/directory_watch.hpp"

namespace race::platform::directory_watch::detail {
// Pure state reducer shared by the native adapter and deterministic fault tests.
// Constructor inputs are admitted by the service factory (both nonzero).
class Invalidation {
  Snapshot state_;
  std::uint64_t limit_;
public:
  Invalidation(std::uint64_t identity,std::uint64_t limit):limit_(limit){state_.observation={identity,1};}
  Snapshot snapshot() const noexcept {return state_;}
  void stop_requested() noexcept {if(state_.phase!=Phase::Stopped)state_.phase=Phase::Stopping;}
  void stopped() noexcept {state_.phase=Phase::Stopped;}
  void failure(Error error) noexcept {
    if(!state_.failure)state_.failure=error;state_.dirty=true;
  }
  // A failed rearm can itself report lost changes, with no outstanding I/O.
  // Retain dirty state and retry one arm on the next poll. Other failures stop.
  void arm_failed(Error error) noexcept {
    if(error.code==ErrorCode::Overflow){(void)batch(true);if(state_.failure)stopped();}
    else {failure(error);stopped();}
  }
  Result<void> batch(bool overflow) noexcept {
    if(state_.failure||state_.phase==Phase::Stopped)return fail(ErrorCode::Stopping,"Directory watch is terminal");
    // generation starts at one; these counters cannot wrap before exhaustion.
    ++state_.completed_batches;if(overflow)++state_.overflow_batches;
    if(state_.dirty)++state_.coalesced_batches;state_.dirty=true;
    if(state_.observation.generation==limit_){
      failure(fail(ErrorCode::Exhausted,"Directory observation identities exhausted"));return *state_.failure;
    }
    ++state_.observation.generation;return {};
  }
  Result<void> acknowledge(Observation observation) noexcept {
    if(state_.phase!=Phase::Watching||state_.failure)return fail(ErrorCode::Stopping,"Directory watch is terminal");
    if(observation!=state_.observation)return fail(ErrorCode::StaleHandle,"Obsolete directory observation");
    state_.dirty=false;return {};
  }
};
} // namespace race::platform::directory_watch::detail
