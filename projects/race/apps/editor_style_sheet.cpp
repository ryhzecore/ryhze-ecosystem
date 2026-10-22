#include "editor_style_sheet.hpp"
#include "editor_style.hpp"

namespace race::desktop::ui {
namespace {
constexpr char sheet_template[] = R"css(
QWidget { color:#F7F7F8; font-family:"%1"; font-size:13px; }
QMainWindow, QDialog { background:#090A0C; }
QFrame#DialogSurface { background:#111214; border:1px solid #303238; border-radius:@surface@; }
QWidget#Surface { background:#111214; border:0; border-radius:@flat@; }
QWidget#DockHeader, QWidget#PanelHeader { background:#111214; border:0; border-bottom:1px solid #303238; border-radius:@flat@; }
QWidget#HeaderBrand { font-family:"%2"; font-size:26px; font-weight:500; color:#F7F7F8; }
QWidget#Eyebrow { font-size:11px; font-weight:500; color:#84B6FF; }
QWidget#SectionTitle { font-family:"%2"; font-size:16px; font-weight:500; color:#F7F7F8; }
QWidget#PropertyLabel { color:#BFC1C7; font-size:11px; }
QWidget#AxisLabel { color:#BFC1C7; font-size:11px; }
QWidget#AxisLabel[axis="0"] { color:#F48891; }
QWidget#AxisLabel[axis="1"] { color:#86CBA1; }
QWidget#AxisLabel[axis="2"] { color:#84B6FF; }
QWidget#Subtle { color:#9FA2AB; font-size:11px; }
QToolBar { background:#090A0C; border:0; spacing:6px; padding:6px 8px; }
QToolBar#SceneTools { background:#111214; border-bottom:1px solid #303238; spacing:4px; padding:4px 8px; }
QToolBar::separator { background:#303238; width:1px; margin:4px 6px; }
QWidget#PanelActions { background:#111214; border:0; }
QToolButton#qt_toolbar_ext_button { min-width:@compact-content-height@; max-width:@compact-content-height@; min-height:@compact-content-height@; max-height:@compact-content-height@; padding:0; border:1px solid #303238; border-radius:@compact-pill@; }
QToolButton, QPushButton { background:#111214; border:1px solid #303238; border-radius:@pill@; min-height:@main-content-height@; padding:0 14px; }
QToolButton[compact="true"], QPushButton[compact="true"] { min-height:@compact-content-height@; padding:0 10px; border-radius:@compact-pill@; font-size:12px; }
QToolButton:hover, QPushButton:hover { background:#1B1C20; border-color:#555860; }
QToolButton:pressed, QPushButton:pressed { background:#18191C; border-color:#84B6FF; }
QToolButton:focus, QPushButton:focus { border-color:#84B6FF; }
QToolButton:checked, QPushButton:checked, QToolButton[primary="true"], QPushButton[primary="true"] { background:#165BFA; color:#F7F7F8; }
QToolButton[primary="true"]:hover, QPushButton[primary="true"]:hover { background:#165BFA; border-color:#555860; }
QToolButton[primary="true"]:focus, QPushButton[primary="true"]:focus { border-color:#84B6FF; }
QToolButton:disabled, QPushButton:disabled { color:#9FA2AB; }
QToolButton[circular="true"] { min-width:@main-content-height@; max-width:@main-content-height@; min-height:@main-content-height@; max-height:@main-content-height@; padding:0; border-radius:@pill@; }
QToolButton[circular="true"][compact="true"] { min-width:@compact-content-height@; max-width:@compact-content-height@; min-height:@compact-content-height@; max-height:@compact-content-height@; border-radius:@compact-pill@; }
QToolButton::menu-indicator { subcontrol-position:right center; right:5px; }
QToolButton[circular="true"]::menu-indicator { image:none; width:0; }
QWidget#SegmentedTools { background:#18191C; border:1px solid #303238; border-radius:@pill@; }
QWidget#SegmentedTools QToolButton { background:transparent; border:1px solid transparent; border-radius:@compact-pill@; padding:0 10px; }
QWidget#SegmentedTools QToolButton:hover { background:#1B1C20; border-color:#555860; }
QWidget#SegmentedTools QToolButton:pressed { background:#111214; border-color:#84B6FF; }
QWidget#SegmentedTools QToolButton:checked { background:#165BFA; color:#F7F7F8; }
QWidget#SegmentedTools QToolButton:focus { border-color:#84B6FF; }
QLineEdit, QDoubleSpinBox, QSpinBox, QComboBox { background:#18191C; border:1px solid #303238; border-radius:@field@; min-width:0; min-height:@field-content-height@; max-height:@field-content-height@; padding:0 10px; selection-background-color:#165BFA; selection-color:#F7F7F8; }
QDialog QLineEdit, QDialog QComboBox { border-radius:@compact-pill@; min-height:@compact-content-height@; max-height:@compact-content-height@; }
QPlainTextEdit { background:#18191C; border:1px solid #303238; border-radius:@field@; min-width:0; min-height:@field-content-height@; padding:6px 10px; selection-background-color:#165BFA; selection-color:#F7F7F8; }
QPlainTextEdit > QWidget#qt_scrollarea_viewport { background:transparent; border:0; }
QLineEdit:focus, QPlainTextEdit:focus, QDoubleSpinBox:focus, QSpinBox:focus, QComboBox:focus { border-color:#84B6FF; }
QLineEdit:disabled, QDoubleSpinBox:disabled, QSpinBox:disabled, QComboBox:disabled { color:#9FA2AB; }
QDoubleSpinBox QLineEdit, QSpinBox QLineEdit, QComboBox QLineEdit { background:transparent; border:0; border-radius:@flat@; min-width:0; min-height:0; padding:0; }
QWidget#AxisField { background:#18191C; border:1px solid #303238; border-radius:@axis@; }
QWidget#AxisField QDoubleSpinBox { background:transparent; border-color:transparent; border-radius:@inner-field@; padding:0 3px; }
QWidget#AxisField QDoubleSpinBox:focus { border-color:#84B6FF; }
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button, QSpinBox::up-button, QSpinBox::down-button { width:0; height:0; border:0; }
QComboBox::drop-down { border:0; width:22px; border-top-right-radius:@field@; border-bottom-right-radius:@field@; }
QComboBox QAbstractItemView { background:#18191C; border:1px solid #303238; border-radius:@popup@; padding:@popup-padding@; outline:0; }
QComboBox QAbstractItemView::item { min-height:18px; padding:5px 10px; border:1px solid transparent; border-radius:@item@; }
QComboBox QAbstractItemView::item:hover { background:#1B1C20; }
QComboBox QAbstractItemView::item:selected { background:#165BFA; color:#F7F7F8; }
QAbstractItemView QWidget#qt_scrollarea_viewport { background:transparent; border:0; }
QTreeWidget, QListWidget { background:#111214; alternate-background-color:#18191C; border:0; border-radius:@flat@; padding:4px; outline:0; }
QTreeWidget::item, QListWidget::item { min-height:18px; padding:4px 8px; border:1px solid transparent; border-radius:@field@; }
QTreeWidget::item:hover, QListWidget::item:hover { background:#1B1C20; }
QTreeWidget::item:selected, QListWidget::item:selected { background:#165BFA; color:#F7F7F8; }
QTreeWidget::item:focus, QListWidget::item:focus { border-color:#84B6FF; }
QHeaderView::section { background:#111214; color:#BFC1C7; border:0; border-bottom:1px solid #303238; padding:6px 8px; }
QDockWidget { color:#F7F7F8; background:#111214; border:0; border-radius:@flat@; }
QDockWidget > QWidget { background:#111214; }
QDockWidget::title { background:#111214; border:0; border-bottom:1px solid #303238; border-radius:@flat@; padding:6px 10px; text-align:left; }
QDockWidget::close-button, QDockWidget::float-button { border:0; border-radius:@field@; padding:4px; }
QMainWindow::separator { background:#111214; border-right:1px solid #303238; width:5px; height:5px; }
QMainWindow::separator:hover { background:#84B6FF; }
QMenuBar { background:#090A0C; border:0; spacing:4px; }
QMenuBar::item { padding:8px 12px; background:transparent; border-radius:@compact-pill@; }
QMenuBar::item:selected { background:#1B1C20; }
QMenu { background:#18191C; color:#F7F7F8; border:1px solid #303238; border-radius:@popup@; padding:@popup-padding@; }
QMenu::item { min-height:18px; padding:5px 10px; border:1px solid transparent; border-radius:@item@; }
QMenu::item:selected { background:#1B1C20; border-color:#84B6FF; }
QMenu::item:disabled { color:#9FA2AB; }
QMenu::separator { height:1px; background:#303238; margin:4px 8px; }
QTabBar { background:#111214; qproperty-drawBase:0; }
QTabBar::tab { background:#111214; color:#F7F7F8; border:1px solid #303238; border-radius:@compact-pill@; min-height:@compact-content-height@; padding:0 12px; margin:2px 1px; }
QTabBar::tab:hover { background:#1B1C20; border-color:#555860; }
QTabBar::tab:selected { background:#165BFA; border-color:#84B6FF; }
QTabBar::tab:disabled { color:#9FA2AB; }
QTabBar::tab:selected:focus { border-color:#F7F7F8; }
QStatusBar { background:#090A0C; color:#BFC1C7; border-top:1px solid #303238; padding:4px 8px; }
QStatusBar::item { border:0; }
QScrollArea { background:#111214; border:0; border-radius:@flat@; }
QScrollArea > QWidget, QScrollArea > QWidget > QWidget { background:#111214; border:0; }
QScrollBar:vertical { background:#090A0C; width:@scrollbar-width@; margin:0; }
QScrollBar:horizontal { background:#090A0C; height:@scrollbar-width@; margin:0; }
QScrollBar::handle { background:#303238; border-radius:@scrollbar-radius@; min-width:24px; min-height:24px; }
QScrollBar::add-line, QScrollBar::sub-line { width:0; height:0; }
QScrollBar::add-page, QScrollBar::sub-page { background:transparent; }
QToolTip { background:#18191C; color:#F7F7F8; border:1px solid #303238; border-radius:@field@; padding:6px 8px; }
)css";
} // namespace

QString style_sheet(const QString &body, const QString &heading) {
  auto sheet = QString::fromUtf8(sheet_template).arg(body, heading);
  const auto token = [&sheet](const char *name, int value) {
    sheet.replace(QString::fromLatin1(name), QString::number(value) + QStringLiteral("px"));
  };
  token("@flat@", ShapeTokens::workspace_radius);
  token("@pill@", ShapeTokens::main_radius);
  token("@compact-pill@", ShapeTokens::compact_radius);
  token("@field@", ShapeTokens::field_radius);
  token("@popup@", ShapeTokens::popup_radius);
  token("@inner-field@", ShapeTokens::inner_field_radius);
  token("@axis@", ShapeTokens::axis_radius);
  token("@item@", ShapeTokens::item_radius);
  token("@surface@", ShapeTokens::surface_radius);
  token("@main-content-height@", ShapeTokens::main_height - 2);
  token("@compact-content-height@", ShapeTokens::compact_height - 2);
  token("@field-content-height@", ShapeTokens::field_height - 2);
  token("@scrollbar-width@", ShapeTokens::scrollbar_width);
  token("@scrollbar-radius@", ShapeTokens::scrollbar_width / 2);
  token("@popup-padding@", ShapeTokens::popup_padding);
  return sheet;
}
} // namespace race::desktop::ui
