#pragma once
#include "race/foundation/result.hpp"
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace race::render {
inline constexpr std::size_t viewport_mesh_max_vertices = 65536;
inline constexpr std::size_t viewport_mesh_max_indices = 196608;
// Frame totals count each instance, even when packets share geometry.
inline constexpr std::size_t viewport_mesh_frame_vertices = 262144;
inline constexpr std::size_t viewport_mesh_frame_indices = 196608;
struct GeometryVertex {
  std::array<double, 3> position{}, normal{};
  std::array<double, 2> uv{}; // Top-left texture origin; finite, bounded to +/-1e12.
  friend bool operator==(const GeometryVertex &, const GeometryVertex &) = default;
};
// Factory copies validated inputs. No mutable alias or domain-owned pointer is
// exposed; RenderFrame copies retain this packet independently of Assets/Scene.
class ViewportGeometry {
  std::vector<GeometryVertex> vertices_;
  std::vector<std::uint32_t> indices_;
  bool has_uv_{};
  ViewportGeometry(std::vector<GeometryVertex>, std::vector<std::uint32_t>, bool);
public:
  static Result<std::shared_ptr<const ViewportGeometry>> create(
      std::span<const GeometryVertex>, std::span<const std::uint32_t>, bool has_uv = false);
  ViewportGeometry(const ViewportGeometry &) = delete;
  std::span<const GeometryVertex> vertices() const noexcept { return vertices_; }
  std::span<const std::uint32_t> indices() const noexcept { return indices_; }
  bool has_uv() const noexcept { return has_uv_; }
};
} // namespace race::render
