#include "kika/linalg/dense.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "util/parallel.hpp"

namespace kika::linalg {

std::optional<Mat6> inverse(const Mat6& m) {
  Mat6 a = m;
  Mat6 inv = Mat6::identity();
  for (std::size_t c = 0; c < 6; ++c) {
    std::size_t p = c;
    for (std::size_t r = c + 1; r < 6; ++r)
      if (std::abs(a(r, c)) > std::abs(a(p, c))) p = r;
    if (a(p, c) == 0.0 || !std::isfinite(a(p, c))) return std::nullopt;
    if (p != c)
      for (std::size_t j = 0; j < 6; ++j) {
        std::swap(a(p, j), a(c, j));
        std::swap(inv(p, j), inv(c, j));
      }
    const double d = a(c, c);
    for (std::size_t j = 0; j < 6; ++j) {
      a(c, j) /= d;
      inv(c, j) /= d;
    }
    for (std::size_t r = 0; r < 6; ++r) {
      if (r == c) continue;
      const double f = a(r, c);
      if (f == 0.0) continue;
      for (std::size_t j = 0; j < 6; ++j) {
        a(r, j) -= f * a(c, j);
        inv(r, j) -= f * inv(c, j);
      }
    }
  }
  return inv;
}

bool is_positive_definite(const Mat6& m) {
  Dense a(6, 6);
  for (std::size_t i = 0; i < 6; ++i)
    for (std::size_t j = 0; j < 6; ++j) a(i, j) = 0.5 * (m(i, j) + m(j, i));
  return cholesky_inplace(a);
}

bool lu_solve_inplace(Dense& a, Dense& b) {
  const std::size_t n = a.rows;
  const std::size_t m = b.cols;
  for (std::size_t c = 0; c < n; ++c) {
    std::size_t p = c;
    for (std::size_t r = c + 1; r < n; ++r)
      if (std::abs(a(r, c)) > std::abs(a(p, c))) p = r;
    if (a(p, c) == 0.0) return false;
    if (p != c) {
      for (std::size_t j = 0; j < n; ++j) std::swap(a(p, j), a(c, j));
      for (std::size_t j = 0; j < m; ++j) std::swap(b(p, j), b(c, j));
    }
    const double d = a(c, c);
    for (std::size_t r = c + 1; r < n; ++r) {
      const double f = a(r, c) / d;
      if (f == 0.0) continue;
      for (std::size_t j = c; j < n; ++j) a(r, j) -= f * a(c, j);
      for (std::size_t j = 0; j < m; ++j) b(r, j) -= f * b(c, j);
    }
  }
  for (std::size_t c = n; c-- > 0;) {
    for (std::size_t j = 0; j < m; ++j) {
      double s = b(c, j);
      for (std::size_t k = c + 1; k < n; ++k) s -= a(c, k) * b(k, j);
      b(c, j) = s / a(c, c);
    }
  }
  return true;
}

namespace {

// Скалярное произведение с четырьмя независимыми суммами (быстрее, результат детерминирован).
double dot4(const double* x, const double* y, std::size_t n) {
  double s0 = 0.0, s1 = 0.0, s2 = 0.0, s3 = 0.0;
  std::size_t k = 0;
  for (; k + 4 <= n; k += 4) {
    s0 += x[k] * y[k];
    s1 += x[k + 1] * y[k + 1];
    s2 += x[k + 2] * y[k + 2];
    s3 += x[k + 3] * y[k + 3];
  }
  for (; k < n; ++k) s0 += x[k] * y[k];
  return (s0 + s1) + (s2 + s3);
}

}  // namespace

bool cholesky_inplace(Dense& a) {
  const std::size_t n = a.rows;
  for (std::size_t j = 0; j < n; ++j) {
    const double* rj = &a.v[j * n];
    const double d = a(j, j) - dot4(rj, rj, j);
    if (!(d > 0.0)) return false;
    const double ljj = std::sqrt(d);
    a(j, j) = ljj;
    // строки ниже j независимы — считаем их параллельно, если работы достаточно
    const std::size_t grain = j < 64 ? n : std::max<std::size_t>(16, 32768 / j);
    util::parallel_for(j + 1, n, grain, [&](std::size_t lo, std::size_t hi) {
      for (std::size_t i = lo; i < hi; ++i) {
        double* ri = &a.v[i * n];
        ri[j] = (ri[j] - dot4(ri, rj, j)) / ljj;
      }
    });
  }
  // верхний треугольник обнуляем, чтобы матрица была ровно L
  for (std::size_t i = 0; i < n; ++i)
    for (std::size_t j = i + 1; j < n; ++j) a(i, j) = 0.0;
  return true;
}

void cholesky_solve(const Dense& l, std::vector<double>& b) {
  const std::size_t n = l.rows;
  for (std::size_t i = 0; i < n; ++i) {
    const double* ri = &l.v[i * n];
    b[i] = (b[i] - dot4(ri, b.data(), i)) / ri[i];
  }
  // Lᵀ·x = y: идём по строкам L (подряд в памяти), а не по столбцам
  for (std::size_t i = n; i-- > 0;) {
    const double* ri = &l.v[i * n];
    const double xi = b[i] / ri[i];
    b[i] = xi;
    for (std::size_t k = 0; k < i; ++k) b[k] -= ri[k] * xi;
  }
}

void householder_qr(const Dense& a, Dense& q, Dense& r) {
  const std::size_t m = a.rows;
  const std::size_t n = a.cols;
  const std::size_t k = std::min(m, n);
  Dense w = a;
  std::vector<std::vector<double>> vs(k);
  for (std::size_t j = 0; j < k; ++j) {
    double norm = 0.0;
    for (std::size_t i = j; i < m; ++i) norm += w(i, j) * w(i, j);
    norm = std::sqrt(norm);
    if (norm == 0.0) continue;  // столбец уже нулевой — отражение не нужно
    const double alpha = w(j, j) > 0 ? -norm : norm;
    std::vector<double> v(m - j);
    for (std::size_t i = j; i < m; ++i) v[i - j] = w(i, j);
    v[0] -= alpha;
    double vn = 0.0;
    for (double x : v) vn += x * x;
    vn = std::sqrt(vn);
    if (vn == 0.0) continue;
    for (double& x : v) x /= vn;
    for (std::size_t c = j; c < n; ++c) {
      double s = 0.0;
      for (std::size_t i = j; i < m; ++i) s += v[i - j] * w(i, c);
      for (std::size_t i = j; i < m; ++i) w(i, c) -= 2.0 * s * v[i - j];
    }
    vs[j] = std::move(v);
  }
  r = Dense(k, n);
  for (std::size_t i = 0; i < k; ++i)
    for (std::size_t c = i; c < n; ++c) r(i, c) = w(i, c);
  q = Dense(m, k);
  for (std::size_t i = 0; i < k; ++i) q(i, i) = 1.0;
  for (std::size_t j = k; j-- > 0;) {
    const auto& v = vs[j];
    if (v.empty()) continue;
    for (std::size_t c = 0; c < k; ++c) {
      double s = 0.0;
      for (std::size_t i = j; i < m; ++i) s += v[i - j] * q(i, c);
      for (std::size_t i = j; i < m; ++i) q(i, c) -= 2.0 * s * v[i - j];
    }
  }
}

}  // namespace kika::linalg
