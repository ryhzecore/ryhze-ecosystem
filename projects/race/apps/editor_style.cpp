#include "editor_style.hpp"
#include "editor_style_sheet.hpp"
#include <QAction>
#include <QAbstractItemView>
#include <QApplication>
#include <QComboBox>
#include <QDialog>
#include <QEvent>
#include <QFont>
#include <QFontDatabase>
#include <QFrame>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QPen>
#include <QPixmap>
#include <QPolygonF>
#include <QProxyStyle>
#include <QStyleFactory>
#include <QSizePolicy>
#include <QThread>
#include <QToolButton>
#include <QVariant>
#include <QVBoxLayout>
#include <QWidget>
#include <array>
#include <stdexcept>
#include <memory>

namespace race::desktop::ui {
namespace {
// Reserve the same compact extent that the toolbar overflow control paints.
// QToolBarLayout reads this metric directly; a wider QSS minimum alone overlaps.
class EditorStyle final : public QProxyStyle {
public:
  explicit EditorStyle(QStyle *base) : QProxyStyle(base) {}
  int pixelMetric(PixelMetric metric, const QStyleOption *option = nullptr,
                  const QWidget *widget = nullptr) const override {
    if (metric == PM_ToolBarExtensionExtent)
      return ShapeTokens::compact_height;
    return QProxyStyle::pixelMetric(metric, option, widget);
  }
  int styleHint(StyleHint hint, const QStyleOption *option = nullptr, const QWidget *widget = nullptr,
                QStyleHintReturn *result = nullptr) const override {
    // Fusion's menu delegate bypasses the rounded item-view rules below. Use
    // Qt's styled list delegate for every combo, retaining its normal input path.
    if (hint == SH_ComboBox_Popup)
      return 0;
    return QProxyStyle::styleHint(hint, option, widget, result);
  }
};
// QSS rounds its own paint, not an opaque native popup backing window. Prepare
// only popup surfaces at polish, before showing them; no frame work or masks.
class PopupPolish final : public QObject {
public:
  explicit PopupPolish(QApplication &app) : QObject(&app) {
    setObjectName(QStringLiteral("RACEPopupPolish"));
    app.installEventFilter(this);
  }

protected:
  bool eventFilter(QObject *watched, QEvent *event) override {
    if (event->type() != QEvent::Polish)
      return false;
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget)
      return false;
    if (qobject_cast<QMenu *>(widget) || widget->windowType() == Qt::ToolTip) {
      prepare(widget);
      return false;
    }
    auto *popup = widget->window();
    if (popup->windowType() != Qt::Popup)
      return false;
    for (auto *parent = popup->parentWidget(); parent; parent = parent->parentWidget()) {
      if (!qobject_cast<QComboBox *>(parent))
        continue;
      prepare(popup);
      if (auto *frame = qobject_cast<QFrame *>(popup))
        frame->setFrameShape(QFrame::NoFrame);
      if (auto *view = qobject_cast<QAbstractItemView *>(widget))
        view->viewport()->setAutoFillBackground(false);
      break;
    }
    return false;
  }

private:
  static void prepare(QWidget *popup) {
    if (!popup->windowFlags().testFlag(Qt::FramelessWindowHint))
      popup->setWindowFlag(Qt::FramelessWindowHint, true);
    popup->setAttribute(Qt::WA_TranslucentBackground, true);
    popup->setAttribute(Qt::WA_OpaquePaintEvent, false);
    popup->setAutoFillBackground(false);
  }
};
void gui_thread() {
  auto *app = qobject_cast<QApplication *>(QCoreApplication::instance());
  if (!app || QThread::currentThread() != app->thread())
    throw std::runtime_error("RACE editor styling requires the QApplication thread");
}
struct Fonts {
  QString body, heading;
};
QString action_label(const QString &text) {
  QString label;
  label.reserve(text.size());
  for (qsizetype n = 0; n < text.size(); ++n) {
    if (text[n] != QLatin1Char('&'))
      label += text[n];
    else if (n + 1 < text.size() && text[n + 1] == QLatin1Char('&')) {
      label += QLatin1Char('&');
      ++n;
    }
  }
  return label;
}
const Fonts &fonts() {
  gui_thread();
  static const Fonts loaded = [] {
    const auto body = QFontDatabase::addApplicationFont(QStringLiteral(":/race/fonts/Inter.ttf"));
    if (body < 0)
      throw std::runtime_error("RACE embedded Inter font could not load");
    const auto body_names = QFontDatabase::applicationFontFamilies(body);
    if (body_names.isEmpty()) {
      QFontDatabase::removeApplicationFont(body);
      throw std::runtime_error("RACE embedded Inter font has no registered family");
    }
    const auto heading = QFontDatabase::addApplicationFont(QStringLiteral(":/race/fonts/SpaceGrotesk.ttf"));
    if (heading < 0) {
      QFontDatabase::removeApplicationFont(body);
      throw std::runtime_error("RACE embedded Space Grotesk font could not load");
    }
    const auto heading_names = QFontDatabase::applicationFontFamilies(heading);
    if (heading_names.isEmpty()) {
      QFontDatabase::removeApplicationFont(heading);
      QFontDatabase::removeApplicationFont(body);
      throw std::runtime_error("RACE embedded Space Grotesk font has no registered family");
    }
    return Fonts{body_names.front(), heading_names.front()};
  }();
  return loaded;
}
QPixmap glyph(Icon kind, const QColor &color) {
  QPixmap pixels(40, 40);
  if (pixels.isNull())
    throw std::runtime_error("RACE editor icon allocation failed");
  pixels.setDevicePixelRatio(2);
  pixels.fill(Qt::transparent);
  QPainter paint(&pixels);
  paint.setRenderHint(QPainter::Antialiasing);
  paint.setPen(QPen(color, 1.8, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  paint.setBrush(Qt::NoBrush);
  switch (kind) {
  case Icon::Undo:
  case Icon::Redo: {
    if (kind == Icon::Redo) {
      paint.translate(20, 0);
      paint.scale(-1, 1);
    }
    QPainterPath path;
    path.moveTo(7, 6);
    path.lineTo(3, 10);
    path.lineTo(7, 14);
    paint.drawPath(path);
    path = QPainterPath{};
    path.moveTo(3, 10);
    path.cubicTo(12, 3, 18, 8, 16, 15);
    paint.drawPath(path);
    break;
  }
  case Icon::Save:
    paint.drawRoundedRect(QRectF(4, 3, 12, 14), 1, 1);
    paint.drawPolyline(QPolygonF{QPointF(7, 3), QPointF(7, 8), QPointF(13, 8), QPointF(13, 3)});
    paint.drawRect(QRectF(7, 12, 6, 5));
    break;
  case Icon::Play: {
    QPainterPath path;
    path.moveTo(7, 4);
    path.lineTo(16, 10);
    path.lineTo(7, 16);
    path.closeSubpath();
    paint.drawPath(path);
    break;
  }
  case Icon::Stop:
    paint.drawRoundedRect(QRectF(5, 5, 10, 10), 1.5, 1.5);
    break;
  case Icon::Menu:
    paint.drawLine(QPointF(3, 7), QPointF(17, 7));
    paint.drawLine(QPointF(3, 13), QPointF(17, 13));
    break;
  case Icon::Plus:
    paint.drawLine(QPointF(4, 10), QPointF(16, 10));
    paint.drawLine(QPointF(10, 4), QPointF(10, 16));
    break;
  case Icon::Close:
    paint.drawLine(QPointF(5, 5), QPointF(15, 15));
    paint.drawLine(QPointF(15, 5), QPointF(5, 15));
    break;
  }
  paint.end();
  return pixels;
}
} // namespace
QString heading_family() { return fonts().heading; }
QIcon icon(Icon kind) {
  gui_thread();
  const auto index = static_cast<std::size_t>(kind);
  if (index >= 8)
    throw std::invalid_argument("Unknown RACE editor icon");
  static const std::array<QIcon, 8> cached = [] {
    std::array<QIcon, 8> icons;
    for (std::size_t n = 0; n < icons.size(); ++n) {
      const auto kind = static_cast<Icon>(n);
      icons[n].addPixmap(glyph(kind, QColor(QStringLiteral("#F7F7F8"))), QIcon::Normal);
      icons[n].addPixmap(glyph(kind, QColor(QStringLiteral("#9FA2AB"))), QIcon::Disabled);
    }
    return icons;
  }();
  return cached[index];
}
QToolButton *button(QWidget *parent, QAction *action, bool circular, bool compact) {
  gui_thread();
  if (!action)
    throw std::invalid_argument("RACE editor button requires an action");
  auto *control = new QToolButton(parent);
  control->setDefaultAction(action);
  control->setAutoRaise(false);
  control->setProperty("circular", circular);
  control->setProperty("compact", compact);
  control->setFocusPolicy(Qt::StrongFocus);
  const int height = compact ? ShapeTokens::compact_height : ShapeTokens::main_height;
  const int icon_size = compact ? 18 : 20;
  control->setIconSize(QSize(icon_size, icon_size));
  control->setFixedHeight(height);
  control->setToolButtonStyle(circular                  ? Qt::ToolButtonIconOnly
                              : action->icon().isNull() ? Qt::ToolButtonTextOnly
                                                        : Qt::ToolButtonTextBesideIcon);
  control->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
  const auto refresh = [control, action] {
    const auto accessible_label = action->property("accessibleLabel").toString();
    const auto label = accessible_label.isEmpty() ? action_label(action->text()) : accessible_label;
    const auto tooltip = action->toolTip();
    control->setAccessibleName(label);
    control->setToolTip(tooltip.isEmpty() || tooltip == action->text() ? label : tooltip);
  };
  refresh();
  QObject::connect(action, &QAction::changed, control, refresh);
  if (circular)
    control->setFixedSize(height, height);
  return control;
}
QVBoxLayout *dialog_layout(QDialog &dialog) {
  gui_thread();
  auto *outer = new QVBoxLayout(&dialog);
  outer->setContentsMargins(8, 8, 8, 8);
  outer->setSizeConstraint(QLayout::SetMinimumSize);
  auto *surface = new QFrame(&dialog);
  surface->setObjectName(QStringLiteral("DialogSurface"));
  surface->setFrameShape(QFrame::NoFrame);
  surface->setAutoFillBackground(false);
  outer->addWidget(surface);
  // Child backplates stay inside the curved edge; QSS does not clip children.
  auto *body = new QVBoxLayout(surface);
  body->setContentsMargins(16, 16, 16, 16);
  body->setSizeConstraint(QLayout::SetMinimumSize);
  body->setSpacing(12);
  return body;
}
void apply(QApplication &app) {
  gui_thread();
  const auto &registered = fonts();
  if (!app.findChild<QObject *>(QStringLiteral("RACEPopupPolish"), Qt::FindDirectChildrenOnly))
    new PopupPolish(app);
  auto fusion = std::unique_ptr<QStyle>(QStyleFactory::create(QStringLiteral("Fusion")));
  if (!fusion)
    throw std::runtime_error("RACE editor Fusion style is unavailable");
  auto *style = new EditorStyle(fusion.get());
  fusion.release();
  app.setStyle(style);
  QPalette palette;
  palette.setColor(QPalette::Window, QColor(QStringLiteral("#090A0C")));
  palette.setColor(QPalette::WindowText, QColor(QStringLiteral("#F7F7F8")));
  palette.setColor(QPalette::Base, QColor(QStringLiteral("#111214")));
  palette.setColor(QPalette::AlternateBase, QColor(QStringLiteral("#18191C")));
  palette.setColor(QPalette::Text, QColor(QStringLiteral("#F7F7F8")));
  palette.setColor(QPalette::Button, QColor(QStringLiteral("#111214")));
  palette.setColor(QPalette::ButtonText, QColor(QStringLiteral("#F7F7F8")));
  palette.setColor(QPalette::Highlight, QColor(QStringLiteral("#165BFA")));
  palette.setColor(QPalette::HighlightedText, QColor(QStringLiteral("#F7F7F8")));
  palette.setColor(QPalette::Link, QColor(QStringLiteral("#84B6FF")));
  palette.setColor(QPalette::ToolTipBase, QColor(QStringLiteral("#18191C")));
  palette.setColor(QPalette::ToolTipText, QColor(QStringLiteral("#F7F7F8")));
  palette.setColor(QPalette::Disabled, QPalette::Text, QColor(QStringLiteral("#9FA2AB")));
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, QColor(QStringLiteral("#9FA2AB")));
  app.setPalette(palette);
  QFont body(registered.body);
  body.setPixelSize(13);
  app.setFont(body);
  auto sheet = style_sheet(registered.body, registered.heading);
  app.setStyleSheet(sheet);
}
} // namespace race::desktop::ui
