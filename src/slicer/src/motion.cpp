// Оценка времени печати по трапециям скорости (как toolhead.py в Klipper: стыки отрезков,
// просмотр вперёд). Своя реализация по описанию алгоритма.

#include "motion.hpp"

#include <algorithm>
#include <cmath>

namespace kika::slicer::detail {

TimeEstimator::TimeEstimator(double corner_speed, double max_accel) {
  const double a = std::max(max_accel, 1.0);
  jd_ = corner_speed * corner_speed * (std::sqrt(2.0) - 1.0) / a;
  q_.reserve(4096);
}

void TimeEstimator::move(double dx, double dy, double dz, double v, double accel) {
  const double d = std::sqrt(dx * dx + dy * dy + dz * dz);
  if (d < 1e-9 || v <= 0) return;
  Move m;
  m.d = d;
  m.ux = dx / d;
  m.uy = dy / d;
  m.uz = dz / d;
  m.accel = std::max(accel, 1.0);
  m.delta_v2 = 2.0 * d * m.accel;
  m.max_cruise_v2 = v * v;
  m.max_start_v2 = 0;
  if (!q_.empty()) {
    const Move& p = q_.back();
    double start_v2 = std::min({m.max_cruise_v2, p.max_cruise_v2, p.max_start_v2 + p.delta_v2});
    const double cos_t = -(m.ux * p.ux + m.uy * p.uy + m.uz * p.uz);
    const double sin_d2 = std::sqrt(std::max(0.5 * (1.0 - cos_t), 0.0));
    const double cos_d2 = std::sqrt(std::max(0.5 * (1.0 + cos_t), 0.0));
    const double one_minus = 1.0 - sin_d2;
    if (one_minus > 1e-12 && cos_d2 > 1e-12) {
      const double r = sin_d2 / one_minus;
      const double qtan = 0.25 * sin_d2 / cos_d2;
      // разворот назад: sin = 0 → r = 0 → остановка; продолжение по прямой (cos_d2 = 0) — без предела
      start_v2 = std::min({start_v2, r * jd_ * m.accel, r * jd_ * p.accel, m.delta_v2 * qtan, p.delta_v2 * qtan});
    }
    m.max_start_v2 = start_v2;
  }
  q_.push_back(m);
  if (q_.size() >= 4096) flush();  // в конце пачки — остановка: ошибка ничтожная
}

void TimeEstimator::stop(double seconds) {
  flush();
  total_ += std::max(seconds, 0.0);
}

void TimeEstimator::flush() {
  const std::size_t n = q_.size();
  if (n == 0) return;
  // назад: с какой скорости можно начать отрезок, чтобы успеть затормозить
  std::vector<double> start(n), end(n);
  double next_start = 0;
  for (std::size_t i = n; i-- > 0;) {
    const Move& m = q_[i];
    end[i] = next_start;
    start[i] = std::min(m.max_start_v2, end[i] + m.delta_v2);
    next_start = start[i];
  }
  // вперёд: с какой скорости реально начинаем и до какой успеваем разогнаться
  double prev_end = 0;
  for (std::size_t i = 0; i < n; ++i) {
    const Move& m = q_[i];
    const double s2 = std::min(start[i], prev_end);
    const double e2 = std::min(end[i], s2 + m.delta_v2);
    const double c2 = std::min(m.max_cruise_v2, 0.5 * (s2 + e2 + m.delta_v2));
    const double vs = std::sqrt(s2), ve = std::sqrt(e2), vc = std::sqrt(std::max(c2, 1e-18));
    const double acc_d = std::max(c2 - s2, 0.0) / (2 * m.accel), dec_d = std::max(c2 - e2, 0.0) / (2 * m.accel);
    const double cruise_d = std::max(m.d - acc_d - dec_d, 0.0);
    total_ += (vc - vs) / m.accel + (vc - ve) / m.accel + cruise_d / vc;
    prev_end = e2;
  }
  q_.clear();
}

}  // namespace kika::slicer::detail
