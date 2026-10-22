#include "race/scene/math.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace race::scene {
bool finite(Vec3 v) noexcept {
  return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}
Result<void> validate(LocalTransform t) noexcept {
  if (!finite(t.translation) || !finite(t.scale) || !std::isfinite(t.rotation.x) ||
      !std::isfinite(t.rotation.y) || !std::isfinite(t.rotation.z) || !std::isfinite(t.rotation.w))
    return fail(ErrorCode::InvalidInput, "non-finite transform");
  auto q = t.rotation;
  double n = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
  if (!std::isfinite(n) || std::abs(n - 1) > 1e-10)
    return fail(ErrorCode::InvalidInput, "rotation must be a unit quaternion");
  return {};
}
Result<Quat> axis_angle(Vec3 axis, double radians) noexcept {
  double length = std::hypot(axis.x, axis.y, axis.z);
  if (!finite(axis) || !std::isfinite(radians) || !std::isfinite(length) || length == 0)
    return fail(ErrorCode::InvalidInput, "invalid rotation axis");
  double s = std::sin(radians * .5);
  Quat q{(axis.x / length) * s, (axis.y / length) * s, (axis.z / length) * s,
         std::cos(radians * .5)};
  auto v = validate({{}, q, {1, 1, 1}});
  if (!v)
    return v.error();
  return q;
}
Result<Affine> affine(LocalTransform t) noexcept {
  auto valid = validate(t);
  if (!valid)
    return valid.error();
  auto q = t.rotation;
  auto s = t.scale;
  Affine a{{(1 - 2 * (q.y * q.y + q.z * q.z)) * s.x, 2 * (q.x * q.y - q.z * q.w) * s.y,
            2 * (q.x * q.z + q.y * q.w) * s.z, t.translation.x, 2 * (q.x * q.y + q.z * q.w) * s.x,
            (1 - 2 * (q.x * q.x + q.z * q.z)) * s.y, 2 * (q.y * q.z - q.x * q.w) * s.z,
            t.translation.y, 2 * (q.x * q.z - q.y * q.w) * s.x, 2 * (q.y * q.z + q.x * q.w) * s.y,
            (1 - 2 * (q.x * q.x + q.y * q.y)) * s.z, t.translation.z}};
  for (double v : a.m)
    if (!std::isfinite(v))
      return fail(ErrorCode::Overflow, "affine overflow");
  return a;
}
Result<Affine> compose(const Affine &a, const Affine &b) noexcept {
  Affine c;
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned col = 0; col < 4; ++col) {
      double v = col == 3 ? a.m[r * 4 + 3] : 0;
      for (unsigned k = 0; k < 3; ++k)
        v += a.m[r * 4 + k] * b.m[k * 4 + col];
      if (!std::isfinite(v))
        return fail(ErrorCode::Overflow, "derived transform overflow");
      c.m[r * 4 + col] = v;
    }
  return c;
}
Result<Affine> inverse(const Affine &a) noexcept {
  // Scale first so invertibility is not decided by an absolute world-unit epsilon.
  double scale = 0;
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c)
      scale = std::max(scale, std::abs(a.m[r * 4 + c]));
  if (!std::isfinite(scale) || scale == 0)
    return fail(ErrorCode::InvalidInput, "singular parent transform");
  double b[9];
  for (unsigned r = 0; r < 3; ++r)
    for (unsigned c = 0; c < 3; ++c)
      b[r * 3 + c] = a.m[r * 4 + c] / scale;
  double det = b[0] * (b[4] * b[8] - b[5] * b[7]) - b[1] * (b[3] * b[8] - b[5] * b[6]) +
               b[2] * (b[3] * b[7] - b[4] * b[6]);
  if (!std::isfinite(det) || std::abs(det) <= 64 * std::numeric_limits<double>::epsilon())
    return fail(ErrorCode::InvalidInput, "singular or ill-conditioned parent transform");
  double co[9]{b[4] * b[8] - b[5] * b[7], b[2] * b[7] - b[1] * b[8], b[1] * b[5] - b[2] * b[4],
               b[5] * b[6] - b[3] * b[8], b[0] * b[8] - b[2] * b[6], b[2] * b[3] - b[0] * b[5],
               b[3] * b[7] - b[4] * b[6], b[1] * b[6] - b[0] * b[7], b[0] * b[4] - b[1] * b[3]};
  Affine out;
  for (unsigned r = 0; r < 3; ++r) {
    for (unsigned c = 0; c < 3; ++c)
      out.m[r * 4 + c] = (co[r * 3 + c] / det) / scale;
    out.m[r * 4 + 3] =
        -(out.m[r * 4] * a.m[3] + out.m[r * 4 + 1] * a.m[7] + out.m[r * 4 + 2] * a.m[11]);
  }
  for (double v : out.m)
    if (!std::isfinite(v))
      return fail(ErrorCode::Overflow, "inverse transform overflow");
  return out;
}
bool near(const Affine &a, const Affine &b, double tolerance) noexcept {
  for (unsigned i = 0; i < 12; ++i)
    if (!std::isfinite(a.m[i]) || !std::isfinite(b.m[i]) ||
        std::abs(a.m[i] - b.m[i]) > tolerance * std::max({1., std::abs(a.m[i]), std::abs(b.m[i])}))
      return false;
  return true;
}
Result<LocalTransform> decompose(const Affine &a) noexcept {
  for (double v : a.m)
    if (!std::isfinite(v))
      return fail(ErrorCode::InvalidInput, "non-finite affine");
  LocalTransform t;
  t.translation = {a.m[3], a.m[7], a.m[11]};
  double s[3]{std::hypot(a.m[0], a.m[4], a.m[8]), std::hypot(a.m[1], a.m[5], a.m[9]),
              std::hypot(a.m[2], a.m[6], a.m[10])};
  // Zero scale is valid. Complete an orthonormal basis only for missing
  // columns; this preserves the matrix without requiring a unique rotation.
  double m[9]{};
  unsigned count = 0;
  for (unsigned c = 0; c < 3; ++c)
    if (s[c] != 0) {
      ++count;
      for (unsigned r = 0; r < 3; ++r)
        m[r * 3 + c] = a.m[r * 4 + c] / s[c];
    }
  auto cross_column = [&](unsigned dst, unsigned u, unsigned v) {
    m[dst] = m[3 + u] * m[6 + v] - m[6 + u] * m[3 + v];
    m[3 + dst] = m[6 + u] * m[v] - m[u] * m[6 + v];
    m[6 + dst] = m[u] * m[3 + v] - m[3 + u] * m[v];
  };
  if (!count) {
    m[0] = m[4] = m[8] = 1;
  } else if (count == 1) {
    unsigned c = s[0] != 0 ? 0 : s[1] != 0 ? 1 : 2;
    unsigned d = (c + 1) % 3;
    unsigned axis = 0;
    for (unsigned r = 1; r < 3; ++r)
      if (std::abs(m[r * 3 + c]) < std::abs(m[axis * 3 + c]))
        axis = r;
    for (unsigned r = 0; r < 3; ++r)
      m[r * 3 + d] = (r == axis ? 1. : 0.) - m[r * 3 + c] * m[axis * 3 + c];
    double n = std::hypot(m[d], m[3 + d], m[6 + d]);
    for (unsigned r = 0; r < 3; ++r)
      m[r * 3 + d] /= n;
    cross_column((c + 2) % 3, c, d);
  } else if (count == 2) {
    unsigned c = s[0] == 0 ? 0 : s[1] == 0 ? 1 : 2;
    cross_column(c, (c + 1) % 3, (c + 2) % 3);
  }
  double det = m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
               m[2] * (m[3] * m[7] - m[4] * m[6]);
  if (det < 0) {
    s[0] = -s[0];
    for (unsigned r = 0; r < 3; ++r)
      m[r * 3] = -m[r * 3];
  }
  for (unsigned c = 0; c < 3; ++c)
    for (unsigned d = c + 1; d < 3; ++d)
      if (std::abs(m[c] * m[d] + m[3 + c] * m[3 + d] + m[6 + c] * m[6 + d]) > 1e-10)
        return fail(ErrorCode::Unsupported, "local shear is not supported");
  Quat q;
  double trace = m[0] + m[4] + m[8];
  if (trace > 0) {
    double z = 2 * std::sqrt(trace + 1);
    q = {(m[7] - m[5]) / z, (m[2] - m[6]) / z, (m[3] - m[1]) / z, z / 4};
  } else if (m[0] > m[4] && m[0] > m[8]) {
    double z = 2 * std::sqrt(1 + m[0] - m[4] - m[8]);
    q = {z / 4, (m[1] + m[3]) / z, (m[2] + m[6]) / z, (m[7] - m[5]) / z};
  } else if (m[4] > m[8]) {
    double z = 2 * std::sqrt(1 + m[4] - m[0] - m[8]);
    q = {(m[1] + m[3]) / z, z / 4, (m[5] + m[7]) / z, (m[2] - m[6]) / z};
  } else {
    double z = 2 * std::sqrt(1 + m[8] - m[0] - m[4]);
    q = {(m[2] + m[6]) / z, (m[5] + m[7]) / z, z / 4, (m[3] - m[1]) / z};
  }
  t.rotation = q;
  t.scale = {s[0], s[1], s[2]};
  auto rebuilt = affine(t);
  if (!rebuilt || !near(a, rebuilt.value()))
    return fail(ErrorCode::Unsupported, "affine cannot be represented as local TRS");
  return t;
}
Result<RelativePosition> relative_position(Vec3 v, LocalOrigin o) noexcept {
  Vec3 r{v.x - o.position.x, v.y - o.position.y, v.z - o.position.z};
  if (!o.revision || !finite(v) || !finite(o.position) || !finite(r))
    return fail(ErrorCode::InvalidInput, "invalid origin conversion");
  return RelativePosition{r, o.revision};
}
Result<Vec3> absolute_position(RelativePosition r, LocalOrigin o) noexcept {
  if (r.origin_revision != o.revision || !o.revision)
    return fail(ErrorCode::StaleHandle, "stale local origin");
  Vec3 v{r.metres.x + o.position.x, r.metres.y + o.position.y, r.metres.z + o.position.z};
  if (!finite(r.metres) || !finite(o.position) || !finite(v))
    return fail(ErrorCode::InvalidInput, "invalid origin conversion");
  return v;
}
} // namespace race::scene
