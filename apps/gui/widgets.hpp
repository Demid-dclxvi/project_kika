#pragma once
// Общие мелочи окна: перевод строк, числа в полях ввода, кнопки, поля, списки — для главного окна
// и диалогов (настройки печати, принтер).

#include <QComboBox>
#include <QEnterEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPolygonF>
#include <QPushButton>
#include <QSlider>
#include <QStyle>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kika/app/scene.hpp"

namespace kika::gui {

inline QString qs(const std::string& s) { return QString::fromStdString(s); }
inline std::string ss(const QString& s) { return s.toStdString(); }
inline QString esc(const std::string& s) { return qs(s).toHtmlEscaped(); }
inline QString fmtq(double v, int d = -1) { return qs(app::fmt(v, d)); }

inline std::filesystem::path to_path(const QString& s) {
#ifdef _WIN32
  return std::filesystem::path(s.toStdWString());
#else
  return std::filesystem::path(s.toStdString());
#endif
}

inline QString to_qpath(const std::filesystem::path& p) {
#ifdef _WIN32
  return QString::fromStdWString(p.wstring());
#else
  return QString::fromStdString(p.string());
#endif
}

// Число из поля ввода: запятая или точка, пробелы между разрядами допустимы.
inline std::optional<double> parse_num(const QString& text) {
  QString t = text.trimmed();
  t.remove(QChar(' '));
  t.remove(QChar(0x00A0));
  t.remove(QChar(0x202F));
  t.replace(QChar(','), QChar('.'));
  t.replace(QChar(0x2212), QChar('-'));  // «−» из подписей
  if (t.isEmpty()) return std::nullopt;
  bool ok = false;
  const double v = t.toDouble(&ok);
  if (!ok || !std::isfinite(v)) return std::nullopt;
  return v;
}

inline QString num_text(double v) { return QString::number(v, 'g', 12).replace(QChar('.'), QChar(',')); }

inline QString ru_int(long long n) { return QLocale(QLocale::Russian).toString(n); }

inline void repolish(QWidget* w) {
  w->style()->unpolish(w);
  w->style()->polish(w);
  w->update();
}

inline void set_bad(QWidget* w, bool bad) {
  if (w->property("bad").toBool() == bad) return;
  w->setProperty("bad", bad);
  repolish(w);
}

// Удалить всё из раскладки. Виджеты удаляются позже (может идти обработка их же сигнала),
// а сигналы глушатся сразу, чтобы скрытие не вызвало повторной обработки.
inline void clear_layout(QLayout* l) {
  while (QLayoutItem* it = l->takeAt(0)) {
    if (QWidget* w = it->widget()) {
      w->blockSignals(true);
      for (QObject* c : w->findChildren<QObject*>()) c->blockSignals(true);
      w->hide();
      w->deleteLater();
      delete it;
    } else if (QLayout* sub = it->layout()) {
      clear_layout(sub);
      delete sub;
    } else {
      delete it;
    }
  }
}

// Колесо мыши не меняет значение, пока поле не в фокусе: иначе прокрутка панели
// случайно переключает материал или направление.
template <class W>
class Calm : public W {
 public:
  explicit Calm(QWidget* parent = nullptr) : W(parent) { this->setFocusPolicy(Qt::StrongFocus); }

 protected:
  void wheelEvent(QWheelEvent* e) override {
    if (this->hasFocus())
      W::wheelEvent(e);
    else
      e->ignore();
  }
};

// Список, который сжимается вместе с панелью (иначе его ширина — по самому длинному пункту).
// Стрелка рисуется здесь: таблица стилей задаёт рамку списка, а стрелку без картинки задать нельзя.
class Combo : public Calm<QComboBox> {
 public:
  Combo() {
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(4);
  }

 protected:
  void paintEvent(QPaintEvent* e) override {
    Calm<QComboBox>::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(isEnabled() ? QColor(0x61, 0x64, 0x6b) : QColor(0xb9, 0xba, 0xb6), 1.6, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    const QPointF c(width() - 12.5, height() / 2.0 + 0.5);
    p.drawPolyline(QPolygonF({c + QPointF(-4, -2), c + QPointF(0, 2), c + QPointF(4, -2)}));
  }
};

class Slider : public Calm<QSlider> {
 public:
  Slider(int lo, int hi, int value) {
    setOrientation(Qt::Horizontal);
    setRange(lo, hi);
    setValue(value);
  }
};

// Рамка с обратными вызовами наведения и щелчка.
class Frame : public QFrame {
 public:
  explicit Frame(const char* name) { setObjectName(name); }
  std::function<void(bool)> on_hover;
  std::function<void()> on_click;

 protected:
  void enterEvent(QEnterEvent* e) override {
    if (on_hover) on_hover(true);
    QFrame::enterEvent(e);
  }
  void leaveEvent(QEvent* e) override {
    if (on_hover) on_hover(false);
    QFrame::leaveEvent(e);
  }
  void mousePressEvent(QMouseEvent* e) override {
    QFrame::mousePressEvent(e);
    if (on_click && e->button() == Qt::LeftButton) on_click();
  }
};

// Подпись в одну строку, которая не помещается, — с многоточием (полный текст — в подсказке).
class ElideLabel : public QLabel {
 public:
  explicit ElideLabel(const QString& text) : QLabel(text) {
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setToolTip(text);
  }

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setPen(palette().color(QPalette::WindowText));
    p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fontMetrics().elidedText(text(), Qt::ElideRight, width()));
  }
};

inline QLabel* make_label(const QString& text, const char* name = nullptr, bool wrap = true) {
  auto* l = new QLabel(text);
  if (name) l->setObjectName(name);
  l->setWordWrap(wrap);
  return l;
}

inline QLabel* make_hint(const QString& text) { return make_label(text, "hint"); }
inline QLabel* make_warn(const QString& text) { return make_label(text, "warn"); }

inline QPushButton* make_button(const QString& text, const char* cls = nullptr) {
  auto* b = new QPushButton(text);
  if (cls) b->setProperty("cls", cls);
  b->setCursor(Qt::PointingHandCursor);
  return b;
}

inline QPushButton* make_icon_button(const QString& text, const QString& tip) {
  auto* b = make_button(text, "icon");
  b->setToolTip(tip);
  b->setFocusPolicy(Qt::TabFocus);
  return b;
}

inline QLabel* make_dot(const app::Rgb& c) {
  auto* d = new QLabel;
  d->setFixedSize(10, 10);
  d->setStyleSheet(QString("background:%1;border-radius:3px;").arg(QColor::fromRgbF(c[0], c[1], c[2]).name()));
  return d;
}

// Поле: подпись сверху, ввод снизу.
inline QWidget* make_field(const QString& title, QWidget* input) {
  auto* w = new QWidget;
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  v->addWidget(make_label(title, "fieldLabel"));
  v->addWidget(input);
  return w;
}

inline QLineEdit* make_num(std::optional<double> v) {
  auto* e = new QLineEdit(v ? num_text(*v) : QString());
  e->setObjectName("num");
  e->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  return e;
}

inline Combo* make_combo(const std::vector<std::pair<const char*, const char*>>& items, const std::string& cur) {
  auto* c = new Combo;
  for (const auto& [key, name] : items) c->addItem(QString::fromUtf8(name), QString::fromUtf8(key));
  const int i = c->findData(qs(cur));
  if (i >= 0) c->setCurrentIndex(i);
  return c;
}

inline std::string combo_key(const QComboBox* c) { return ss(c->currentData().toString()); }

inline QFrame* make_step() {
  auto* f = new QFrame;
  f->setObjectName("step");
  auto* v = new QVBoxLayout(f);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(10);
  return f;
}

inline QWidget* make_step_title(const QString& n, const QString& text) {
  auto* w = new QWidget;
  auto* h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(9);
  if (!n.isEmpty()) {
    auto* num = new QLabel(n);
    num->setObjectName("stepNum");
    num->setAlignment(Qt::AlignCenter);
    num->setFixedSize(20, 20);
    h->addWidget(num);
  }
  h->addWidget(make_label(text, "stepTitle", false));
  h->addStretch();
  return w;
}

inline QHBoxLayout* hbox(int spacing = 8) {
  auto* h = new QHBoxLayout;
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(spacing);
  return h;
}

inline QWidget* wrap_layout(QLayout* l) {
  auto* w = new QWidget;
  w->setLayout(l);
  return w;
}

// Сегментированная группа кнопок.
inline QFrame* make_seg(QHBoxLayout** layout) {
  auto* f = new QFrame;
  f->setObjectName("seg");
  auto* h = new QHBoxLayout(f);
  h->setContentsMargins(3, 3, 3, 3);
  h->setSpacing(2);
  *layout = h;
  return f;
}

inline QPushButton* make_seg_button(const QString& text) {
  auto* b = make_button(text, "seg");
  b->setCheckable(true);
  return b;
}


}  // namespace kika::gui
