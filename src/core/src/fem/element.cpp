// Перенос ElementBasis из fdmfea/fem.py.

#include "kika/fem/element.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace kika::fem {

namespace {

constexpr std::array<std::pair<int, int>, kPairs> make_pairs() {
  std::array<std::pair<int, int>, kPairs> p{};
  int k = 0;
  for (int i = 0; i < 6; ++i)
    for (int j = i; j < 6; ++j) p[static_cast<std::size_t>(k++)] = {i, j};
  return p;
}
constexpr auto kPairIdx = make_pairs();

// B (6×24) и G (6×9) в точке (xi, eta, zeta)
void b_and_g(double xi, double eta, double zeta, double dx, double dy, double dz, double* b, double* g) {
  const double inv[3] = {2 / dx, 2 / dy, 2 / dz};
  for (int i = 0; i < 6 * 24; ++i) b[i] = 0.0;
  for (int i = 0; i < 6 * 9; ++i) g[i] = 0.0;
  for (int a = 0; a < 8; ++a) {
    const double xa = kLocalNodes[a][0], ya = kLocalNodes[a][1], za = kLocalNodes[a][2];
    const double nx = xa * (1 + eta * ya) * (1 + zeta * za) / 8 * inv[0];
    const double ny = ya * (1 + xi * xa) * (1 + zeta * za) / 8 * inv[1];
    const double nz = za * (1 + xi * xa) * (1 + eta * ya) / 8 * inv[2];
    const int c = 3 * a;
    b[0 * 24 + c] = nx;
    b[1 * 24 + c + 1] = ny;
    b[2 * 24 + c + 2] = nz;
    b[3 * 24 + c + 1] = nz;
    b[3 * 24 + c + 2] = ny;
    b[4 * 24 + c] = nz;
    b[4 * 24 + c + 2] = nx;
    b[5 * 24 + c] = ny;
    b[5 * 24 + c + 1] = nx;
  }
  // несовместные моды P1 = 1 − ξ², P2 = 1 − η², P3 = 1 − ζ²
  const double dp[3][3] = {{-2 * xi * inv[0], 0, 0}, {0, -2 * eta * inv[1], 0}, {0, 0, -2 * zeta * inv[2]}};
  for (int k = 0; k < 3; ++k) {
    const double nx = dp[k][0], ny = dp[k][1], nz = dp[k][2];
    const int c = 3 * k;
    g[0 * 9 + c] = nx;
    g[1 * 9 + c + 1] = ny;
    g[2 * 9 + c + 2] = nz;
    g[3 * 9 + c + 1] = nz;
    g[3 * 9 + c + 2] = ny;
    g[4 * 9 + c] = nz;
    g[4 * 9 + c + 2] = nx;
    g[5 * 9 + c] = ny;
    g[5 * 9 + c + 1] = nx;
  }
}

// out(a, b) += s · Σ_ij E_k(i,j) · X(i, a) · Y(j, b) для пары k = (i, j)
void add_pair(const double* x, int nxc, const double* y, int nyc, int i, int j, double s, double* out) {
  for (int a = 0; a < nxc; ++a) {
    const double xi = x[i * nxc + a];
    const double xj = x[j * nxc + a];
    for (int b = 0; b < nyc; ++b) {
      double v = xi * y[j * nyc + b];
      if (i != j) v += xj * y[i * nyc + b];
      out[a * nyc + b] += s * v;
    }
  }
}

}  // namespace

CVec c_to_vec(const linalg::Mat6& c) {
  CVec v{};
  for (int k = 0; k < kPairs; ++k) {
    const auto [i, j] = kPairIdx[static_cast<std::size_t>(k)];
    v[static_cast<std::size_t>(k)] = c(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
  }
  return v;
}

linalg::Mat6 vec_to_c(const CVec& v) {
  linalg::Mat6 c;
  for (int k = 0; k < kPairs; ++k) {
    const auto [i, j] = kPairIdx[static_cast<std::size_t>(k)];
    c(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) = v[static_cast<std::size_t>(k)];
    c(static_cast<std::size_t>(j), static_cast<std::size_t>(i)) = v[static_cast<std::size_t>(k)];
  }
  return c;
}

ElementBasis::ElementBasis(double dx, double dy, double dz) : dims_{dx, dy, dz} {
  const double det_j = dx * dy * dz / 8.0;
  const double gp = 1.0 / std::sqrt(3.0);
  for (int g = 0; g < 8; ++g)
    b_and_g(kLocalNodes[g][0] * gp, kLocalNodes[g][1] * gp, kLocalNodes[g][2] * gp, dx, dy, dz,
            &bg_[static_cast<std::size_t>(g) * 6 * 24], &gg_[static_cast<std::size_t>(g) * 6 * 9]);
  kuu_.assign(kPairs * 24 * 24, 0.0);
  kua_.assign(kPairs * 24 * 9, 0.0);
  kaa_.assign(kPairs * 9 * 9, 0.0);
  for (int k = 0; k < kPairs; ++k) {
    const auto [i, j] = kPairIdx[static_cast<std::size_t>(k)];
    for (int g = 0; g < 8; ++g) {
      const double* b = &bg_[static_cast<std::size_t>(g) * 6 * 24];
      const double* gm = &gg_[static_cast<std::size_t>(g) * 6 * 9];
      add_pair(b, 24, b, 24, i, j, det_j, &kuu_[static_cast<std::size_t>(k) * 576]);
      add_pair(b, 24, gm, 9, i, j, det_j, &kua_[static_cast<std::size_t>(k) * 216]);
      add_pair(gm, 9, gm, 9, i, j, det_j, &kaa_[static_cast<std::size_t>(k) * 81]);
    }
  }
  for (int g = 0; g < 8; ++g)
    for (int r = 0; r < 6 * 24; ++r) bint_[static_cast<std::size_t>(r)] += bg_[static_cast<std::size_t>(g) * 144 + static_cast<std::size_t>(r)] * det_j;
}

void ElementBasis::condensation(const CVec& c, double* kua, double* kaa) const {
  for (int r = 0; r < 216; ++r) kua[r] = 0.0;
  for (int r = 0; r < 81; ++r) kaa[r] = 0.0;
  for (int k = 0; k < kPairs; ++k) {
    const double ck = c[static_cast<std::size_t>(k)];
    if (ck == 0.0) continue;
    const double* a = &kua_[static_cast<std::size_t>(k) * 216];
    const double* b = &kaa_[static_cast<std::size_t>(k) * 81];
    for (int r = 0; r < 216; ++r) kua[r] += ck * a[r];
    for (int r = 0; r < 81; ++r) kaa[r] += ck * b[r];
  }
}

void ElementBasis::stiffness(const CVec& c, double* ke) const {
  for (int r = 0; r < 576; ++r) ke[r] = 0.0;
  for (int k = 0; k < kPairs; ++k) {
    const double ck = c[static_cast<std::size_t>(k)];
    if (ck == 0.0) continue;
    const double* a = &kuu_[static_cast<std::size_t>(k) * 576];
    for (int r = 0; r < 576; ++r) ke[r] += ck * a[r];
  }
  double kua[216];
  linalg::Dense kaa(9, 9);
  condensation(c, kua, kaa.v.data());
  // X = Kaa⁻¹ · Kauᵀ (9×24); Ke −= Kua · X
  linalg::Dense x(9, 24);
  for (int a = 0; a < 24; ++a)
    for (int b = 0; b < 9; ++b) x(static_cast<std::size_t>(b), static_cast<std::size_t>(a)) = kua[a * 9 + b];
  if (!linalg::lu_solve_inplace(kaa, x))
    throw std::runtime_error("Вырожденный элемент: несовместные моды не исключаются");
  for (int a = 0; a < 24; ++a)
    for (int m = 0; m < 9; ++m) {
      const double v = kua[a * 9 + m];
      if (v == 0.0) continue;
      for (int b = 0; b < 24; ++b) ke[a * 24 + b] -= v * x(static_cast<std::size_t>(m), static_cast<std::size_t>(b));
    }
}

void ElementBasis::strains(const CVec& c, const double* ue, double* eps_gauss, double* eps_mean) const {
  double kua[216];
  linalg::Dense kaa(9, 9);
  condensation(c, kua, kaa.v.data());
  // α = −Kaa⁻¹ · Kuaᵀ · ue
  linalg::Dense rhs(9, 1);
  for (int m = 0; m < 9; ++m) {
    double s = 0.0;
    for (int a = 0; a < 24; ++a) s += kua[a * 9 + m] * ue[a];
    rhs(static_cast<std::size_t>(m), 0) = s;
  }
  if (!linalg::lu_solve_inplace(kaa, rhs))
    throw std::runtime_error("Вырожденный элемент: несовместные моды не исключаются");
  double alpha[9];
  for (int m = 0; m < 9; ++m) alpha[m] = -rhs(static_cast<std::size_t>(m), 0);
  for (int i = 0; i < 6; ++i) eps_mean[i] = 0.0;
  for (int g = 0; g < 8; ++g) {
    const double* b = &bg_[static_cast<std::size_t>(g) * 144];
    const double* gm = &gg_[static_cast<std::size_t>(g) * 54];
    for (int i = 0; i < 6; ++i) {
      double s = 0.0;
      for (int a = 0; a < 24; ++a) s += b[i * 24 + a] * ue[a];
      for (int m = 0; m < 9; ++m) s += gm[i * 9 + m] * alpha[m];
      eps_gauss[g * 6 + i] = s;
      eps_mean[i] += s;
    }
  }
  for (int i = 0; i < 6; ++i) eps_mean[i] /= 8.0;
}

}  // namespace kika::fem
