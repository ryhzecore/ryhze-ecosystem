#include "editor_viewport.hpp"

namespace race_editor {
bool Viewport::smoke_navigation() {
  const auto saved = target_;
  const auto generation = document_.generation();
  orbiting_ = true;
  target_ = {1e12, 1e9, -1e12};
  const auto distant_origin = target_;
  reset_navigation(false);
  navigate(1.0 / 60);
  bool passed = target_ == distant_origin;
  QFocusEvent focus(QEvent::FocusIn);
  QCoreApplication::sendEvent(this, &focus);
  QKeyEvent press(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &press);
  navigate(1.0 / 60);
  const auto moved = target_;
  const auto displacement = sub(moved, distant_origin);
  passed = passed && moved != distant_origin && dot(displacement, displacement) < .0101 &&
           document_.generation() == generation;
  QKeyEvent release(QEvent::KeyRelease, Qt::Key_D, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &release);
  navigate(1.0 / 60);
  passed = passed && target_ == moved;
  QCoreApplication::sendEvent(this, &press);
  QFocusEvent lost(QEvent::FocusOut);
  QCoreApplication::sendEvent(this, &lost);
  navigate(1.0 / 60);
  passed = passed && target_ == moved;
  QCoreApplication::sendEvent(this, &focus);
  orbiting_ = true;
  QKeyEvent repeat(QEvent::KeyPress, Qt::Key_D, Qt::NoModifier, {}, true);
  QCoreApplication::sendEvent(this, &repeat);
  navigate(1.0 / 60);
  passed = passed && target_ == moved;
  QCoreApplication::sendEvent(this, &press);
  navigate(1.0 / 60);
  passed = passed && target_ != moved && document_.generation() == generation;
  QCoreApplication::sendEvent(this, &release);
  target_ = saved;
  orbiting_ = false;
  reset_navigation(isActive());
  return passed;
}

bool Viewport::smoke_drag(bool cancel_edit, bool hold, int wanted_axis) {
  auto rows = document_.objects();
  if (!rows)
    return false;
  for (const auto &row : rows.value())
    if (row.id == selected) {
      if (tool < 0)
        tool = 0;
      QPointF start, end;
      if (!smoke_drag_endpoints(row, wanted_axis, start, end))
        return false;
      const auto before = document_.generation();
      QMouseEvent press(QEvent::MouseButtonPress, start, start, start, Qt::LeftButton, Qt::LeftButton,
                        Qt::NoModifier);
      QCoreApplication::sendEvent(this, &press);
      QMouseEvent move(QEvent::MouseMove, end, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &move);
      if (!preview_ || document_.generation() != before)
        return false;
      if (hold)
        return true;
      const auto expected = preview_->local;
      if (cancel_edit) {
        QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
        QCoreApplication::sendEvent(this, &escape);
      }
      QMouseEvent release(QEvent::MouseButtonRelease, end, end, end, Qt::LeftButton, Qt::NoButton,
                          Qt::NoModifier);
      QCoreApplication::sendEvent(this, &release);
      if (preview_)
        return false;
      if (cancel_edit)
        return document_.generation() == before;
      if (document_.generation() == before)
        return false;
      auto committed = document_.objects();
      if (!committed)
        return false;
      bool matches = false;
      for (const auto &candidate : committed.value())
        if (candidate.id == row.id)
          matches = candidate.local == expected && candidate.local != row.local;
      if (!matches || !document_.undo())
        return false;
      auto restored = document_.objects();
      if (!restored)
        return false;
      for (const auto &candidate : restored.value())
        if (candidate.id == row.id)
          return candidate.local == row.local;
    }
  return false;
}

bool Viewport::smoke_noop_drag() {
  auto rows = document_.objects();
  if (!rows)
    return false;
  const auto found = std::find_if(rows.value().begin(), rows.value().end(),
                                  [this](const auto &row) { return row.id == selected; });
  if (found == rows.value().end())
    return false;
  const auto generation = document_.generation();
  const auto saved_tool = tool;
  auto callback = on_edit;
  unsigned publications{};
  on_edit = [&] { ++publications; };
  bool passed = true;
  for (int mode = 0; mode < 3; ++mode) {
    tool = mode;
    auto handles = make_gizmo(*found);
    if (!handles) {
      passed = false;
      break;
    }
    QPointF start, end;
    bool picked = false;
    for (int a = 0; a < 3 && !picked; ++a) {
      const auto &h = handles.value().axes[a];
      if (!h.visible)
        continue;
      if (mode == 1) {
        for (int n = 1; n < 60; ++n)
          if (desktop::gizmo::hit_test(handles.value(), h.ring[n]) == a) {
            start = {h.ring[n].x, h.ring[n].y};
            if (h.edge_on) {
              const auto d = desktop::gizmo::drag_direction(handles.value(), h.ring[n], a);
              if (!d)
                continue;
              end = start + QPointF(d->x, d->y) * 30;
            } else
              end = {h.ring[n + 4].x, h.ring[n + 4].y};
            picked = true;
            break;
          }
      } else if (desktop::gizmo::hit_test(handles.value(), h.end) == a) {
        const auto d = desktop::gizmo::drag_direction(handles.value(), h.end, a);
        if (!d)
          continue;
        start = {h.end.x, h.end.y};
        end = start + QPointF(d->x, d->y) * 24;
        picked = true;
      }
    }
    if (!picked) {
      passed = false;
      break;
    }
    QMouseEvent press(QEvent::MouseButtonPress, start, start, start, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(this, &press);
    QMouseEvent outward(QEvent::MouseMove, end, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &outward);
    passed = passed && preview_.has_value();
    QMouseEvent back(QEvent::MouseMove, start, start, start, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &back);
    QMouseEvent release(QEvent::MouseButtonRelease, start, start, start, Qt::LeftButton, Qt::NoButton,
                        Qt::NoModifier);
    QCoreApplication::sendEvent(this, &release);
    passed = passed && !preview_ && document_.generation() == generation;
  }
  cancel();
  on_edit = std::move(callback);
  tool = saved_tool;
  if (on_tool)
    on_tool(tool);
  return passed && publications == 0;
}

bool Viewport::smoke_rotation_turns() {
  auto rows = document_.objects();
  if (!rows)
    return false;
  const auto found = std::find_if(rows.value().begin(), rows.value().end(),
                                  [this](const auto &row) { return row.id == selected; });
  if (found == rows.value().end())
    return false;
  const auto generation = document_.generation();
  const auto saved_tool = tool;
  tool = 1;
  bool passed = true;
  for (int a = 0; a < 3; ++a) {
    auto handles = make_gizmo(*found);
    if (!handles) {
      passed = false;
      break;
    }
    const auto &h = handles.value().axes[a];
    std::optional<desktop::gizmo::Point> tangent;
    int begin = -1;
    for (int n = 1; n < 64; ++n)
      if (h.visible && desktop::gizmo::hit_test(handles.value(), h.ring[n]) == a) {
        if (h.edge_on) {
          tangent = desktop::gizmo::drag_direction(handles.value(), h.ring[n], a);
          if (!tangent)
            continue;
        }
        begin = n;
        break;
      }
    if (begin < 0) {
      passed = false;
      break;
    }
    const QPointF start(h.ring[begin].x, h.ring[begin].y);
    QMouseEvent press(QEvent::MouseButtonPress, start, start, start, Qt::LeftButton, Qt::LeftButton,
                      Qt::NoModifier);
    QCoreApplication::sendEvent(this, &press);
    for (int n = 1; n <= 80; ++n) {
      const auto p = h.ring[(begin + n) % 64];
      const QPointF point = h.edge_on ? start + QPointF(tangent->x, tangent->y) *
                                                    (desktop::gizmo::handle_pixels * n * 2 * pi / 64)
                                      : QPointF(p.x, p.y);
      QMouseEvent move(QEvent::MouseMove, point, point, point, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
      QCoreApplication::sendEvent(this, &move);
      if (n >= 4)
        passed = passed && preview_ && std::abs(rotation_total_ - n * 2 * pi / 64) < 1e-8 &&
                 document_.generation() == generation;
    }
    QKeyEvent escape(QEvent::KeyPress, Qt::Key_Escape, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &escape);
    passed = passed && !preview_ && document_.generation() == generation;
  }
  cancel();
  tool = saved_tool;
  if (on_tool)
    on_tool(tool);
  return passed;
}

bool Viewport::smoke_axis_view(int view, int mode, const std::function<void()> &refresh,
                               const std::filesystem::path &image) {
  if (view < 0 || view > 5 || mode < 0 || mode > 2)
    return false;
  const auto pose = camera_pose();
  const auto saved_tool = tool;
  cancel();
  const auto restore = [&] {
    cancel();
    yaw_ = pose[0];
    pitch_ = pose[1];
    distance_ = pose[2];
    target_ = {pose[3], pose[4], pose[5]};
    tool = saved_tool;
    if (on_tool)
      on_tool(tool);
    refresh();
  };
  auto rows = document_.objects();
  if (!rows) {
    restore();
    return false;
  }
  const auto found = std::find_if(rows.value().begin(), rows.value().end(),
                                  [this](const auto &row) { return row.id == selected; });
  if (found == rows.value().end()) {
    restore();
    return false;
  }
  target_ = {found->world.m[3], found->world.m[7], found->world.m[11]};
  distance_ = 9;
  tool = mode;
  // X/Z are exactly aligned. Top/bottom use the editor's reachable orbit
  // limit; the Qt-free projection tests also cover exact +/-Y with a valid up.
  yaw_ = view == 0 ? pi / 2 : view == 1 ? -pi / 2 : view == 5 ? pi : 0;
  pitch_ = view == 2 ? 1.45 : view == 3 ? -1.45 : 0;
  bool passed = true;
  // This fixture is an unrotated box. In the exact +Z view, real X/Y are
  // horizontal/vertical and Z projects to the pivot: reject invented tilts.
  if (view == 4) {
    const auto handles = make_gizmo(*found);
    if (!handles) {
      restore();
      return false;
    }
    const auto &l = handles.value();
    const auto close = [](double a, double b) { return std::abs(a - b) < 1e-7; };
    if (mode != 1)
      passed = l.axes[2].end_on && close(l.axes[2].end.x, l.pivot.x) && close(l.axes[2].end.y, l.pivot.y) &&
               close(l.axes[0].end.y, l.pivot.y) && close(l.axes[1].end.x, l.pivot.x);
    else
      for (std::size_t n = 0; n < desktop::gizmo::ring_segments; ++n)
        passed = passed && close(l.axes[0].ring[n].x, l.pivot.x) && close(l.axes[1].ring[n].y, l.pivot.y);
  }
  for (int chosen = 0; chosen < 3; ++chosen) {
    const bool committed = smoke_drag(false, false, chosen);
    refresh();
    const bool cancelled = smoke_drag(true, false, chosen);
    refresh();
    passed = passed && committed && cancelled;
  }
  if (mode == 1)
    passed = smoke_rotation_turns() && passed;
  axis = -1;
  const bool drawn = render();
  passed = passed && drawn && renderer_ && renderer_->stats().overlay_draws == 3;
  if (drawn && renderer_ && !image.empty())
    passed = bool(renderer_->capture(image)) && passed;
  restore();
  return passed;
}

bool Viewport::smoke_tool_input() {
  const auto generation = document_.generation();
  const auto pose = camera_pose();
  const auto old_tool = tool;
  const auto key = [this](int code, bool repeat = false) {
    QKeyEvent event(QEvent::KeyPress, code, Qt::NoModifier, {}, repeat);
    QCoreApplication::sendEvent(this, &event);
  };
  bool passed = true;
  const int codes[]{Qt::Key_Q, Qt::Key_W, Qt::Key_E, Qt::Key_R};
  for (int i = 0; i < 4; ++i) {
    key(codes[i]);
    passed = passed && tool == i - 1;
  }
  const QPointF p(width() / 2., height() / 2.);
  QMouseEvent right(QEvent::MouseButtonPress, p, p, p, Qt::RightButton, Qt::RightButton, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &right);
  QMouseEvent left(QEvent::MouseButtonPress, p, p, p, Qt::LeftButton, Qt::LeftButton | Qt::RightButton,
                   Qt::NoModifier);
  QCoreApplication::sendEvent(this, &left);
  passed = passed && orbiting_ && !dragging_;
  key(Qt::Key_W);
  QMouseEvent up(QEvent::MouseButtonRelease, p, p, p, Qt::RightButton, Qt::NoButton, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &up);
  key(Qt::Key_W, true);
  passed = passed && tool == 2 && !orbiting_ && !dragging_;
  const auto stopped = camera_pose();
  navigate(1.0 / 60);
  passed = passed && camera_pose() == stopped;
  QKeyEvent released(QEvent::KeyRelease, Qt::Key_W, Qt::NoModifier);
  QCoreApplication::sendEvent(this, &released);
  key(Qt::Key_W);
  passed = passed && tool == 0;
  if (smoke_drag(false, true, 0)) {
    const auto start = start_;
    const auto preview = preview_->local;
    const QPointF point = last_ + QPointF(100, 80);
    QMouseEvent middle(QEvent::MouseButtonPress, point, point, point, Qt::MiddleButton,
                       Qt::LeftButton | Qt::MiddleButton, Qt::NoModifier);
    QCoreApplication::sendEvent(this, &middle);
    passed = passed && start_ == start && preview_ && preview_->local == preview;
    QFocusEvent lost(QEvent::FocusOut);
    QCoreApplication::sendEvent(this, &lost);
    passed = passed && !preview_ && !dragging_ && document_.generation() == generation;
  } else
    passed = false;
  if (smoke_drag(false, true, 0)) {
    frame_selected();
    passed = passed && !preview_ && !dragging_ && document_.generation() == generation;
  } else
    passed = false;
  smoke_selection_only_input(passed, generation);
  cancel();
  yaw_ = pose[0];
  pitch_ = pose[1];
  distance_ = pose[2];
  target_ = {pose[3], pose[4], pose[5]};
  tool = old_tool;
  if (on_tool)
    on_tool(tool);
  return passed;
}

bool Viewport::smoke_drag_endpoints(const desktop::ObjectRow &row, int wanted_axis, QPointF &start,
                                    QPointF &end) {
  auto handles = make_gizmo(row);
  if (!handles)
    return false;
  int chosen = -1;
  for (int a = 0; a < 3 && chosen < 0; ++a) {
    if (wanted_axis >= 0 && a != wanted_axis)
      continue;
    const auto &h = handles.value().axes[a];
    if (!h.visible)
      continue;
    if (tool == 1) {
      for (int n = 1; n < 60; ++n) {
        auto p = h.ring[n];
        if (desktop::gizmo::hit_test(handles.value(), p) != a)
          continue;
        start = {p.x, p.y};
        if (h.edge_on) {
          const auto d = desktop::gizmo::drag_direction(handles.value(), p, a);
          if (!d)
            continue;
          end = start + QPointF(d->x, d->y) * 30;
        } else
          end = {h.ring[(n + 5) % 64].x, h.ring[(n + 5) % 64].y};
        chosen = a;
        break;
      }
    } else {
      auto p = h.end;
      if (desktop::gizmo::hit_test(handles.value(), p) == a) {
        const auto d = desktop::gizmo::drag_direction(handles.value(), p, a);
        if (!d)
          continue;
        start = {p.x, p.y};
        end = start + QPointF(d->x, d->y) * 24;
        chosen = a;
      }
    }
  }
  if (chosen < 0)
    return false;
  return true;
}

void Viewport::smoke_selection_only_input(bool &passed, std::uint64_t generation) {
  auto rows = document_.objects();
  if (rows)
    for (const auto &row : rows.value())
      if (row.id == selected) {
        tool = 0;
        auto handles = make_gizmo(row);
        if (!handles) {
          passed = false;
          break;
        }
        tool = -1;
        const QPointF center(handles.value().pivot.x, handles.value().pivot.y), end = center + QPointF(25, 0);
        QMouseEvent press(QEvent::MouseButtonPress, center, center, center, Qt::LeftButton, Qt::LeftButton,
                          Qt::NoModifier);
        QCoreApplication::sendEvent(this, &press);
        QMouseEvent move(QEvent::MouseMove, end, end, end, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(this, &move);
        QMouseEvent release(QEvent::MouseButtonRelease, end, end, end, Qt::LeftButton, Qt::NoButton,
                            Qt::NoModifier);
        QCoreApplication::sendEvent(this, &release);
        passed = passed && !preview_ && !dragging_ && document_.generation() == generation;
        break;
      }
}
} // namespace race_editor
