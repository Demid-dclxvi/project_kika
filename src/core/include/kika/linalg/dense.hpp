#pragma once
// Плотные матрицы: малые фиксированного размера (6×6 жёсткости, 3×3 повороты)
// и небольшие динамические (элементные 24×24, грубый уровень многосеточного решателя).
// Хранение построчное.

#include <array>
#include <cstddef>
#include <optional>
#include <vector>

namespace kika::linalg {

template <std::size_t R, std::size_t C>
struct Mat {
  std::array<double, R * C> a{};

  double& operator()(std::size_t i, std::size_t j) { return a[i * C + j]; }
  double operator()(std::size_t i, std::size_t j) const { return a[i * C + j]; }

  static Mat identity() {
    static_assert(R == C);
    Mat m;
    for (std::size_t i = 0; i < R; ++i) m(i, i) = 1.0;
    return m;
  }
};

using Mat3 = Mat<3, 3>;
using Mat6 = Mat<6, 6>;
using Vec6 = std::array<double, 6>;

template <std::size_t R, std::size_t K, std::size_t C>
Mat<R, C> operator*(const Mat<R, K>& x, const Mat<K, C>& y) {
  Mat<R, C> out;
  for (std::size_t i = 0; i < R; ++i)
    for (std::size_t k = 0; k < K; ++k) {
      const double v = x(i, k);
      for (std::size_t j = 0; j < C; ++j) out(i, j) += v * y(k, j);
    }
  return out;
}

template <std::size_t R, std::size_t C>
Mat<C, R> transpose(const Mat<R, C>& m) {
  Mat<C, R> t;
  for (std::size_t i = 0; i < R; ++i)
    for (std::size_t j = 0; j < C; ++j) t(j, i) = m(i, j);
  return t;
}

template <std::size_t R, std::size_t C>
Mat<R, C> operator+(const Mat<R, C>& x, const Mat<R, C>& y) {
  Mat<R, C> out;
  for (std::size_t k = 0; k < R * C; ++k) out.a[k] = x.a[k] + y.a[k];
  return out;
}

template <std::size_t R, std::size_t C>
Mat<R, C> operator*(double s, const Mat<R, C>& m) {
  Mat<R, C> out;
  for (std::size_t k = 0; k < R * C; ++k) out.a[k] = s * m.a[k];
  return out;
}

inline Vec6 operator*(const Mat6& m, const Vec6& v) {
  Vec6 out{};
  for (std::size_t i = 0; i < 6; ++i) {
    double s = 0.0;
    for (std::size_t j = 0; j < 6; ++j) s += m(i, j) * v[j];
    out[i] = s;
  }
  return out;
}

// Обратная матрица (метод Гаусса — Жордана с выбором ведущего элемента); nullopt для вырожденной.
std::optional<Mat6> inverse(const Mat6& m);

// Положительная определённость симметричной матрицы (разложение Холецкого проходит).
bool is_positive_definite(const Mat6& m);

// ---------------------------------------------------------------------------
// Динамическая плотная матрица

struct Dense {
  std::size_t rows = 0;
  std::size_t cols = 0;
  std::vector<double> v;  // построчно

  Dense() = default;
  Dense(std::size_t r, std::size_t c) : rows(r), cols(c), v(r * c, 0.0) {}

  double& operator()(std::size_t i, std::size_t j) { return v[i * cols + j]; }
  double operator()(std::size_t i, std::size_t j) const { return v[i * cols + j]; }
};

// Решение A·X = B для квадратной A (LU с выбором ведущего элемента). A и B портятся.
// false — матрица вырождена.
bool lu_solve_inplace(Dense& a, Dense& b);

// Разложение Холецкого A = L·Lᵀ на месте (нижний треугольник). false — не положительно определена.
bool cholesky_inplace(Dense& a);

// Решение L·Lᵀ·x = b по готовому разложению.
void cholesky_solve(const Dense& l, std::vector<double>& b);

// QR-разложение m×n (m ≥ 1) отражениями Хаусхолдера, «укороченное» как numpy.linalg.qr:
// Q — m×k, R — k×n, k = min(m, n).
void householder_qr(const Dense& a, Dense& q, Dense& r);

}  // namespace kika::linalg
