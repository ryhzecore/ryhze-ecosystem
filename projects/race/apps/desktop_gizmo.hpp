#pragma once
#include "race/render/viewport.hpp"
#include "race/scene/math.hpp"
#include <array>
#include <optional>
#include <vector>

namespace race::desktop::gizmo {
enum class Mode { Move,Rotate,Scale };
struct Point { double x{},y{}; };
inline constexpr std::size_t ring_segments=64;
inline constexpr double handle_pixels=85;
struct Axis {
  bool visible{};
  bool end_on{},edge_on{};
  Point start,end;
  std::array<Point,ring_segments+1> ring{};
  scene::Vec3 direction,plane_u,plane_v;
};
// Owned, allocation-free projection snapshot. Freeze the mouse-down Layout for
// the complete drag; recomputing it from previews changes the drag's reference.
// All screen coordinates are logical pixels, with top-left origin and Y down.
struct Layout {
  Mode mode{Mode::Move};
  int active_axis{-1};
  double width{},height{},world_length{};
  Point pivot;
  std::array<Axis,3> axes{};
  render::ViewportCamera camera;
  scene::Vec3 pivot_relative,forward,right,up;
  double tangent{},depth{};
};
// Move/Scale directions are normalized independently. Rotate preserves the
// relative lengths/shear of the other two world axes as its local axis plane;
// its Axis::direction is the plane's oriented normal (including reflections).
// Head-on Move/Scale axes use a small cap at the actual endpoint (the pivot
// when exactly end-on). Every stem/ring
// retains its actual projection; an edge-on ring may project to a line.
// Truly degenerate axes/planes are omitted; nonfinite input is rejected. A pivot
// outside the viewport or camera depth range yields an empty valid Layout.
Result<Layout> layout(scene::Vec3 pivot,const std::array<scene::Vec3,3>& world_axes,
    const render::ViewportCamera&,double width,double height,Mode,int active_axis=-1) noexcept;
std::optional<int> hit_test(const Layout&,Point,double tolerance=9) noexcept;
// Unit screen direction for positive interaction at the frozen start point.
// End-on caps use upward motion. Rotation follows the nearest true ring segment;
// coincident branches choose the nearer camera depth, then segment order.
// Recomputing from the same immutable Layout/start produces the same direction.
std::optional<Point> drag_direction(const Layout&,Point start,int axis) noexcept;
// Move: world metres along the normalized axis, including perspective depth.
// An end-on cap uses world_length/85 metres per upward pixel, clamped to depth.
// Scale: positive multiplier in [1/1024,1024]; one handle-length doubles/halves.
// Rotate: signed local-plane radians in [-pi,pi]. Conditioned views use exact
// ray-plane coordinates. Edge-on views use the frozen start-segment tangent at
// 85 pixels/radian, wrapped to [-pi,pi] for caller-side multi-turn unwrapping.
// start==current returns 0 or 1 (Scale).
// No Scene state is mutated; cancellation means discarding the returned preview.
std::optional<double> drag_amount(const Layout&,Point start,Point current,int axis) noexcept;
// At most three opaque NDC triangle draws and 1152 vertices. The active axis is
// drawn last. Rebuilds owned output, reusing outer and surviving inner capacities.
// With a stable visible-axis count, no allocation follows maximum-mode warmup.
// Hidden axes are erased; failure clears all output, so neither stale nor partial
// draws remain. Those transitions may require bounded capacity warmup again.
Result<void> overlays(const Layout&,std::vector<render::OverlayDraw>& output);
// Convenience wrapper for callers without retained output storage.
Result<std::vector<render::OverlayDraw>> overlays(const Layout&);
} // namespace race::desktop::gizmo
