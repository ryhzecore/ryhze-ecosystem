#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <cstdint>
namespace race::scene {
// Metres, right-handed coordinates, Y up; angular helpers take radians.
struct Vec3 {
  double x{}, y{}, z{};
  friend bool operator==(const Vec3 &, const Vec3 &) = default;
};
struct Quat {
  double x{}, y{}, z{}, w{1};
  friend bool operator==(const Quat &, const Quat &) = default;
};
struct LocalTransform {
  Vec3 translation;
  Quat rotation;
  Vec3 scale{1, 1, 1};
  friend bool operator==(const LocalTransform &, const LocalTransform &) = default;
};
struct Affine {
  std::array<double, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
  friend bool operator==(const Affine &, const Affine &) = default;
};
bool finite(Vec3) noexcept;
Result<void> validate(LocalTransform) noexcept;
Result<Quat> axis_angle(Vec3 axis, double radians) noexcept;
Result<Affine> affine(LocalTransform) noexcept;
Result<Affine> compose(const Affine &, const Affine &) noexcept;
Result<Affine> inverse(const Affine &) noexcept;
Result<LocalTransform> decompose(const Affine &) noexcept;
bool near(const Affine &, const Affine &, double tolerance = 1e-10) noexcept;
struct LocalOrigin {
  Vec3 position;
  std::uint64_t revision{1};
};
struct RelativePosition {
  Vec3 metres;
  std::uint64_t origin_revision{};
};
Result<RelativePosition> relative_position(Vec3 absolute, LocalOrigin) noexcept;
Result<Vec3> absolute_position(RelativePosition, LocalOrigin) noexcept;
} // namespace race::scene
