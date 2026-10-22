// Opt-in native UI smoke: header and panel layouts at four window sizes, menus, header actions, gizmos and
// viewport input, plus the dialog and popup pixel checks used by the content smokes.
#include "editor_window.hpp"
#include <QObject>
#include <array>

namespace race_editor {
void Editor::smoke_dialog_curves(QDialog &dialog, const QString &tag, QLineEdit *field) {
  auto *surface = dialog.findChild<QFrame *>("DialogSurface");
  const auto capture = dialog.grab();
  const auto image = capture.toImage();
  const auto ratio = capture.devicePixelRatio();
  const auto color = [&](QWidget *widget, int x, int y) {
    const auto point = widget->mapTo(&dialog, QPoint(x, y));
    return image.pixelColor(int((point.x() + .5) * ratio), int((point.y() + .5) * ratio));
  };
  const auto corners = [&](QWidget *widget, const QColor &expected) {
    return color(widget, 0, 0) == expected && color(widget, widget->width() - 1, 0) == expected &&
           color(widget, 0, widget->height() - 1) == expected &&
           color(widget, widget->width() - 1, widget->height() - 1) == expected;
  };
  const bool ready = surface && field && !image.isNull() && surface->width() > 40 && surface->height() > 40 &&
                     field->width() > 40;
  // A control can fit the dialog while still being clipped by an intermediate
  // row. Check every visible input/action against each actual clipping parent.
  const auto contained = [&](QWidget *control) {
    for (auto *node = control; node != surface; node = node->parentWidget()) {
      auto *parent = node->parentWidget();
      if (!parent || !parent->rect().contains(node->geometry()))
        return false;
    }
    return surface && dialog.rect().contains(QRect(surface->mapTo(&dialog, QPoint{}), surface->size()));
  };
  bool controls_contained = ready;
  int visible_controls = 0;
  if (ready)
    for (auto *control : dialog.findChildren<QWidget *>()) {
      if (!control->isVisibleTo(&dialog) ||
          (!qobject_cast<QLineEdit *>(control) && !qobject_cast<QComboBox *>(control) &&
           !qobject_cast<QAbstractButton *>(control)))
        continue;
      ++visible_controls;
      controls_contained = controls_contained && contained(control);
    }
  smoke_check(tag + "_all_controls_contained_through_ancestors", controls_contained && visible_controls >= 4);
  smoke_check(tag + "_rounded_surface_pixels",
              ready && corners(surface, QColor("#090A0C")) &&
                  color(surface, surface->width() / 2, 3) == QColor("#111214"));
  smoke_check(tag + "_rounded_field_pixels",
              ready && field->height() == desktop::ui::ShapeTokens::compact_height &&
                  corners(field, QColor("#111214")) && color(field, 5, 2) == QColor("#111214") &&
                  color(field, field->width() / 2, 3) == QColor("#18191C"));
  for (auto *combo : dialog.findChildren<QComboBox *>())
    if (combo->isVisibleTo(&dialog))
      smoke_check(tag + "_rounded_combo_pixels",
                  ready && combo->height() == desktop::ui::ShapeTokens::compact_height &&
                      corners(combo, QColor("#111214")) &&
                      color(combo, combo->width() / 2, 3) == QColor("#18191C"));
  auto *buttons = dialog.findChild<QDialogButtonBox *>();
  bool buttons_rounded = ready && buttons;
  if (buttons_rounded)
    for (auto *button : buttons->buttons())
      buttons_rounded = buttons_rounded && button->height() == desktop::ui::ShapeTokens::main_height &&
                        corners(button, QColor("#111214")) &&
                        color(button, button->width() / 2, 0) != QColor("#111214");
  smoke_check(tag + "_rounded_dialog_buttons", buttons_rounded);
  smoke_check(tag + "_rounded_surface_capture",
              ready && surface->grab().save(smoke_ + "/" + tag + "-surface-" +
                                            QString::number(checkpoint_stage_) + ".png"));
}
void Editor::smoke_profile_choices(QComboBox &profile) {
  auto *view = profile.view();
  auto *viewport = view->viewport();
  auto *popup = view->window();
  const auto key = [view](Qt::Key value) {
    // Native Qt keyboard dispatch offers ShortcutOverride before KeyPress.
    // Return can commit and close the popup at that first event.
    QKeyEvent shortcut(QEvent::ShortcutOverride, value, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &shortcut);
    if (!view->isVisible())
      return;
    QKeyEvent press(QEvent::KeyPress, value, Qt::NoModifier),
        release(QEvent::KeyRelease, value, Qt::NoModifier);
    QCoreApplication::sendEvent(view, &press);
    QCoreApplication::sendEvent(view, &release);
  };
  profile.setFocus(Qt::OtherFocusReason);
  profile.showPopup();
  authoring_check("profile_popup_capture",
                  view->isVisible() && popup->grab().save(smoke_ + "/authoring-profile-popup.png"));
  smoke_profile_capture(profile, view, viewport, popup, 0, "profile-geometry-popup");
  key(Qt::Key_End);
  authoring_check("profile_open_keyboard_highlight",
                  view->isVisible() && view->currentIndex().row() == 1 && profile.currentIndex() == 0);
  smoke_profile_capture(profile, view, viewport, popup, 1, "profile-uv-popup");
  key(Qt::Key_Return);
  authoring_check("profile_open_keyboard_choice",
                  !view->isVisible() &&
                      profile.currentData().toInt() == static_cast<int>(mesh_pipeline::Profile::GeometryUv));
  profile.showPopup();
  const auto geometry = view->model()->index(0, profile.modelColumn(), profile.rootModelIndex());
  const auto row = view->visualRect(geometry);
  authoring_check("profile_mouse_target_visible",
                  view->isVisible() && row.isValid() && viewport->rect().contains(row));
  if (view->isVisible() && row.isValid() && viewport->rect().contains(row)) {
    const auto send = [&](QEvent::Type type, const QPoint &point, Qt::MouseButton button,
                          Qt::MouseButtons buttons) {
      const QPointF local(point), window(viewport->mapTo(popup, point)), global(viewport->mapToGlobal(point));
      QMouseEvent event(type, local, window, global, button, buttons, Qt::NoModifier);
      QCoreApplication::sendEvent(viewport, &event);
    };
    // Deliver actual motion before the click, including a distinct entry point,
    // so Qt's opening-release guard sees a move into the chosen row.
    send(QEvent::MouseMove, QPoint(row.left() + 3, row.center().y()), Qt::NoButton, Qt::NoButton);
    send(QEvent::MouseMove, row.center(), Qt::NoButton, Qt::NoButton);
    send(QEvent::MouseButtonPress, row.center(), Qt::LeftButton, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, row.center(), Qt::LeftButton, Qt::NoButton);
  }
  authoring_check("profile_open_mouse_choice",
                  !view->isVisible() &&
                      profile.currentData().toInt() == static_cast<int>(mesh_pipeline::Profile::Geometry));
  // Restore UV through the same open-popup keyboard route for the real recipe.
  profile.showPopup();
  key(Qt::Key_End);
  key(Qt::Key_Return);
  authoring_check("profile_keyboard_restores_uv",
                  !view->isVisible() &&
                      profile.currentData().toInt() == static_cast<int>(mesh_pipeline::Profile::GeometryUv));
  if (view->isVisible())
    profile.hidePopup();
}
// One open-popup row of smoke_profile_choices: capture, rounded selection and frame pixels, placement
// next to the combo box and containment of every row.
void Editor::smoke_profile_capture(QComboBox &profile, QAbstractItemView *view, QWidget *viewport,
                                   QWidget *popup, int row, const QString &name) {
  const auto index = view->model()->index(row, profile.modelColumn(), profile.rootModelIndex());
  const auto item = view->visualRect(index);
  const QRect bounds(viewport->mapTo(popup, item.topLeft()), item.size());
  const auto pixels = popup->grab();
  const auto image = pixels.toImage();
  const auto ratio = pixels.devicePixelRatio();
  const bool ready = view->isVisible() && index.isValid() && item.width() > 40 && item.height() > 12 &&
                     !image.isNull() && viewport->rect().contains(item) && popup->rect().contains(bounds) &&
                     view->currentIndex() == index && view->selectionModel()->isSelected(index);
  authoring_check(name + "_capture", ready && pixels.save(smoke_ + "/authoring-" + name + ".png"));
  const auto pixel = [&](int x, int y) {
    return image.pixelColor(int((bounds.x() + x + .5) * ratio), int((bounds.y() + y + .5) * ratio));
  };
  const QColor background("#18191C"), selected("#165BFA");
  authoring_check(name + "_rounded_selected_row",
                  ready && pixel(1, 1) == background && pixel(item.width() - 2, 1) == background &&
                      pixel(1, item.height() - 2) == background &&
                      pixel(item.width() - 2, item.height() - 2) == background &&
                      pixel(item.width() / 2, 3) == selected);
  authoring_check(name + "_rounded_frame",
                  ready && image.pixelColor(0, 0).alpha() == 0 &&
                      image.pixelColor(image.width() - 1, 0).alpha() == 0 &&
                      image.pixelColor(0, image.height() - 1).alpha() == 0 &&
                      image.pixelColor(image.width() - 1, image.height() - 1).alpha() == 0);
  const QRect anchor(profile.mapToGlobal(QPoint{}), profile.size()),
      global(popup->mapToGlobal(QPoint{}), popup->size());
  const bool adjacent =
      std::abs(global.top() - anchor.bottom() - 1) <= 4 || std::abs(global.bottom() + 1 - anchor.top()) <= 4;
  const bool overlaps = global.left() <= anchor.right() && global.right() >= anchor.left();
  authoring_check(name + "_placement", ready && popup->screen() &&
                                           popup->screen()->availableGeometry().contains(global) &&
                                           adjacent && overlaps);
  bool contained = ready;
  for (int n = 0; n < profile.count(); ++n) {
    const auto rect =
        view->visualRect(view->model()->index(n, profile.modelColumn(), profile.rootModelIndex()));
    contained = contained && rect.isValid() && viewport->rect().contains(rect);
  }
  authoring_check(name + "_all_rows_contained", contained);
}
namespace {
// Header controls checked by the UI smoke, in their left-to-right order.
const char *const header_names[] = {"UiFile", "UiEdit", "UiView", "UiUndo", "UiRedo", "UiSave", "UiPlay"};
} // namespace
void Editor::ui_smoke_step() {
  if (frames_ < 8 || !viewport_->renderer())
    return;
  auto *header = findChild<QToolBar *>("MainHeader");
  auto *brand = findChild<QLabel *>("HeaderBrand");
  auto *badge = findChild<QLabel *>("Eyebrow");
  bool ready = header && brand && badge;
  for (auto *n : header_names)
    ready = ready && ui_button(n);
  for (auto *n : {"UiSelect", "UiMove", "UiRotate", "UiScale"})
    ready = ready && ui_button(n);
  if (!ready) {
    smoke_check("named_ui_controls_present", false);
    ui_layout_ = 4;
    ui_step_ = 100;
  }
  if (ui_layout_ < 4) {
    ui_smoke_layout(header, brand, badge);
    return;
  }
  if (ui_step_ == 0) {
    resize(1280, 800);
    ++ui_step_;
    return;
  }
  if (ui_step_ < 3) {
    ++ui_step_;
    return;
  }
  // Posted events run after this timer frame returns, permitting QMenu's nested event loop.
  if (ui_step_ >= 3 && ui_step_ < 12) {
    ui_smoke_menu();
    return;
  }
  // Steps 12-27 run one group of checks each; any later step (including 100) writes the receipt.
  const int step = ui_step_++;
  if (step >= 12 && step <= 16)
    ui_smoke_action_step(step);
  else if (step >= 17 && step <= 19)
    ui_smoke_view_menu_step(step, header);
  else if (step >= 20 && step <= 24)
    ui_smoke_gizmo_step(step);
  else if (step >= 25 && step <= 27)
    ui_smoke_viewport_step(step);
  else
    ui_smoke_finish();
}
QToolButton *Editor::ui_button(const char *n) const {
  return findChild<QToolButton *>(QString::fromLatin1(n));
}
QRect Editor::ui_rect(QWidget *w) const { return QRect(w->mapTo(this, QPoint{}), w->size()); }
std::string Editor::ui_scene_json() {
  auto snapshot = document_->snapshot();
  if (!snapshot) {
    error("UI smoke snapshot failed");
    return {};
  }
  auto bytes = scene::encode_json(snapshot.value(), *document_->schemas(), {});
  if (!bytes) {
    error("UI smoke Scene encoding failed");
    return {};
  }
  return std::move(bytes).value();
}
void Editor::ui_trigger(const char *n) {
  auto *b = ui_button(n);
  if (!b || !b->defaultAction()) {
    smoke_check(QString(n) + "_action", false);
    return;
  }
  b->defaultAction()->trigger();
}
// One layout pass per requested size: settle, check the header, tools, panels and actions, then record it.
void Editor::ui_smoke_layout(QToolBar *header, QLabel *brand, QLabel *badge) {
  const QSize sizes[] = {QSize(1280, 800), QSize(1100, 720), QSize(1000, 760), QSize(800, 600)};
  const auto wanted = sizes[ui_layout_];
  if (ui_settle_++ == 0) {
    if (!rows_.empty())
      select_object(rows_.front().id);
    resize(wanted);
    return;
  }
  if (ui_settle_ < 4)
    return;
  const auto tag = QString("%1x%2").arg(wanted.width()).arg(wanted.height());
  smoke_check(tag + "_requested_client_size", size() == wanted);
  std::vector<QRect> visible{ui_rect(brand)};
  if (badge->isVisible())
    visible.push_back(ui_rect(badge));
  bool contained = ui_rect(header).contains(ui_rect(brand)) &&
                   (!badge->isVisible() || ui_rect(header).contains(ui_rect(badge)));
  QJsonArray controls;
  bool ordered = ui_rect(brand).left() < ui_rect(header).center().x();
  bool all_visible = true;
  for (auto *n : header_names) {
    auto *b = ui_button(n);
    const auto r = ui_rect(b);
    controls.append(QJsonObject{{"name", n},
                                {"visible", b->isVisible()},
                                {"x", r.x()},
                                {"y", r.y()},
                                {"width", r.width()},
                                {"height", r.height()}});
    all_visible = all_visible && b->isVisible();
    if (!b->isVisible())
      continue;
    visible.push_back(r);
    contained = contained && ui_rect(header).contains(r);
    ordered = ordered && ui_rect(brand).right() < r.left();
  }
  bool overlap = false;
  for (std::size_t i = 0; i < visible.size(); ++i)
    for (std::size_t j = i + 1; j < visible.size(); ++j)
      overlap = overlap || visible[i].intersects(visible[j]);
  smoke_check(tag + "_header_contained_and_disjoint", contained && !overlap);
  smoke_check(tag + "_header_controls_visible", all_visible);
  ui_smoke_layout_tools(tag);
  const auto panels = ui_smoke_layout_panels(tag);
  const auto [body, heading] = ui_smoke_layout_actions(tag, header, brand, ordered);
  const auto png = tag + "-editor-layout-only.png";
  smoke_check(tag + "_layout_png", grab().save(smoke_ + "/" + png));
  ui_layouts_.append(QJsonObject{{"requested_width", wanted.width()},
                                 {"requested_height", wanted.height()},
                                 {"actual_width", width()},
                                 {"actual_height", height()},
                                 {"device_pixel_ratio", devicePixelRatioF()},
                                 {"body_font", body},
                                 {"heading_font", heading},
                                 {"controls", controls},
                                 {"panels", panels},
                                 {"layout_only_png", png}});
  ++ui_layout_;
  ui_settle_ = 0;
}
void Editor::ui_smoke_layout_tools(const QString &tag) {
  bool tools_visible = true;
  for (auto *n : {"UiAddBox", "UiAddMesh", "UiSelect", "UiMove", "UiRotate", "UiScale"})
    tools_visible = tools_visible && ui_button(n)->isVisible();
  smoke_check(tag + "_scene_tools_visible", tools_visible);
  auto *scene_tools = findChild<QToolBar *>("SceneTools");
  auto *tool_group = findChild<QWidget *>("SegmentedTools");
  bool grouped_tools = scene_tools && tool_group && tool_group->isVisible() &&
                       ui_rect(scene_tools).contains(ui_rect(tool_group));
  QRect previous_tool;
  for (auto *n : {"UiSelect", "UiMove", "UiRotate", "UiScale"}) {
    auto *mode = ui_button(n);
    const auto r = ui_rect(mode);
    grouped_tools = grouped_tools && mode->parentWidget() == tool_group && ui_rect(tool_group).contains(r) &&
                    (previous_tool.isNull() || previous_tool.right() < r.left()) && !mode->text().isEmpty();
    previous_tool = r;
  }
  smoke_check(tag + "_text_tools_grouped_and_contained", grouped_tools);
}
QJsonArray Editor::ui_smoke_layout_panels(const QString &tag) {
  auto *inspector_scroll = properties_scroll();
  smoke_check(tag + "_inspector_no_horizontal_clipping",
              inspector_scroll && inspector_->width() <= inspector_scroll->viewport()->width());
  bool fields_fit = true;
  for (const auto &group : fields_) {
    QRect previous;
    for (auto *field : group) {
      const auto r = ui_rect(field);
      const auto parent = ui_rect(field->parentWidget()->parentWidget());
      fields_fit = fields_fit && parent.contains(r) && field->height() == 28 &&
                   (previous.isNull() || previous.right() < r.left());
      previous = r;
    }
  }
  smoke_check(tag + "_vector_fields_fit_rows",
              inspector_scroll && fields_fit && inspector_scroll->horizontalScrollBar()->maximum() == 0);
  QJsonArray panels;
  const auto geometry = [](QRect r) { return QJsonArray{r.x(), r.y(), r.width(), r.height()}; };
  bool connected = true;
  for (auto *name : {"Hierarchy", "Properties"}) {
    auto *dock = findChild<QDockWidget *>(name);
    const auto title = ui_rect(dock->titleBarWidget()), body = ui_rect(dock->widget());
    panels.append(QJsonObject{{"panel", name},
                              {"label", dock->windowTitle()},
                              {"title", geometry(title)},
                              {"body", geometry(body)}});
    connected = connected && title.left() == body.left() && title.right() == body.right() &&
                title.bottom() + 1 == body.top();
  }
  const auto scene_header = ui_rect(findChild<QWidget *>("PanelHeader"));
  const auto scene_view = ui_rect(findChild<QWidget *>("SceneViewport"));
  panels.append(
      QJsonObject{{"panel", "Scene"}, {"title", geometry(scene_header)}, {"body", geometry(scene_view)}});
  smoke_check(tag + "_continuous_panel_edges", connected && scene_header.left() == scene_view.left() &&
                                                   scene_header.right() == scene_view.right() &&
                                                   scene_header.bottom() + 1 == scene_view.top());
  auto *objects = findChild<QDockWidget *>("Hierarchy");
  auto *properties = findChild<QDockWidget *>("Properties");
  const auto objects_area = ui_rect(objects), properties_area = ui_rect(properties),
             scene_area = ui_rect(centralWidget());
  smoke_check(tag + "_objects_below_scene_properties_right",
              dockWidgetArea(objects) == Qt::BottomDockWidgetArea &&
                  dockWidgetArea(properties) == Qt::RightDockWidgetArea &&
                  objects->windowTitle() == "Objects" && objects_area.left() == scene_area.left() &&
                  objects_area.right() == scene_area.right() && objects_area.top() > scene_area.bottom() &&
                  properties_area.left() > scene_area.right() && properties_area.top() == scene_area.top() &&
                  properties_area.bottom() == objects_area.bottom());
  smoke_check(tag + "_all_numeric_groups_visible",
              inspector_scroll && ui_rect(inspector_scroll->viewport()).contains(ui_rect(fields_[4][2])));
  return panels;
}
// Returns the body and heading font families recorded with the layout.
std::pair<QString, QString> Editor::ui_smoke_layout_actions(const QString &tag, QToolBar *header,
                                                            QLabel *brand, bool ordered) {
  auto *view = ui_button("UiView");
  auto *undo = ui_button("UiUndo");
  auto *save = ui_button("UiSave");
  auto *play = ui_button("UiPlay");
  if (view->isVisible() && undo->isVisible())
    ordered = ordered && ui_rect(view).right() < ui_rect(undo).left();
  if (save->isVisible() && play->isVisible())
    ordered = ordered && ui_rect(save).right() < ui_rect(play).left() &&
              ui_rect(play).right() > ui_rect(header).center().x();
  smoke_check(tag + "_brand_left_actions_right", ordered && brand->isVisible());
  bool circles = true;
  for (auto *n : {"UiUndo", "UiRedo"}) {
    auto *b = ui_button(n);
    circles = circles && b->property("circular").toBool() &&
              b->size() == (b->property("compact").toBool() ? QSize(30, 30) : QSize(36, 36));
  }
  smoke_check(tag + "_circular_controls_compact", circles);
  smoke_check(tag + "_compact_control_heights", save->height() == 36 && play->height() == 36 &&
                                                    ui_button("UiMove")->height() == 30 &&
                                                    ui_button("UiSelect")->height() == 30);
  smoke_check(tag + "_primary_and_default_checked",
              play->property("primary").toBool() && ui_button("UiMove")->isChecked());
  const auto body = QFontInfo(save->font()).family(), heading = QFontInfo(brand->font()).family();
  smoke_check(tag + "_bundled_font_families", body == QFontInfo(QApplication::font()).family() &&
                                                  body.startsWith("Inter") &&
                                                  heading == desktop::ui::heading_family());
  return {body, heading};
}
namespace {
// Diagnostic-only observer: bounded snapshots, no event consumption or dispatch changes.
class UiMenuTrace final : public QObject {
  struct Entry {
    const char *phase{};
    int step{}, event_type{};
    bool spontaneous{}, enabled{}, visible{}, focus{}, menu_visible{};
    quintptr receiver{}, active_window{}, active_popup{}, focus_widget{};
  };
  QWidget &window_;
  QToolButton &button_;
  QMenu &menu_;
  const int &step_;
  std::array<Entry, 128> entries_{};
  int count_{};
  bool truncated_{}, sampled_{};

  bool eventFilter(QObject *watched, QEvent *event) override {
    const auto type = event->type();
    const bool key = type == QEvent::KeyPress || type == QEvent::KeyRelease;
    const bool focus = type == QEvent::FocusIn || type == QEvent::FocusOut ||
                       type == QEvent::WindowActivate || type == QEvent::WindowDeactivate;
    const bool related = watched == &button_ || watched == &menu_ || watched == &window_ ||
                         watched == window_.windowHandle() || watched == menu_.windowHandle();
    if ((key && watched == &button_ && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Space) ||
        (focus && related) || ((type == QEvent::Show || type == QEvent::Hide) && watched == &menu_))
      record("qt_event_before_handler", watched, event);
    return false;
  }

public:
  UiMenuTrace(QWidget &window, QToolButton &button, QMenu &menu, const int &step)
      : window_(window), button_(button), menu_(menu), step_(step) {
    qApp->installEventFilter(this);
    record("cycle_begin", &button_);
  }
  ~UiMenuTrace() override { qApp->removeEventFilter(this); }
  void record(const char *phase, QObject *receiver, QEvent *event = nullptr) {
    if (count_ == int(entries_.size())) {
      truncated_ = true;
      return;
    }
    entries_[count_++] = {phase,
                          step_,
                          event ? int(event->type()) : int(QEvent::None),
                          event && event->spontaneous(),
                          button_.isEnabled(),
                          button_.isVisible(),
                          button_.hasFocus(),
                          menu_.isVisible(),
                          reinterpret_cast<quintptr>(receiver),
                          reinterpret_cast<quintptr>(QApplication::activeWindow()),
                          reinterpret_cast<quintptr>(QApplication::activePopupWidget()),
                          reinterpret_cast<quintptr>(QApplication::focusWidget())};
  }
  void finish() {
    record("visibility_sample_after_assertion", &menu_);
    sampled_ = true;
    qApp->removeEventFilter(this);
  }
  QJsonObject receipt() const {
    const auto address = [](quintptr value) { return QString::number(qulonglong(value), 16); };
    QJsonArray events;
    for (int i = 0; i < count_; ++i) {
      const auto &e = entries_[i];
      events.append(QJsonObject{{"sequence", i + 1},
                                {"phase", e.phase},
                                {"ui_step", e.step},
                                {"event_type", e.event_type},
                                {"spontaneous", e.spontaneous},
                                {"button_enabled", e.enabled},
                                {"button_visible", e.visible},
                                {"button_has_focus", e.focus},
                                {"menu_visible", e.menu_visible},
                                {"receiver", address(e.receiver)},
                                {"active_window", address(e.active_window)},
                                {"active_popup", address(e.active_popup)},
                                {"focus_widget", address(e.focus_widget)}});
    }
    return {{"scope", "First UiFile cycle only; pre-handler observation does not prove handler completion"},
            {"identity_scope", "Process-local QObject addresses in hexadecimal"},
            {"capacity", int(entries_.size())},
            {"storage_bytes", qint64(sizeof(entries_))},
            {"truncated", truncated_},
            {"visibility_sample_reached", sampled_},
            {"button", address(reinterpret_cast<quintptr>(&button_))},
            {"menu", address(reinterpret_cast<quintptr>(&menu_))},
            {"window", address(reinterpret_cast<quintptr>(&window_))},
            {"events", events}};
  }
};
} // namespace
// Steps 3-11: Space opens each header menu, which is checked and captured, then Escape closes it.
void Editor::ui_smoke_menu() {
  const auto key = [](QObject *receiver, int k) {
    QCoreApplication::postEvent(receiver, new QKeyEvent(QEvent::KeyPress, k, Qt::NoModifier));
    QCoreApplication::postEvent(receiver, new QKeyEvent(QEvent::KeyRelease, k, Qt::NoModifier));
  };
  const char *menus[] = {"UiFile", "UiEdit", "UiView"};
  const int index = (ui_step_ - 3) / 3, stage = (ui_step_ - 3) % 3;
  auto *b = ui_button(menus[index]);
  auto *menu = b->menu();
  if (stage == 0) {
    if (index == 0 && menu && !ui_menu_trace_)
      ui_menu_trace_ = std::make_unique<UiMenuTrace>(*this, *b, *menu, ui_step_);
    b->setFocus(Qt::ShortcutFocusReason);
    key(b, Qt::Key_Space);
    if (index == 0 && ui_menu_trace_)
      static_cast<UiMenuTrace *>(ui_menu_trace_.get())->record("space_posted", b);
  } else if (stage == 1) {
    smoke_check(QString(menus[index]) + "_space_opens_menu", menu && menu->isVisible());
    if (index == 0 && ui_menu_trace_)
      static_cast<UiMenuTrace *>(ui_menu_trace_.get())->finish();
    if (menu && menu->isVisible()) {
      const auto pixels = menu->grab();
      const auto image = pixels.toImage();
      smoke_check(QString(menus[index]) + "_rounded_popup_transparent_corners",
                  !image.isNull() && image.pixelColor(0, 0).alpha() == 0 &&
                      image.pixelColor(image.width() - 1, 0).alpha() == 0 &&
                      image.pixelColor(0, image.height() - 1).alpha() == 0 &&
                      image.pixelColor(image.width() - 1, image.height() - 1).alpha() == 0 &&
                      image.pixelColor(image.width() / 2, image.height() / 2).alpha() > 0);
      smoke_check(QString(menus[index]) + "_menu_capture",
                  pixels.save(smoke_ + "/" + menus[index] + "-menu.png"));
    }
    if (menu)
      key(menu, Qt::Key_Escape);
  } else {
    // Capturing/saving the popup can queue the next timer before posted input
    // is delivered. Observe visibility only after this menu receives Escape.
    if (menu) {
      QCoreApplication::sendPostedEvents(menu, QEvent::KeyPress);
      QCoreApplication::sendPostedEvents(menu, QEvent::KeyRelease);
    }
    smoke_check(QString(menus[index]) + "_escape_closes_menu", menu && !menu->isVisible());
    if (menu && menu->isVisible())
      menu->close();
  }
  ++ui_step_;
}
// Steps 12-16: exclusive tool actions and the authoring fixture, then header Undo, Redo and Play.
void Editor::ui_smoke_action_step(int step) {
  switch (step) {
  case 12: {
    bool mnemonics = true;
    for (auto *n : {"UiFile", "UiEdit", "UiView"})
      mnemonics =
          mnemonics && ui_button(n)->shortcut() == QKeySequence::mnemonic(ui_button(n)->menu()->title());
    smoke_check("file_edit_view_mnemonics_registered", mnemonics);
    smoke_check("original_undo_play_actions", ui_button("UiUndo")->defaultAction() == undo_action_ &&
                                                  ui_button("UiPlay")->defaultAction() == play_action_);
    bool exclusive = true;
    const char *tools[] = {"UiSelect", "UiMove", "UiRotate", "UiScale"};
    for (int i = 0; i < 4; ++i) {
      ui_trigger(tools[i]);
      for (int j = 0; j < 4; ++j)
        exclusive = exclusive && (ui_button(tools[j])->isChecked() == (i == j));
      exclusive = exclusive && viewport_->tool == i - 1;
    }
    ui_trigger("UiMove");
    smoke_check("transform_tool_actions_exclusive_and_routed", exclusive);
    ui_dirty_before_ = dirty_;
    ui_before_ = ui_scene_json();
    if (rows_.empty() || ui_before_.empty()) {
      smoke_check("authoring_fixture_present", false);
      ui_step_ = 100;
      return;
    }
    select_object(rows_.front().id);
    auto row = *selected_row();
    row.local.translation.x += 0.25;
    smoke_check("authoring_fixture_edit", check(document_->edit(row, document_->generation())));
    changed();
    ui_after_ = ui_scene_json();
    smoke_check("authoring_fixture_changed", !ui_after_.empty() && ui_after_ != ui_before_);
    return;
  }
  case 13:
    ui_trigger("UiUndo");
    smoke_check("header_undo_restores_scene", ui_scene_json() == ui_before_);
    return;
  case 14:
    ui_trigger("UiRedo");
    smoke_check("header_redo_restores_edit", ui_scene_json() == ui_after_);
    return;
  case 15:
    ui_trigger("UiUndo");
    smoke_check("header_undo_cleanup", ui_scene_json() == ui_before_);
    ui_play_generation_ = document_->generation();
    ui_trigger("UiPlay");
    smoke_check("header_play_starts", document_->playing());
    return;
  case 16:
    ui_trigger("UiPlay");
    smoke_check("header_stop_restores_authoring", !document_->playing() &&
                                                      document_->generation() == ui_play_generation_ &&
                                                      ui_scene_json() == ui_before_);
    dirty_ = ui_dirty_before_;
    update_title();
    return;
  }
}
// Steps 17-19: the View menu recovers hidden panels and the header cannot be hidden.
void Editor::ui_smoke_view_menu_step(int step, QToolBar *header) {
  switch (step) {
  case 17: {
    smoke_check("header_cannot_be_hidden_by_toolbar_menu", !header->toggleViewAction()->isEnabled());
    auto *scene_tools = findChild<QToolBar *>("SceneTools");
    auto *hierarchy = findChild<QDockWidget *>("Hierarchy");
    auto *properties = findChild<QDockWidget *>("Properties");
    const auto available = ui_button("UiView")->menu()->actions();
    smoke_check("view_contains_all_panel_recovery_actions",
                available.contains(scene_tools->toggleViewAction()) &&
                    available.contains(hierarchy->toggleViewAction()) &&
                    available.contains(properties->toggleViewAction()));
    scene_tools->hide();
    hierarchy->hide();
    properties->hide();
    return;
  }
  case 18: {
    smoke_check("header_remains_available_with_hidden_panels",
                header->isVisible() && ui_button("UiView")->isVisible());
    findChild<QToolBar *>("SceneTools")->toggleViewAction()->trigger();
    findChild<QDockWidget *>("Hierarchy")->toggleViewAction()->trigger();
    findChild<QDockWidget *>("Properties")->toggleViewAction()->trigger();
    return;
  }
  case 19:
    smoke_check("view_actions_restore_panels", findChild<QToolBar *>("SceneTools")->isVisible() &&
                                                   findChild<QDockWidget *>("Hierarchy")->isVisible() &&
                                                   findChild<QDockWidget *>("Properties")->isVisible());
    return;
  }
}
// Steps 20-24: gizmo drags commit or cancel, then each transform tool's overlay renders and is captured.
void Editor::ui_smoke_gizmo_step(int step) {
  switch (step) {
  case 20: {
    bool passed = true;
    for (int mode = 0; mode < 3; ++mode) {
      viewport_->tool = mode;
      for (int axis = 0; axis < 3; ++axis) {
        const auto tag = QString("gizmo_%1_axis_%2").arg(mode).arg(axis);
        const bool commit = viewport_->smoke_drag(false, false, axis);
        refresh();
        smoke_check(tag + "_commit_one_undo", commit);
        const bool cancel = viewport_->smoke_drag(true, false, axis);
        refresh();
        smoke_check(tag + "_escape_preserves_scene", cancel);
        passed = passed && commit && cancel;
      }
    }
    refresh();
    smoke_check("gizmo_drags_restore_original_scene", passed && ui_scene_json() == ui_before_);
    ui_trigger("UiSelect");
    return;
  }
  case 21:
    smoke_check("selection_tool_has_no_overlay", viewport_->renderer()->stats().overlay_vertices == 0);
    ui_trigger("UiMove");
    return;
  case 22:
    smoke_check("move_arrows_rendered", viewport_->renderer()->stats().overlay_draws == 3 &&
                                            viewport_->renderer()->stats().overlay_vertices > 0);
    smoke_check("move_gpu_capture",
                check(viewport_->renderer()->capture(native_path(smoke_ + "/move-gizmo.ppm"))));
    ui_trigger("UiRotate");
    return;
  case 23:
    smoke_check("rotation_rings_rendered", viewport_->renderer()->stats().overlay_draws == 3 &&
                                               viewport_->renderer()->stats().overlay_vertices > 500);
    smoke_check("rotate_gpu_capture",
                check(viewport_->renderer()->capture(native_path(smoke_ + "/rotate-gizmo.ppm"))));
    ui_trigger("UiScale");
    return;
  case 24:
    smoke_check("scale_handles_rendered", viewport_->renderer()->stats().overlay_draws == 3 &&
                                              viewport_->renderer()->stats().overlay_vertices > 0);
    smoke_check("scale_gpu_capture",
                check(viewport_->renderer()->capture(native_path(smoke_ + "/scale-gizmo.ppm"))));
    ui_trigger("UiMove");
    dirty_ = ui_dirty_before_;
    update_title();
    return;
  }
}
// Steps 25-27: viewport tool input, no-op and seam-crossing drags, and the six axis views.
void Editor::ui_smoke_viewport_step(int step) {
  switch (step) {
  case 25:
    smoke_check("viewport_tools_selection_navigation_and_cancel", viewport_->smoke_tool_input());
    refresh();
    dirty_ = ui_dirty_before_;
    update_title();
    return;
  case 26: {
    const auto dirty = dirty_;
    smoke_check("gizmo_out_and_back_preserves_history_and_dirty",
                viewport_->smoke_noop_drag() && dirty_ == dirty);
    smoke_check("rotation_crosses_seam_and_multiple_quadrants", viewport_->smoke_rotation_turns());
    refresh();
    return;
  }
  case 27: {
    const auto before = ui_scene_json();
    const auto dirty = dirty_;
    const auto camera = viewport_->camera_pose();
    for (int view = 0; view < 6; ++view)
      for (int mode = 0; mode < 3; ++mode) {
        const auto tag = QString("axis_view_%1_mode_%2").arg(view).arg(mode);
        const auto image = view % 2 == 0 ? native_path(smoke_ + "/" + tag + ".ppm") : std::filesystem::path{};
        smoke_check(tag + "_axis_alignment_commit_cancel_undo_and_render",
                    viewport_->smoke_axis_view(view, mode, [this] { refresh(); }, image));
      }
    smoke_check("axis_views_restore_scene_and_camera",
                ui_scene_json() == before && viewport_->camera_pose() == camera);
    refresh();
    dirty_ = dirty;
    update_title();
    return;
  }
  }
}
void Editor::ui_smoke_finish() {
  bool passed = true;
  for (const auto &entry : smoke_checks_) {
    const auto o = entry.toObject();
    passed = passed && !o.contains("error") && o.value("passed").toBool();
  }
  QJsonObject receipt{
      {"schema", 1},
      {"application", "RACE Qt editor UI"},
      {"passed", passed},
      {"checks", smoke_checks_},
      {"layouts", ui_layouts_},
      {"source_commit", QString::fromUtf8(build::source_commit)},
      {"authored_input_sha256", QString::fromUtf8(build::authored_input_sha256)},
      {"scope",
       "Qt layout and synthetic Qt keyboard/menu/action checks; PNGs omit native child viewport; no physical input or GPU image parity claim"}};
  if (viewport_->renderer()) {
    const auto stats = viewport_->renderer()->stats();
    receipt["adapter"] = QString::fromStdString(stats.adapter);
    receipt["hardware"] = stats.hardware;
    receipt["debug_layer"] = stats.debug_layer;
    receipt["debug_errors"] = qint64(stats.debug_errors);
    receipt["debug_warnings"] = qint64(stats.debug_warnings);
    passed = passed && stats.hardware && stats.debug_layer && !stats.debug_errors && !stats.debug_warnings;
    receipt["passed"] = passed;
  }
  if (ui_menu_trace_)
    receipt["file_menu_trace"] = static_cast<UiMenuTrace *>(ui_menu_trace_.get())->receipt();
  QSaveFile file(smoke_ + "/ui-receipt.json");
  const auto bytes = QJsonDocument(receipt).toJson();
  const bool written = file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
  closing_smoke_ = true;
  close();
  QCoreApplication::exit(passed && written ? 0 : 2);
}
} // namespace race_editor
