#include "editor_viewport.hpp"

namespace race_editor {
Viewport::Viewport(desktop::Document &doc, const std::shared_ptr<const desktop::MeshSet> &meshes,
                   std::filesystem::path shaders)
    : document_(doc), meshes_(meshes), shaders_(std::move(shaders)) {
  setSurfaceType(QSurface::RasterSurface);
}

Viewport::~Viewport() { renderer_.reset(); }

render::WindowsViewport *Viewport::renderer() const { return renderer_.get(); }

bool Viewport::previewing() const { return preview_.has_value(); }

std::array<double, 6> Viewport::camera_pose() const {
  return {yaw_, pitch_, distance_, target_.x, target_.y, target_.z};
}

void Viewport::frame_selected() {
  cancel();
  auto rows = document_.objects();
  if (!rows)
    return;
  for (auto &row : rows.value())
    if (row.id == selected) {
      auto bounds = desktop::local_bounds(row, meshes_.get());
      if (!bounds) {
        report(bounds.error());
        return;
      }
      const auto &b = bounds.value();
      Vec low{1e300, 1e300, 1e300}, high{-1e300, -1e300, -1e300};
      const auto &m = row.world.m;
      for (int corner = 0; corner < 8; ++corner) {
        Vec p{corner & 1 ? b.maximum[0] : b.minimum[0], corner & 2 ? b.maximum[1] : b.minimum[1],
              corner & 4 ? b.maximum[2] : b.minimum[2]};
        Vec w{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
              m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
        low = {std::min(low.x, w.x), std::min(low.y, w.y), std::min(low.z, w.z)};
        high = {std::max(high.x, w.x), std::max(high.y, w.y), std::max(high.z, w.z)};
      }
      target_ = mul(add(low, high), .5);
      distance_ = std::clamp(std::sqrt(dot(sub(high, low), sub(high, low))) * 2, 2.0, 10000.0);
    }
}

Result<scene::Affine> Viewport::gizmo_parent(const desktop::ObjectRow &row) const {
  const std::array scale{row.local.scale.x, row.local.scale.y, row.local.scale.z};
  scene::LocalTransform orientation;
  orientation.rotation = row.local.rotation;
  auto rotation = scene::affine(orientation);
  if (!rotation)
    return rotation.error();
  scene::Affine parent;
  const std::array translation{row.local.translation.x, row.local.translation.y, row.local.translation.z};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      double value = 0;
      for (int k = 0; k < 3; ++k) {
        if (scale[k] == 0)
          return fail(ErrorCode::InvalidInput, "Collapsed transform has no invertible gizmo basis");
        value += (row.world.m[r * 4 + k] / scale[k]) * rotation.value().m[c * 4 + k];
      }
      parent.m[r * 4 + c] = value;
    }
    parent.m[r * 4 + 3] = row.world.m[r * 4 + 3];
    for (int c = 0; c < 3; ++c)
      parent.m[r * 4 + 3] -= parent.m[r * 4 + c] * translation[c];
  }
  for (const auto value : parent.m)
    if (!std::isfinite(value))
      return fail(ErrorCode::InvalidInput, "Gizmo parent basis overflow");
  return parent;
}

Result<desktop::gizmo::Layout> Viewport::make_gizmo(const desktop::ObjectRow &row) {
  const auto parent = gizmo_parent(row);
  if (!parent)
    return parent.error();
  scene::LocalTransform orientation;
  orientation.rotation = row.local.rotation;
  const auto rotated = scene::affine(orientation);
  if (!rotated)
    return rotated.error();
  const auto basis = scene::compose(parent.value(), rotated.value());
  if (!basis)
    return basis.error();
  std::array<Vec, 3> axes;
  for (int a = 0; a < 3; ++a) {
    const auto &m = basis.value().m;
    axes[a] = {m[a], m[4 + a], m[8 + a]};
  }
  const auto e = eye();
  render::ViewportCamera camera;
  camera.eye = {e.x, e.y, e.z};
  camera.target = {target_.x, target_.y, target_.z};
  camera.far_metres = std::max(2000.0, distance_ * 4);
  return desktop::gizmo::layout({row.world.m[3], row.world.m[7], row.world.m[11]}, axes, camera, width(),
                                height(), static_cast<desktop::gizmo::Mode>(tool), axis);
}

void Viewport::begin_gizmo(const desktop::ObjectRow &row, const desktop::gizmo::Layout &handles, int chosen) {
  before_ = row;
  rotation_previous_ = rotation_total_ = 0;
  drag_generation_ = document_.generation();
  drag_layout_ = handles;
  axis = chosen;
  if (on_axis)
    on_axis(axis);
  scene::LocalTransform orientation;
  orientation.rotation = row.local.rotation;
  const auto rotated = scene::affine(orientation);
  if (!rotated) {
    cancel();
    return;
  }
  const auto parent = gizmo_parent(row);
  if (!parent) {
    cancel();
    return;
  }
  const auto basis = scene::compose(parent.value(), rotated.value());
  if (!basis) {
    cancel();
    return;
  }
  for (int a = 0; a < 3; ++a) {
    const auto &r = rotated.value().m;
    move_axes_[a] = {r[a], r[4 + a], r[8 + a]};
    const auto &m = basis.value().m;
    axis_lengths_[a] = std::hypot(m[a], m[4 + a], m[8 + a]);
  }
  dragging_ = true;
  setMouseGrabEnabled(true);
}

Vec Viewport::eye() const {
  return add(target_, {distance_ * std::cos(pitch_) * std::sin(yaw_), distance_ * std::sin(pitch_),
                       distance_ * std::cos(pitch_) * std::cos(yaw_)});
}

void Viewport::frame_all() {
  auto rows = document_.objects();
  if (!rows || rows.value().empty())
    return;
  Vec low{1e300, 1e300, 1e300}, high{-1e300, -1e300, -1e300};
  bool found = false;
  for (const auto &row : rows.value())
    if (row.visible) {
      auto bounds = desktop::local_bounds(row, meshes_.get());
      if (!bounds) {
        report(bounds.error());
        return;
      }
      const auto &b = bounds.value();
      for (int corner = 0; corner < 8; ++corner) {
        Vec p{corner & 1 ? b.maximum[0] : b.minimum[0], corner & 2 ? b.maximum[1] : b.minimum[1],
              corner & 4 ? b.maximum[2] : b.minimum[2]};
        auto &m = row.world.m;
        Vec w{m[0] * p.x + m[1] * p.y + m[2] * p.z + m[3], m[4] * p.x + m[5] * p.y + m[6] * p.z + m[7],
              m[8] * p.x + m[9] * p.y + m[10] * p.z + m[11]};
        low = {std::min(low.x, w.x), std::min(low.y, w.y), std::min(low.z, w.z)};
        high = {std::max(high.x, w.x), std::max(high.y, w.y), std::max(high.z, w.z)};
        found = true;
      }
    }
  if (found) {
    target_ = mul(add(low, high), .5);
    distance_ = std::clamp(std::sqrt(dot(sub(high, low), sub(high, low))) * 1.8, 3.0, 10000.0);
  }
}

void Viewport::report(Result<void> r) {
  if (!r && on_error)
    on_error(QString::fromUtf8(r.error().message));
}

Result<void> Viewport::recover_failed_operation(Result<void> failed) {
  if (failed || !renderer_ || renderer_->state() != render::ViewportState::Removed)
    return failed;
  if (!automatic_recovery_enabled)
    return fail(ErrorCode::FailedPrerequisite, "Profile interrupted by device removal; samples are invalid");
  cancel();
  auto recovered = renderer_->recover_removed(reinterpret_cast<HWND>(winId()));
  if (on_recovery_boundary)
    on_recovery_boundary(); // Pause duration must not enter Play catch-up.
  if (!recovered)
    renderer_blocked_ = true;
  return recovered;
}

bool Viewport::render() {
  extraction_ms = 0;
  submission_ms = 0;
  if (renderer_blocked_ || !surface_available_ || !isExposed() || width() <= 0 || height() <= 0)
    return false;
  if (!prepare_render_surface())
    return false;
  auto extraction_start = Clock::now();
  // One read snapshot supplies rows and lighting settings to the interactive/preview path.
  auto extraction = document_.extract(document_.playing());
  if (!extraction) {
    if (on_error)
      on_error(extraction.error().message);
    return false;
  }
  Result<std::vector<desktop::ObjectRow>> rows = std::move(extraction.value().rows);
  if (preview_) {
    auto candidate = desktop::preview_objects(std::move(rows).value(), *preview_);
    if (!candidate) {
      cancel();
      report(candidate.error());
      return false;
    }
    rows = std::move(candidate);
  }
  render::RenderFrame frame;
  if (auto lit = desktop::apply_lighting(frame, extraction.value().lighting); !lit) {
    report(lit.error());
    return false;
  }
  auto e = eye();
  frame.camera.eye = {e.x, e.y, e.z};
  frame.camera.target = {target_.x, target_.y, target_.z};
  frame.camera.far_metres = std::max(2000.0, distance_ * 4);
  frame.scene_generation = document_.generation(document_.playing());
  frame.origin_revision = document_.origin(document_.playing()).revision;
  frame.simulation_tick = simulation_tick;
  displayed_gizmo_.reset();
  for (auto &row : rows.value())
    if (row.visible) {
      auto draw = desktop::mesh_draw(row, meshes_.get(), row.id == selected);
      if (!draw) {
        report(draw.error());
        return false;
      }
      frame.draws.push_back(std::move(draw).value());
      if (row.id == selected && tool >= 0 && !document_.playing()) {
        auto handles = make_gizmo(row);
        if (!handles)
          continue;
        displayed_gizmo_ = handles.value();
        auto overlay = desktop::gizmo::overlays(handles.value(), overlay_scratch_);
        if (!overlay) {
          report(overlay.error());
          return false;
        }
      }
    }
  if (displayed_gizmo_)
    frame.overlays = std::move(overlay_scratch_);
  extraction_ms = milliseconds(Clock::now() - extraction_start);
  auto submission_start = Clock::now();
  auto drawn = renderer_->draw(frame);
  if (!drawn && renderer_->state() == render::ViewportState::Removed) {
    drawn = recover_failed_operation(drawn);
    if (drawn)
      drawn = renderer_->draw(frame); // Resubmit the same owned snapshot; no second Play tick/extraction.
    if (!drawn && renderer_->state() == render::ViewportState::Removed)
      renderer_blocked_ = true;
  }
  if (displayed_gizmo_)
    overlay_scratch_ = std::move(frame.overlays);
  report(drawn);
  submission_ms = milliseconds(Clock::now() - submission_start);
  return bool(drawn);
}

bool Viewport::prepare_render_surface() {
  if (!renderer_) {
    auto r = render::WindowsViewport::create(reinterpret_cast<HWND>(winId()), shaders_);
    if (!r) {
      renderer_blocked_ = true;
      if (on_error)
        on_error(QString::fromUtf8(r.error().message));
      return false;
    }
    renderer_ = std::move(r).value();
  }
  if (renderer_->state() == render::ViewportState::SurfaceRetired) {
    auto restored = renderer_->recreate(reinterpret_cast<HWND>(winId()));
    if (!restored) {
      renderer_blocked_ = true;
      report(restored);
      return false;
    }
    if (on_recovery_boundary)
      on_recovery_boundary();
  }
  const auto pixel_width = static_cast<std::uint32_t>(width() * devicePixelRatio());
  const auto pixel_height = static_cast<std::uint32_t>(height() * devicePixelRatio());
  auto resized = renderer_->resize(pixel_width, pixel_height);
  if (!resized && renderer_->state() == render::ViewportState::Removed) {
    resized = recover_failed_operation(resized);
    if (resized)
      resized = renderer_->resize(pixel_width, pixel_height);
  }
  if (!resized) {
    report(resized);
    return false;
  }
  return true;
}
} // namespace race_editor
