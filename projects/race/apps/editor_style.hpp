#pragma once
#include <QIcon>
#include <QString>

class QAction;
class QApplication;
class QDialog;
class QVBoxLayout;
class QToolButton;
class QWidget;

namespace race::desktop::ui {
// Dense editor adaptation of Ryhze's shared rounded controls. Attached panels
// remain one continuous workspace; floating surfaces and insets use paired radii.
struct ShapeTokens final {
  static constexpr int main_height=36;
  static constexpr int compact_height=30;
  static constexpr int field_height=28;
  static constexpr int scrollbar_width=10;
  static constexpr int workspace_radius=0;
  static constexpr int field_radius=field_height/2;
  static constexpr int axis_radius=(field_height+2)/2;
  static constexpr int item_radius=compact_height/2;
  static constexpr int popup_radius=item_radius+7;
  static constexpr int surface_radius=main_height/2+2;
  static constexpr int main_radius=main_height/2;
  static constexpr int compact_radius=compact_height/2;
  static constexpr int inner_field_radius=axis_radius-1;
  static constexpr int popup_padding=popup_radius-item_radius-1;
};
enum class Icon { Undo, Redo, Save, Play, Stop, Menu, Plus, Close };
// QApplication's GUI thread only. Missing embedded fonts fail startup visibly.
void apply(QApplication&);
// One rounded inset surface for an app-authored modal, preserving its native
// title bar, resize/close behavior and attached workspace outside the dialog.
QVBoxLayout* dialog_layout(QDialog&);
QIcon icon(Icon);
QToolButton* button(QWidget* parent,QAction* action,bool circular=false,bool compact=false);
QString heading_family();
} // namespace race::desktop::ui
