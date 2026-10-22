#include "editor_viewport.hpp"

namespace race_editor {
std::optional<desktop::NavigationAction> Viewport::movement(int key) {
  using A = desktop::NavigationAction;
  switch (key) {
  case Qt::Key_A:
    return A::Left;
  case Qt::Key_D:
    return A::Right;
  case Qt::Key_W:
    return A::Forward;
  case Qt::Key_S:
    return A::Backward;
  case Qt::Key_Q:
    return A::Down;
  case Qt::Key_E:
    return A::Up;
  default:
    return {};
  }
}

void Viewport::cancel() {
  preview_.reset();
  drag_layout_.reset();
  setMouseGrabEnabled(false);
  dragging_ = false;
  orbiting_ = false;
  reset_navigation(navigation_enabled_ && isActive());
}

void Viewport::reset_navigation(bool active) {
  report(navigation_.reset(desktop::navigation_timestamp(), active));
}

void Viewport::set_navigation_enabled(bool enabled) {
  navigation_enabled_ = enabled;
  reset_navigation(enabled && isActive());
}

void Viewport::navigate(double seconds) {
  if (!navigation_enabled_ || !orbiting_)
    return;
  auto sample = navigation_.sample(desktop::navigation_timestamp(), ++navigation_update_);
  if (!sample) {
    report(sample.error());
    reset_navigation(false);
    return;
  }
  const auto &value = sample.value();
  if ((value.right == 0 && value.forward == 0 && value.up == 0) || !std::isfinite(seconds) || seconds <= 0)
    return;
  const double scale = 6 * std::min(seconds, .05);
  const Vec next{target_.x + (value.right * std::cos(yaw_) - value.forward * std::sin(yaw_)) * scale,
                 target_.y + value.up * scale,
                 target_.z + (-value.right * std::sin(yaw_) - value.forward * std::cos(yaw_)) * scale};
  // Frame-selected authoring coordinates may be far outside the standalone
  // runtime camera's navigation bounds. Bound displacement, never teleport it.
  if (std::isfinite(next.x) && std::isfinite(next.y) && std::isfinite(next.z))
    target_ = next;
}

bool Viewport::event(QEvent *e) {
  if (e->type() == QEvent::PlatformSurface) {
    auto *surface = static_cast<QPlatformSurfaceEvent *>(e);
    if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
      surface_available_ = false;
      cancel();
      if (renderer_)
        report(renderer_->retire_surface());
    } else if (surface->surfaceEventType() == QPlatformSurfaceEvent::SurfaceCreated)
      surface_available_ = true;
  }
  if (e->type() == QEvent::Resize && dragging_)
    cancel();
  if (e->type() == QEvent::FocusOut || e->type() == QEvent::Hide) {
    cancel();
    reset_navigation(false);
  }
  if (e->type() == QEvent::FocusIn)
    reset_navigation(navigation_enabled_);
  return QWindow::event(e);
}

void Viewport::keyPressEvent(QKeyEvent *e) {
  if (!orbiting_ && !dragging_ && !e->isAutoRepeat() && e->modifiers() == Qt::NoModifier) {
    const int next = e->key() == Qt::Key_Q   ? -1
                     : e->key() == Qt::Key_W ? 0
                     : e->key() == Qt::Key_E ? 1
                     : e->key() == Qt::Key_R ? 2
                                             : -2;
    if (next != -2) {
      cancel();
      tool = next;
      axis = -1;
      if (on_tool)
        on_tool(tool);
      if (on_axis)
        on_axis(axis);
      e->accept();
      return;
    }
  }
  if (navigation_enabled_ && orbiting_ && e->modifiers() == Qt::NoModifier)
    if (auto action = movement(e->key())) {
      auto admitted = navigation_.push(
          {navigation_.context(), desktop::navigation_timestamp(), *action, true, e->isAutoRepeat()});
      if (!admitted) {
        report(admitted);
        reset_navigation(navigation_enabled_ && isActive());
      }
      e->accept();
      return;
    }
  if (e->key() == Qt::Key_Escape)
    cancel();
  else if (e->key() == Qt::Key_F)
    frame_selected();
  else
    QWindow::keyPressEvent(e);
}

void Viewport::keyReleaseEvent(QKeyEvent *e) {
  if (navigation_enabled_)
    if (auto action = movement(e->key())) {
      if (navigation_.active()) {
        auto admitted = navigation_.push(
            {navigation_.context(), desktop::navigation_timestamp(), *action, false, e->isAutoRepeat()});
        if (!admitted) {
          report(admitted);
          reset_navigation(navigation_enabled_ && isActive());
        }
      }
      e->accept();
      return;
    }
  QWindow::keyReleaseEvent(e);
}

void Viewport::mousePressEvent(QMouseEvent *e) {
  if (e->button() == Qt::RightButton) {
    cancel();
    last_ = e->position();
    requestActivate();
    orbiting_ = true;
    reset_navigation(navigation_enabled_);
    return;
  }
  if (e->button() != Qt::LeftButton || document_.playing() || orbiting_ || dragging_)
    return;
  last_ = start_ = e->position();
  // Admit inspector values before picking/copying a drag row. Publication can
  // move the object and advance its generation, so the old row is not reusable.
  if (on_admit_edit && !on_admit_edit())
    return;
  requestActivate();
  auto rows = document_.objects();
  if (!rows)
    return;
  if (tool >= 0)
    for (const auto &row : rows.value())
      if (row.id == selected && row.visible) {
        auto handles = make_gizmo(row);
        if (!handles)
          break;
        if (auto chosen = desktop::gizmo::hit_test(handles.value(), {e->position().x(), e->position().y()})) {
          begin_gizmo(row, handles.value(), *chosen);
          return;
        }
      }
  pick_geometry(e, rows.value());
}

void Viewport::mouseMoveEvent(QMouseEvent *e) {
  QPointF delta = e->position() - last_;
  last_ = e->position();
  if (!dragging_ && !orbiting_) {
    axis =
        displayed_gizmo_
            ? desktop::gizmo::hit_test(*displayed_gizmo_, {e->position().x(), e->position().y()}).value_or(-1)
            : -1;
    const bool end_on =
        axis >= 0 && displayed_gizmo_ && displayed_gizmo_->axes[static_cast<std::size_t>(axis)].end_on;
    setCursor(end_on ? Qt::SizeVerCursor : axis >= 0 ? Qt::SizeAllCursor : Qt::ArrowCursor);
  }
  if (orbiting_) {
    yaw_ -= delta.x() * .008;
    pitch_ = std::clamp(pitch_ + delta.y() * .008, -1.45, 1.45);
  }
  if (dragging_) {
    QPointF total = e->position() - start_;
    if (!preview_ && std::abs(total.x()) + std::abs(total.y()) < 4)
      return;
    if (!drag_layout_ || axis < 0 || document_.generation() != drag_generation_) {
      cancel();
      return;
    }
    const auto amount = desktop::gizmo::drag_amount(*drag_layout_, {start_.x(), start_.y()},
                                                    {e->position().x(), e->position().y()}, axis);
    if (!amount)
      return;
    preview_ = before_;
    const Vec a = axis == 0 ? Vec{1, 0, 0} : axis == 1 ? Vec{0, 1, 0} : Vec{0, 0, 1};
    if (tool == 0)
      preview_->local.translation =
          add(before_.local.translation, mul(move_axes_[axis], *amount / axis_lengths_[axis]));
    else if (tool == 1) {
      rotation_total_ += std::remainder(*amount - rotation_previous_, 2 * pi);
      rotation_previous_ = *amount;
      auto rotation = scene::axis_angle(a, rotation_total_);
      if (!rotation) {
        preview_.reset();
        return;
      }
      preview_->local.rotation = multiply(before_.local.rotation, rotation.value());
    } else {
      if (axis == 0)
        preview_->local.scale.x *= *amount;
      if (axis == 1)
        preview_->local.scale.y *= *amount;
      if (axis == 2)
        preview_->local.scale.z *= *amount;
    }
    if (!scene::validate(preview_->local))
      preview_.reset();
  }
}

void Viewport::mouseReleaseEvent(QMouseEvent *e) {
  if (e->button() == Qt::RightButton) {
    orbiting_ = false;
    reset_navigation(navigation_enabled_ && isActive());
    return;
  }
  if (e->button() != Qt::LeftButton)
    return;
  dragging_ = false;
  drag_layout_.reset();
  setMouseGrabEnabled(false);
  if (preview_) {
    if (preview_->local == before_.local) {
      preview_.reset();
      return;
    }
    const auto generation = document_.generation();
    auto edited = document_.edit(*preview_, drag_generation_);
    preview_.reset();
    report(edited);
    if (edited && document_.generation() != generation && on_edit)
      on_edit();
  }
}

void Viewport::wheelEvent(QWheelEvent *e) {
  if (dragging_)
    return;
  distance_ = std::clamp(distance_ * std::exp(-e->angleDelta().y() * .001), .2, 10000.0);
}

void Viewport::pick_geometry(QMouseEvent *e, const std::vector<desktop::ObjectRow> &rows) {
  Vec origin = eye(), forward = unit(sub(target_, origin)), right = unit(cross(forward, {0, 1, 0})),
      up = cross(right, forward);
  double aspect = double(width()) / std::max(1, height()), tangent = std::tan(pi / 8);
  Vec direction = unit(add(forward, add(mul(right, (2 * e->position().x() / width() - 1) * aspect * tangent),
                                        mul(up, (1 - 2 * e->position().y() / height()) * tangent))));
  double nearest = 1e100;
  std::optional<desktop::ObjectRow> hit;
  for (const auto &row : rows)
    if (row.visible) {
      auto inv = scene::inverse(row.world);
      if (!inv)
        continue;
      const auto &m = inv.value().m;
      Vec local_origin{m[0] * origin.x + m[1] * origin.y + m[2] * origin.z + m[3],
                       m[4] * origin.x + m[5] * origin.y + m[6] * origin.z + m[7],
                       m[8] * origin.x + m[9] * origin.y + m[10] * origin.z + m[11]};
      Vec local_dir{m[0] * direction.x + m[1] * direction.y + m[2] * direction.z,
                    m[4] * direction.x + m[5] * direction.y + m[6] * direction.z,
                    m[8] * direction.x + m[9] * direction.y + m[10] * direction.z};
      auto bounds = desktop::local_bounds(row, meshes_.get());
      if (!bounds) {
        report(bounds.error());
        return;
      }
      const auto &b = bounds.value();
      std::array<double, 3> o{local_origin.x, local_origin.y, local_origin.z},
          d{local_dir.x, local_dir.y, local_dir.z};
      double near_t = 0, far_t = 1e100;
      for (int i = 0; i < 3; ++i) {
        if (std::abs(d[i]) < 1e-12) {
          if (o[i] < b.minimum[i] || o[i] > b.maximum[i])
            far_t = -1;
        } else {
          double a = (b.minimum[i] - o[i]) / d[i], z = (b.maximum[i] - o[i]) / d[i];
          if (a > z)
            std::swap(a, z);
          near_t = std::max(near_t, a);
          far_t = std::min(far_t, z);
        }
      }
      if (near_t <= far_t && near_t < nearest) {
        nearest = near_t;
        hit = row;
      }
    }
  selected = hit ? hit->id : PersistentId{};
  if (on_select)
    on_select(selected);
  axis = -1;
  if (on_axis)
    on_axis(axis); // Clicking geometry selects; only handles transform it.
}
} // namespace race_editor
