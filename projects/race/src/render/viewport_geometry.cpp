#include "race/render/viewport_geometry.hpp"
#include <algorithm>
#include <cmath>

namespace race::render {
ViewportGeometry::ViewportGeometry(std::vector<GeometryVertex> vertices,
                                   std::vector<std::uint32_t> indices, bool has_uv)
    : vertices_(std::move(vertices)), indices_(std::move(indices)), has_uv_(has_uv) {}
Result<std::shared_ptr<const ViewportGeometry>> ViewportGeometry::create(
    std::span<const GeometryVertex> vertices, std::span<const std::uint32_t> indices, bool has_uv) {
  if (vertices.size() > viewport_mesh_max_vertices || indices.size() > viewport_mesh_max_indices)
    return fail(ErrorCode::Exhausted, "viewport mesh packet capacity exceeded");
  if (vertices.size() < 3 || indices.size() < 3 || indices.size() % 3)
    return fail(ErrorCode::InvalidInput, "viewport mesh requires indexed triangles");
  for (const auto &vertex : vertices) {
    for (const auto uv : vertex.uv)
      if (!std::isfinite(uv) || std::abs(uv) > 1e12)
        return fail(ErrorCode::InvalidInput, "viewport mesh UV exceeds finite bounds");
    for (std::size_t i = 0; i < 3; ++i)
      if (!std::isfinite(vertex.position[i]) || std::abs(vertex.position[i]) > 1e12 ||
          !std::isfinite(vertex.normal[i]))
        return fail(ErrorCode::InvalidInput, "viewport mesh vertex exceeds finite bounds");
    if (std::abs(std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]) - 1.0) > 1e-6)
      return fail(ErrorCode::InvalidInput, "viewport mesh normal must be unit length");
  }
  for (const auto i : indices)
    if (i >= vertices.size()) return fail(ErrorCode::InvalidInput, "viewport mesh index out of range");
  for (std::size_t i = 0; i < indices.size(); i += 3) {
    const auto &a = vertices[indices[i]].position, &b = vertices[indices[i + 1]].position,
               &c = vertices[indices[i + 2]].position;
    std::array<double, 3> u{}, v{};
    double us{}, vs{};
    for (std::size_t axis = 0; axis < 3; ++axis) {
      u[axis] = b[axis] - a[axis]; v[axis] = c[axis] - a[axis];
      us = std::max(us, std::abs(u[axis])); vs = std::max(vs, std::abs(v[axis]));
    }
    if (!us || !vs) return fail(ErrorCode::InvalidInput, "viewport mesh degenerate triangle");
    for (std::size_t axis = 0; axis < 3; ++axis) { u[axis] /= us; v[axis] /= vs; }
    if (!(std::hypot(u[1]*v[2]-u[2]*v[1], u[2]*v[0]-u[0]*v[2], u[0]*v[1]-u[1]*v[0]) > 0))
      return fail(ErrorCode::InvalidInput, "viewport mesh degenerate triangle");
  }
  try {
    return std::shared_ptr<const ViewportGeometry>(new ViewportGeometry(
        {vertices.begin(), vertices.end()}, {indices.begin(), indices.end()}, has_uv));
  } catch (const std::bad_alloc &) { return fail(ErrorCode::Exhausted, "viewport mesh packet allocation"); }
}
} // namespace race::render
