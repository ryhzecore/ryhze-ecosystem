#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace race::physics {
// Approved D3 race-rigid-primitives-1 boundary. These are admission limits, not
// an allocator proof or permission to initialize a solver in an application.
struct PrimitiveProfile {
  static constexpr std::uint32_t bodies=256, moving_bodies=128, shapes=256;
  static constexpr std::uint32_t body_pairs=32768, contacts=32768, in_flight_pairs=1024, jobs=2048;
  static constexpr std::uint32_t commands=256, events=4096, queries=64, hits_per_query=256;
  static constexpr std::size_t service_bytes=64U*1024U*1024U, scratch_bytes=16U*1024U*1024U;
  static constexpr double tick_seconds=1.0/60.0, max_displacement_m=0.025;
  // The solver receives one float conversion; admission must bound that same dt.
  static constexpr float solver_tick_seconds=static_cast<float>(tick_seconds);
  static constexpr double local_extent_m=1000, box_convex_radius_m=0.005;
};
enum class PrimitiveKind : std::uint8_t { Sphere=1, Box=2 };
enum class MotionType : std::uint8_t { Static=1, Kinematic=2, Dynamic=3 };
using Triple=std::array<double,3>;
struct Primitive {
  PrimitiveKind kind{PrimitiveKind::Box};
  // Full dimensions in metres. A sphere stores its diameter on all three axes.
  Triple dimensions_m{1,1,1};
};
struct MassProperties {
  double mass_kg{};
  // Diagonal inertia about the primitive's centre, in kg*m^2; principal axes
  // match the box axes. COM remains the shape centre in this first profile.
  Triple inertia_kg_m2{};
};
struct MotionBound {
  Triple velocity_m_s{}, angular_velocity_rad_s{};
  // Conservative acceleration bounds include gravity, forces, torques and any
  // admitted impulse converted to the initial velocity. Damping is not credited.
  Triple acceleration_m_s2{}, angular_acceleration_rad_s2{};
};
struct CapacityUse {
  std::uint32_t bodies{}, moving_bodies{}, shapes{}, commands{}, events{}, queries{}, hits{};
};
Result<Primitive> bake_primitive(Primitive source, Triple positive_scale, MotionType) noexcept;
Result<MassProperties> primitive_mass(Primitive, double mass_kg) noexcept;
Result<double> admit_motion(Primitive, MotionBound) noexcept;
Result<void> admit_capacity(CapacityUse) noexcept;
} // namespace race::physics
