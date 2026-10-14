#pragma once
// Вертикальный ползунок с двумя ручками — диапазон слоёв предпросмотра (нижний и верхний слой),
// как в OrcaSlicer. Колесо и стрелки двигают верхнюю ручку, с Shift — нижнюю. Без moc.

#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <functional>
#include <tuple>

namespace kika::gui {

class RangeSlider : public QWidget {
 public:
  explicit RangeSlider(QWidget* parent = nullptr) : QWidget(parent) {
    setMinimumWidth(78);
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
  }

  void set_count(int n) {
    n_ = std::max(0, n);
    lo_ = 0;
    hi_ = std::max(0, n_ - 1);
    update();
  }
  int count() const { return n_; }
  int lower() const { return lo_; }
  int upper() const { return hi_; }
  void set_range(int lo, int hi) {
    if (n_ == 0) return;
    hi = std::clamp(hi, 0, n_ - 1);
    lo = std::clamp(lo, 0, hi);
    if (lo == lo_ && hi == hi_) return;
    lo_ = lo;
    hi_ = hi;
    update();
    if (on_change) on_change(lo_, hi_);
  }
  // Подпись у ручки: номер слоя и высота.
  std::function<QString(int)> label;
  std::function<void(int, int)> on_change;

 protected:
  void paintEvent(QPaintEvent*) override {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QColor line(0xdf, 0xe0, 0xda), dark(0x16, 0x18, 0x1b), muted(0x61, 0x64, 0x6b);
    const int x = track_x();
    p.setPen(Qt::NoPen);
    p.setBrush(line);
    p.drawRoundedRect(QRectF(x - 2, top(), 4, bottom() - top()), 2, 2);
    if (n_ == 0) return;
    const double ylo = y_of(lo_), yhi = y_of(hi_);
    p.setBrush(dark);
    p.drawRoundedRect(QRectF(x - 2, yhi, 4, ylo - yhi), 2, 2);
    QFont f = font();
    f.setPixelSize(11);
    p.setFont(f);
    for (const auto& [v, y, active] : {std::tuple{hi_, yhi, drag_ != 1}, std::tuple{lo_, ylo, drag_ == 1}}) {
      p.setPen(QPen(QColor(255, 255, 255), 2));
      p.setBrush(dark);
      p.drawEllipse(QPointF(x, y), 7, 7);
      const QString t = label ? label(v) : QString::number(v + 1);
      const QFontMetrics fm(f);
      const int w = fm.horizontalAdvance(t) + 10;
      const QRectF box(std::max(2.0, x - 10.0 - w), y - 9, w, 18);
      p.setPen(Qt::NoPen);
      p.setBrush(active ? dark : QColor(255, 255, 255, 230));
      p.drawRoundedRect(box, 4, 4);
      p.setPen(active ? QColor(255, 255, 255) : muted);
      p.drawText(box, Qt::AlignCenter, t);
      if (lo_ == hi_) break;
    }
  }

  void mousePressEvent(QMouseEvent* e) override {
    if (n_ == 0) return;
    const double y = e->position().y();
    // ближе к какой ручке; одинаково близко (ручки вместе) — сверху верхняя
    const double dl = std::abs(y - y_of(lo_)), dh = std::abs(y - y_of(hi_));
    drag_ = (dl < dh || (dl == dh && y > y_of(hi_))) ? 1 : 2;
    move_to(y);
  }
  void mouseMoveEvent(QMouseEvent* e) override {
    if (drag_) move_to(e->position().y());
  }
  void mouseReleaseEvent(QMouseEvent*) override {
    drag_ = 0;
    update();
  }
  void wheelEvent(QWheelEvent* e) override {
    const int step = e->angleDelta().y() > 0 ? 1 : (e->angleDelta().y() < 0 ? -1 : 0);
    if (e->modifiers() & Qt::ShiftModifier)
      set_range(std::min(lo_ + step, hi_), hi_);
    else
      set_range(lo_, std::max(hi_ + step, lo_));
    e->accept();
  }
  void keyPressEvent(QKeyEvent* e) override {
    const bool lower_handle = e->modifiers() & Qt::ShiftModifier;
    int d = 0;
    if (e->key() == Qt::Key_Up || e->key() == Qt::Key_Right) d = 1;
    if (e->key() == Qt::Key_Down || e->key() == Qt::Key_Left) d = -1;
    if (e->key() == Qt::Key_PageUp) d = 10;
    if (e->key() == Qt::Key_PageDown) d = -10;
    if (d == 0) {
      QWidget::keyPressEvent(e);
      return;
    }
    if (lower_handle)
      set_range(std::min(lo_ + d, hi_), hi_);
    else
      set_range(lo_, std::max(hi_ + d, lo_));
  }

 private:
  int track_x() const { return width() - 16; }
  double top() const { return 16; }
  double bottom() const { return height() - 16.0; }
  double y_of(int v) const {
    if (n_ <= 1) return bottom();
    return bottom() - (bottom() - top()) * v / (n_ - 1);
  }
  void move_to(double y) {
    if (n_ <= 1) return;
    const int v = static_cast<int>(std::lround((bottom() - y) / (bottom() - top()) * (n_ - 1)));
    if (drag_ == 1)
      set_range(std::min(v, hi_), hi_);
    else
      set_range(lo_, std::max(v, lo_));
  }

  int n_ = 0, lo_ = 0, hi_ = 0;
  int drag_ = 0;  // 1 — нижняя ручка, 2 — верхняя
};

}  // namespace kika::gui
