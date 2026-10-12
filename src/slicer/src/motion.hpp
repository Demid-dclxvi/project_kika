#pragma once
// Оценка времени печати с разгонами и торможениями — так, как планирует движение Klipper:
// трапеция скорости на каждом отрезке, скорость на стыке ограничена «скоростью в прямом угле»
// (square_corner_velocity) и центростремительным ускорением, просмотр вперёд по очереди отрезков.
// Внутренний заголовок слайсера.

#include <cstddef>
#include <vector>

namespace kika::slicer::detail {

class TimeEstimator {
 public:
  // corner_speed — скорость в прямом угле, мм/с; max_accel — предел принтера, мм/с².
  TimeEstimator(double corner_speed, double max_accel);

  // Отрезок по X, Y, Z со скоростью v (мм/с) и ускорением accel (мм/с²).
  void move(double dx, double dy, double dz, double v, double accel);
  // Движение, после которого принтер стоит (откат, подъём по Z, пауза): время задано.
  void stop(double seconds);
  // Досчитать очередь (в конце печати).
  void flush();
  double total() const { return total_; }

 private:
  struct Move {
    double d = 0, ux = 0, uy = 0, uz = 0;
    double accel = 1, delta_v2 = 0, max_cruise_v2 = 0, max_start_v2 = 0;
  };
  double jd_ = 0;  // junction deviation
  std::vector<Move> q_;
  double total_ = 0;
};

}  // namespace kika::slicer::detail
