#pragma once
#include "race/foundation/identity.hpp"
#include <array>
#include <memory>
#include <span>
#include <variant>

namespace race::vfx {
// Cosmetic CPU simulation; metres, seconds, right-handed Y-up. Composition
// copies authoritative events here. This module never writes back to their owner.
struct Vec3 {
  double x{}, y{}, z{};
  friend bool operator==(const Vec3&, const Vec3&) = default;
};
struct Orientation {
  double x{}, y{}, z{}, w{1};
  friend bool operator==(const Orientation&, const Orientation&) = default;
};
struct EmitterTag;
using EmitterHandle = Handle<EmitterTag>;
enum class OverloadPolicy : std::uint8_t { DropNewest, CoalesceMatching };
struct Config {
  PersistentId world{1, 1};
  std::size_t max_emitters{32}, max_particles{4096}, max_events{128};
  std::uint32_t max_event_particles{4096}, max_spawn_attempts_per_tick{4096};
  double fixed_step_seconds{1.0 / 60.0};
  Vec3 origin{};
  std::uint64_t origin_revision{1}, first_tick{1};
  OverloadPolicy overload{OverloadPolicy::DropNewest};
};
struct EmitterDesc {
  PersistentId source{};
  std::uint64_t revision{1}, seed{1};
  Vec3 position{}, velocity{}, acceleration{0, -9.81, 0};
  // Independent uniform components in emitter-local axes, rotated by event
  // orientation. No rejection sampling or process-global random generator.
  Vec3 velocity_spread{1, 1, 1};
  std::uint32_t particles_per_tick{}, lifetime_ticks{60};
  double radius{0.02};
  std::array<float, 4> color{1, 0.6f, 0.1f, 1};
};
struct GameplayBurst {
  Vec3 velocity{};
  friend bool operator==(const GameplayBurst&, const GameplayBurst&) = default;
};
struct Impact {
  Vec3 impulse_velocity{}; // Already converted to m/s by producer; no mass ownership.
  PersistentId material{};
  friend bool operator==(const Impact&, const Impact&) = default;
};
struct Splash {
  Vec3 velocity{};
  PersistentId fluid{};
  friend bool operator==(const Splash&, const Splash&) = default;
};
struct Weather {
  Vec3 wind_velocity{};
  friend bool operator==(const Weather&, const Weather&) = default;
};
using EventPayload = std::variant<GameplayBurst, Impact, Splash, Weather>;
struct Event {
  EmitterHandle emitter{};
  PersistentId world{}, source{};
  std::uint64_t tick{}, serial{}, effect_revision{1}, origin_revision{1};
  Vec3 position{};
  Orientation orientation{};
  std::uint32_t particles{1};
  EventPayload payload{GameplayBurst{}};
};
enum class Admission : std::uint8_t { Queued, Coalesced, Dropped };
struct SubmitResult {
  Admission admission{};
  std::uint32_t accepted_particles{}, dropped_particles{};
};
struct Particle {
  std::uint64_t id{}, birth_tick{};
  PersistentId source{};
  std::uint64_t effect_revision{}, origin_revision{};
  Vec3 position{}, velocity{};
  std::uint32_t age_ticks{}, lifetime_ticks{};
  double radius{};
  std::array<float, 4> color{};
};
struct Bounds { bool empty{true}; Vec3 minimum{}, maximum{}; };
struct StepReport {
  std::uint64_t tick{}, spawned{}, expired{}, dropped_particles{};
  std::size_t active_particles{}, processed_events{};
};
struct Statistics {
  std::uint64_t queued_events{}, coalesced_events{}, dropped_events{};
  std::uint64_t dropped_particles{}, spawned_particles{}, expired_particles{};
  std::uint64_t cancelled_events{}, rebases{};
  bool counter_saturated{};
};
class ParticleSystem {
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit ParticleSystem(std::unique_ptr<Impl>);
public:
  ~ParticleSystem();
  ParticleSystem(const ParticleSystem&) = delete;
  ParticleSystem& operator=(const ParticleSystem&) = delete;
  static Result<std::unique_ptr<ParticleSystem>> create(Config);
  Result<EmitterHandle> create_emitter(EmitterDesc);
  // Cancels queued events; already published particles retain their lifetime.
  Result<void> remove_emitter(EmitterHandle);
  // Next tick only. Serials must strictly increase per emitter, including dropped
  // admissions. Failed validation does not consume a serial. Replays are rejected.
  Result<SubmitResult> submit(Event);
  // Exactly the next tick. Existing particles advance/expire, then sources emit.
  // Birth age=0; lifetime N is visible for N tick snapshots. No variable-dt path.
  Result<StepReport> step(std::uint64_t expected_tick);
  // Atomic translation of owned copies only, including queued event coordinates.
  // Velocities, IDs, seeds, ages, source revisions and gameplay authority persist.
  Result<void> rebase(Vec3 absolute_origin, std::uint64_t new_revision);
  // Single owner-thread API. Span remains valid only until the next mutation.
  std::span<const Particle> particles() const noexcept;
  Bounds bounds() const noexcept;
  Statistics statistics() const noexcept;
  std::uint64_t next_tick() const noexcept;
  std::uint64_t origin_revision() const noexcept;
  Vec3 origin() const noexcept;
};
} // namespace race::vfx
