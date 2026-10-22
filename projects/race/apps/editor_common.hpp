#pragma once
// Shared declarations for the editor sources: includes, small helpers and widgets.
#include "editor_startup_measurement.hpp"
#include "desktop_scene.hpp"
#include "desktop_gizmo.hpp"
#include "desktop_meshes.hpp"
#include "desktop_package.hpp"
#include "desktop_profile.hpp"
#include "desktop_persistence.hpp"
#include "desktop_material_assignment.hpp"
#include "desktop_mesh_checkpoint.hpp"
#include "desktop_mesh_reimport.hpp"
#include "editor_reimport_worker.hpp"
#include "desktop_project_browser.hpp"
#include "desktop_surface_reimport.hpp"
#include "desktop_surface_checkpoint.hpp"
#include "desktop_project_authoring.hpp"
#include "desktop_navigation.hpp"
#include "render/windows_viewport.hpp"
#include "editor_style.hpp"
#include "editor_content_browser.hpp"
#include "race/build_identity.hpp"
#include <QApplication>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QActionGroup>
#include <QCloseEvent>
#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QDialogButtonBox>
#include <QPushButton>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QItemSelectionModel>
#include <QToolButton>
#include <QFontInfo>
#include <QMenu>
#include <QFocusEvent>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPlatformSurfaceEvent>
#include <QPlainTextEdit>
#include <QSaveFile>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSignalBlocker>
#include <QStatusBar>
#include <QTimer>
#include <QTabBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWindow>
#include <QUuid>
#include <algorithm>
#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <functional>
#include <numbers>
#include <limits>

namespace race_editor {
using namespace race;
using Vec = scene::Vec3;
using Clock = std::chrono::steady_clock;
inline double milliseconds(Clock::duration d) { return std::chrono::duration<double, std::milli>(d).count(); }
inline constexpr double pi = std::numbers::pi;
inline Vec add(Vec a, Vec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec sub(Vec a, Vec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec mul(Vec a, double s) { return {a.x * s, a.y * s, a.z * s}; }
inline double dot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec cross(Vec a, Vec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline Vec unit(Vec a) { return mul(a, 1 / std::sqrt(dot(a, a))); }
inline scene::Quat multiply(scene::Quat a, scene::Quat b) {
  return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
          a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline scene::Quat euler(Vec degrees) {
  auto x = scene::axis_angle({1, 0, 0}, degrees.x * pi / 180).value();
  auto y = scene::axis_angle({0, 1, 0}, degrees.y * pi / 180).value();
  auto z = scene::axis_angle({0, 0, 1}, degrees.z * pi / 180).value();
  return multiply(multiply(z, y), x);
}
inline Vec angles(scene::Quat q) {
  return {std::atan2(2 * (q.w * q.x + q.y * q.z), 1 - 2 * (q.x * q.x + q.y * q.y)) * 180 / pi,
          std::asin(std::clamp(2 * (q.w * q.y - q.z * q.x), -1.0, 1.0)) * 180 / pi,
          std::atan2(2 * (q.w * q.z + q.x * q.y), 1 - 2 * (q.y * q.y + q.z * q.z)) * 180 / pi};
}
inline QString label(PersistentId id) { return QString("Box %1").arg(id.low); }
inline std::string path_string(const QString &path) { return path.toUtf8().toStdString(); }
inline std::filesystem::path native_path(const QString &path) {
  return std::filesystem::path(path.toStdWString());
}

// Preserve QDockWidget close/float behavior behind a circular panel menu.
inline QWidget *dock_title(QDockWidget *dock) {
  auto *title = new QWidget(dock);
  title->setObjectName("DockHeader");
  auto *layout = new QHBoxLayout(title);
  layout->setContentsMargins(12, 6, 8, 6);
  layout->setSpacing(8);
  title->setFixedHeight(42);
  auto *label = new QLabel(dock->windowTitle(), title);
  label->setObjectName("SectionTitle");
  layout->addWidget(label);
  layout->addStretch();
  auto *action = new QAction("Panel actions", title);
  action->setIcon(desktop::ui::icon(desktop::ui::Icon::Menu));
  auto *button = desktop::ui::button(title, action, true, true);
  button->setAccessibleName(dock->windowTitle() + " panel actions");
  auto *menu = new QMenu(button);
  auto *floating = menu->addAction("Float panel");
  floating->setCheckable(true);
  QObject::connect(floating, &QAction::triggered, dock, [dock](bool value) { dock->setFloating(value); });
  QObject::connect(dock, &QDockWidget::topLevelChanged, floating, &QAction::setChecked);
  menu->addAction(dock->toggleViewAction());
  button->setMenu(menu);
  button->setPopupMode(QToolButton::InstantPopup);
  layout->addWidget(button);
  return title;
}

// Qt normally interprets/corrects text before its focus-out editingFinished
// signal. Preserve the user's edit until Enter or an owner-admitted operation;
// a menu/dialog focus transfer must not publish or normalize it first.
class InspectorNumber final : public QDoubleSpinBox {
public:
  std::function<void()> on_invalid_commit;

protected:
  void focusOutEvent(QFocusEvent *event) override {
    auto *edit = lineEdit();
    const auto text = edit->text();
    const auto number = value();
    const auto cursor = edit->cursorPosition(), start = edit->selectionStart();
    const auto length = edit->selectedText().size();
    QSignalBlocker blocked(this), edit_blocked(edit);
    QDoubleSpinBox::focusOutEvent(event);
    setValue(number);
    edit->setText(text);
    if (start >= 0) {
      if (cursor == start)
        edit->setSelection(start + int(length), -int(length));
      else
        edit->setSelection(start, int(length));
    } else
      edit->setCursorPosition(cursor);
  }
  void keyPressEvent(QKeyEvent *event) override {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) && !hasAcceptableInput()) {
      if (on_invalid_commit)
        on_invalid_commit();
      event->accept();
      return;
    }
    QDoubleSpinBox::keyPressEvent(event);
  }
};
} // namespace race_editor
