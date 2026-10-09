#pragma once
// Конечный элемент: 8-узловой шестигранник с несовместными модами (Вильсон — Тейлор).
// Точно передаёт изгиб даже при 2–3 элементах по толщине. Перенос fdmfea/fem.py.
//
// Материал у каждого элемента свой (анизотропный). Матрица жёсткости линейна по
// 21 независимой компоненте C, поэтому базисные матрицы считаются один раз на размер элемента.

#include <array>
#include <vector>

#include "kika/linalg/dense.hpp"

namespace kika::fem {

inline constexpr int kPairs = 21;  // независимые компоненты симметричной C 6×6 (i ≤ j)
using CVec = std::array<double, kPairs>;

CVec c_to_vec(const linalg::Mat6& c);
linalg::Mat6 vec_to_c(const CVec& v);

// Узлы элемента в локальных координатах (−1/+1) и узлы граней:
// 0: −x, 1: +x, 2: −y, 3: +y, 4: −z, 5: +z (обход наружу по правилу правой руки).
inline constexpr int kLocalNodes[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                          {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
inline constexpr int kFaceNodes[6][4] = {{0, 4, 7, 3}, {1, 2, 6, 5}, {0, 1, 5, 4},
                                         {3, 7, 6, 2}, {0, 3, 2, 1}, {4, 5, 6, 7}};
inline constexpr double kFaceNormals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0},
                                              {0, 1, 0},  {0, 0, -1}, {0, 0, 1}};

class ElementBasis {
 public:
  ElementBasis(double dx, double dy, double dz);

  double dx() const { return dims_[0]; }
  double dy() const { return dims_[1]; }
  double dz() const { return dims_[2]; }
  double volume() const { return dims_[0] * dims_[1] * dims_[2]; }

  // Матрица жёсткости 24×24 (построчно) после исключения несовместных мод.
  void stiffness(const CVec& c, double* ke) const;

  // Деформации в 8 точках Гаусса (8×6) и средняя по элементу (6) при перемещениях узлов ue (24).
  void strains(const CVec& c, const double* ue, double* eps_gauss, double* eps_mean) const;

  // ∫B dV (6×24) — для температурных нагрузок.
  const std::array<double, 6 * 24>& b_integral() const { return bint_; }

 private:
  void condensation(const CVec& c, double* kua, double* kaa) const;

  std::array<double, 3> dims_;
  std::array<double, 8 * 6 * 24> bg_{};  // B в точках Гаусса
  std::array<double, 8 * 6 * 9> gg_{};   // G (несовместные моды) в точках Гаусса
  std::array<double, 6 * 24> bint_{};
  std::vector<double> kuu_;  // 21 × 24 × 24
  std::vector<double> kua_;  // 21 × 24 × 9
  std::vector<double> kaa_;  // 21 × 9 × 9
};

}  // namespace kika::fem
