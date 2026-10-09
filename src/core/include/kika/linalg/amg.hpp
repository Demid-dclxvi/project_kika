#pragma once
// Алгебраический многосеточный метод (сглаженная агрегация) + сопряжённые градиенты.
// Перенос fdmfea/amg.py: агрегаты строятся геометрически по воксельной сетке (блоки
// 3×3×3 узла), ближнее ядро — 6 перемещений тела как жёсткого целого, сглаживатель —
// полином Чебышёва, внешний метод — метод сопряжённых градиентов с V-циклом.

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "kika/linalg/dense.hpp"
#include "kika/linalg/sparse.hpp"

namespace kika::linalg {

struct AmgOptions {
  std::int64_t max_coarse = 3000;  // меньше — решаем грубую задачу напрямую
  int max_levels = 12;
  int degree = 3;  // степень полинома Чебышёва
  int block = 3;   // размер блока агрегации в ячейках сетки
};

struct PcgResult {
  int iterations = 0;
  double rel_residual = 0.0;
};

class SmoothedAggregation {
 public:
  // a — симметричная положительно определённая матрица (забирается во владение);
  // near_null — базис ближнего ядра, построчно n × k; cells — ячейка сетки каждой неизвестной.
  SmoothedAggregation(CsrMatrix a, std::vector<double> near_null, int k,
                      std::vector<std::array<std::int64_t, 3>> cells, const AmgOptions& opt = {});

  int n_levels() const { return static_cast<int>(levels_.size()) + 1; }
  std::int64_t coarse_size() const { return coarse_.rows; }
  const CsrMatrix& matrix() const { return levels_.empty() ? coarse_ : levels_.front().a; }

  // Один V-цикл: x ≈ A⁻¹·b.
  void vcycle(std::span<const double> b, std::span<double> x);

  // Предобусловленные сопряжённые градиенты; x на входе игнорируется (старт с нуля).
  PcgResult solve(std::span<const double> b, std::span<double> x, double tol = 1e-7, int maxiter = 500);

 private:
  struct Level {
    CsrMatrix a, p, r;
    std::vector<double> dinv;
    double rho = 1.0;
    // рабочие векторы
    std::vector<double> res, d, x, bc, xc;
  };

  void vcycle_level(std::size_t lv, std::span<const double> b, std::span<double> x);
  void chebyshev(Level& lv, std::span<const double> b, std::span<double> x, bool zero_start);
  void coarse_solve(std::span<const double> b, std::span<double> x);

  int degree_ = 3;
  std::vector<Level> levels_;
  CsrMatrix coarse_;
  enum class CoarseKind { Cholesky, Iterative } coarse_kind_ = CoarseKind::Cholesky;
  Dense chol_;
  std::vector<double> coarse_dinv_;
  std::vector<double> tmp_;
};

// Оценка спектрального радиуса D⁻¹·A степенным методом (20 итераций).
double spectral_radius(const CsrMatrix& a, std::span<const double> dinv, int iters = 20);

}  // namespace kika::linalg
