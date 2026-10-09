// Перенос fdmfea/amg.py. Отличия (см. docs/ARCHITECTURE.md):
//  * начальный вектор степенного метода — свой генератор (splitmix64 + Бокс — Мюллер), а не numpy;
//  * если грубая матрица не раскладывается по Холецкому, сдвиг диагонали увеличивается
//    (в прототипе — псевдообратная матрица); грубая задача больше 4000 неизвестных решается
//    методом сопряжённых градиентов почти точно (в прототипе — разреженный LU).

#include "kika/linalg/amg.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <utility>

namespace kika::linalg {

namespace {

// Детерминированный генератор нормальных чисел: одинаковый на всех платформах.
class NormalRng {
 public:
  explicit NormalRng(std::uint64_t seed) : state_(seed) {}
  double next() {
    if (has_spare_) {
      has_spare_ = false;
      return spare_;
    }
    double u1 = uniform();
    while (u1 <= 0.0) u1 = uniform();
    const double u2 = uniform();
    const double r = std::sqrt(-2.0 * std::log(u1));
    const double t = 2.0 * std::numbers::pi * u2;
    spare_ = r * std::sin(t);
    has_spare_ = true;
    return r * std::cos(t);
  }

 private:
  double uniform() {
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z ^= z >> 31;
    return static_cast<double>(z >> 11) * 0x1.0p-53;
  }
  std::uint64_t state_;
  double spare_ = 0.0;
  bool has_spare_ = false;
};

struct Tentative {
  CsrMatrix t;
  std::vector<double> bc;              // грубый базис, построчно nc × k
  std::vector<std::int64_t> per_agg;   // число грубых неизвестных в агрегате
};

// Tentative prolongator T (QR базиса B на каждом агрегате) и грубый базис Bc.
Tentative tentative(const std::vector<std::int32_t>& agg, std::int64_t n_agg, const std::vector<double>& b,
                    int nb) {
  const std::size_t n = agg.size();
  const auto na = static_cast<std::size_t>(n_agg);
  std::vector<std::int64_t> counts(na, 0);
  for (auto a : agg) ++counts[static_cast<std::size_t>(a)];
  std::vector<std::int64_t> starts(na + 1, 0);
  for (std::size_t a = 0; a < na; ++a) starts[a + 1] = starts[a] + counts[a];
  // устойчивая сортировка по номеру агрегата
  std::vector<std::int64_t> order(n);
  {
    std::vector<std::int64_t> pos(starts.begin(), starts.end() - 1);
    for (std::size_t i = 0; i < n; ++i) order[static_cast<std::size_t>(pos[static_cast<std::size_t>(agg[i])]++)] =
        static_cast<std::int64_t>(i);
  }
  Tentative res;
  res.per_agg.resize(na);
  std::vector<std::int64_t> coff(na + 1, 0);
  for (std::size_t a = 0; a < na; ++a) {
    res.per_agg[a] = std::min<std::int64_t>(counts[a], nb);
    coff[a + 1] = coff[a] + res.per_agg[a];
  }
  const std::int64_t nc = coff[na];
  res.bc.assign(static_cast<std::size_t>(nc) * static_cast<std::size_t>(nb), 0.0);

  CsrMatrix& t = res.t;
  t.rows = static_cast<std::int64_t>(n);
  t.cols = nc;
  t.row_ptr.assign(n + 1, 0);
  for (std::size_t i = 0; i < n; ++i)
    t.row_ptr[i + 1] = t.row_ptr[i] + res.per_agg[static_cast<std::size_t>(agg[i])];
  t.col.resize(static_cast<std::size_t>(t.row_ptr[n]));
  t.val.resize(static_cast<std::size_t>(t.row_ptr[n]));

  Dense ba, q, r;
  for (std::size_t a = 0; a < na; ++a) {
    const auto m = static_cast<std::size_t>(counts[a]);
    if (m == 0) continue;
    ba = Dense(m, static_cast<std::size_t>(nb));
    for (std::size_t i = 0; i < m; ++i) {
      const auto row = static_cast<std::size_t>(order[static_cast<std::size_t>(starts[a]) + i]);
      for (std::size_t c = 0; c < static_cast<std::size_t>(nb); ++c)
        ba(i, c) = b[row * static_cast<std::size_t>(nb) + c];
    }
    householder_qr(ba, q, r);
    const std::size_t km = q.cols;
    // знак диагонали R делаем положительным — для устойчивости
    for (std::size_t j = 0; j < km; ++j) {
      const double d = r(j, j);
      const double s = d > 0 ? 1.0 : (d < 0 ? -1.0 : 1.0);
      if (s < 0) {
        for (std::size_t i = 0; i < m; ++i) q(i, j) = -q(i, j);
        for (std::size_t c = 0; c < r.cols; ++c) r(j, c) = -r(j, c);
      }
    }
    for (std::size_t i = 0; i < m; ++i) {
      const auto row = static_cast<std::size_t>(order[static_cast<std::size_t>(starts[a]) + i]);
      const auto p0 = static_cast<std::size_t>(t.row_ptr[row]);
      for (std::size_t j = 0; j < km; ++j) {
        t.col[p0 + j] = static_cast<std::int32_t>(coff[a] + static_cast<std::int64_t>(j));
        t.val[p0 + j] = q(i, j);
      }
    }
    for (std::size_t j = 0; j < km; ++j)
      for (std::size_t c = 0; c < static_cast<std::size_t>(nb); ++c)
        res.bc[(static_cast<std::size_t>(coff[a]) + j) * static_cast<std::size_t>(nb) + c] = r(j, c);
  }
  return res;
}

void axpy(double alpha, std::span<const double> x, std::span<double> y) {
  for (std::size_t i = 0; i < y.size(); ++i) y[i] += alpha * x[i];
}

}  // namespace

double spectral_radius(const CsrMatrix& a, std::span<const double> dinv, int iters) {
  const auto n = static_cast<std::size_t>(a.rows);
  if (n == 0) return 1.0;
  NormalRng rng(0);
  std::vector<double> x(n), y(n);
  for (auto& v : x) v = rng.next();
  const double nx = norm2(x);
  for (auto& v : x) v /= nx;
  double lam = 1.0;
  for (int it = 0; it < iters; ++it) {
    a.multiply(x, y);
    for (std::size_t i = 0; i < n; ++i) y[i] *= dinv[i];
    lam = norm2(y);
    if (lam == 0.0) return 1.0;
    for (std::size_t i = 0; i < n; ++i) x[i] = y[i] / lam;
  }
  return lam;
}

SmoothedAggregation::SmoothedAggregation(CsrMatrix a, std::vector<double> near_null, int k,
                                         std::vector<std::array<std::int64_t, 3>> cells, const AmgOptions& opt)
    : degree_(opt.degree) {
  CsrMatrix cur = std::move(a);
  std::vector<double> basis = std::move(near_null);
  while (cur.rows > opt.max_coarse && static_cast<int>(levels_.size()) < opt.max_levels) {
    const auto n = static_cast<std::size_t>(cur.rows);
    std::vector<double> dinv = cur.diagonal();
    for (auto& v : dinv) v = 1.0 / (v > 0 ? v : 1.0);
    const double rho = spectral_radius(cur, dinv);

    // агрегаты: блоки block×block×block ячеек
    std::vector<std::array<std::int64_t, 3>> acell(n);
    std::array<std::int64_t, 3> mx{0, 0, 0};
    for (std::size_t i = 0; i < n; ++i)
      for (std::size_t ax = 0; ax < 3; ++ax) {
        acell[i][ax] = cells[i][ax] / opt.block;
        mx[ax] = std::max(mx[ax], acell[i][ax] + 1);
      }
    std::vector<std::int64_t> key(n);
    for (std::size_t i = 0; i < n; ++i) key[i] = (acell[i][2] * mx[1] + acell[i][1]) * mx[0] + acell[i][0];
    std::vector<std::int64_t> ukey = key;
    std::sort(ukey.begin(), ukey.end());
    ukey.erase(std::unique(ukey.begin(), ukey.end()), ukey.end());
    std::vector<std::int32_t> agg(n);
    for (std::size_t i = 0; i < n; ++i)
      agg[i] = static_cast<std::int32_t>(std::lower_bound(ukey.begin(), ukey.end(), key[i]) - ukey.begin());
    const auto n_agg = static_cast<std::int64_t>(ukey.size());

    Tentative tv = tentative(agg, n_agg, basis, k);
    if (static_cast<double>(tv.t.cols) >= 0.9 * static_cast<double>(cur.rows)) break;  // огрубление застопорилось

    const double omega = 4.0 / (3.0 * rho);
    CsrMatrix at = multiply(cur, tv.t);
    std::vector<double> s(n);
    for (std::size_t i = 0; i < n; ++i) s[i] = omega * dinv[i];
    scale_rows(at, s);
    CsrMatrix p = add(tv.t, at, 1.0, -1.0);
    eliminate_zeros(p);
    at = CsrMatrix{};
    CsrMatrix r = transpose(p);
    CsrMatrix ac = multiply(r, multiply(cur, p));
    ac = add(ac, transpose(ac), 0.5, 0.5);
    eliminate_zeros(ac);

    // «ячейка» грубой неизвестной — ячейка её агрегата
    std::vector<std::array<std::int64_t, 3>> agg_cell(static_cast<std::size_t>(n_agg));
    for (std::size_t i = 0; i < n; ++i) agg_cell[static_cast<std::size_t>(agg[i])] = acell[i];
    std::vector<std::array<std::int64_t, 3>> ccells;
    ccells.reserve(static_cast<std::size_t>(tv.t.cols));
    for (std::size_t g = 0; g < agg_cell.size(); ++g)
      for (std::int64_t j = 0; j < tv.per_agg[g]; ++j) ccells.push_back(agg_cell[g]);

    Level lv;
    lv.a = std::move(cur);
    lv.p = std::move(p);
    lv.r = std::move(r);
    lv.dinv = std::move(dinv);
    lv.rho = rho;
    lv.res.resize(n);
    lv.d.resize(n);
    lv.x.resize(n);
    lv.bc.resize(static_cast<std::size_t>(ac.rows));
    lv.xc.resize(static_cast<std::size_t>(ac.rows));
    levels_.push_back(std::move(lv));
    cur = std::move(ac);
    basis = std::move(tv.bc);
    cells = std::move(ccells);
  }

  coarse_ = std::move(cur);
  const auto n = static_cast<std::size_t>(coarse_.rows);
  tmp_.resize(n);
  if (n <= 4000) {
    Dense ad(n, n);
    for (std::size_t i = 0; i < n; ++i)
      for (auto p = coarse_.row_ptr[i]; p < coarse_.row_ptr[i + 1]; ++p)
        ad(i, static_cast<std::size_t>(coarse_.col[static_cast<std::size_t>(p)])) +=
            coarse_.val[static_cast<std::size_t>(p)];
    double dmax = 0.0;
    for (std::size_t i = 0; i < n; ++i) {
      for (std::size_t j = i + 1; j < n; ++j) {
        const double v = 0.5 * (ad(i, j) + ad(j, i));
        ad(i, j) = v;
        ad(j, i) = v;
      }
      dmax = std::max(dmax, std::abs(ad(i, i)));
    }
    dmax = std::max(dmax, 1e-30);
    // почти вырожденная грубая матрица: увеличиваем сдвиг диагонали
    for (double shift : {1e-10, 1e-8, 1e-6, 1e-4}) {
      chol_ = ad;
      for (std::size_t i = 0; i < n; ++i) chol_(i, i) += shift * dmax;
      if (cholesky_inplace(chol_)) {
        coarse_kind_ = CoarseKind::Cholesky;
        return;
      }
    }
    chol_ = Dense{};
  }
  coarse_kind_ = CoarseKind::Iterative;
  coarse_dinv_ = coarse_.diagonal();
  for (auto& v : coarse_dinv_) v = 1.0 / (v > 0 ? v : 1.0);
}

void SmoothedAggregation::coarse_solve(std::span<const double> b, std::span<double> x) {
  const std::size_t n = b.size();
  if (coarse_kind_ == CoarseKind::Cholesky) {
    std::copy(b.begin(), b.end(), tmp_.begin());
    cholesky_solve(chol_, tmp_);
    std::copy(tmp_.begin(), tmp_.end(), x.begin());
    return;
  }
  // большая грубая задача: сопряжённые градиенты с диагональным предобусловливателем почти до точного
  std::fill(x.begin(), x.end(), 0.0);
  std::vector<double> r(b.begin(), b.end()), z(n), p(n), ap(n);
  const double nb = norm2(b);
  if (nb == 0.0) return;
  for (std::size_t i = 0; i < n; ++i) z[i] = coarse_dinv_[i] * r[i];
  p = z;
  double rz = dot(r, z);
  const int maxiter = static_cast<int>(std::max<std::size_t>(1000, 10 * n));
  for (int it = 0; it < maxiter; ++it) {
    coarse_.multiply(p, ap);
    const double alpha = rz / dot(p, ap);
    axpy(alpha, p, x);
    axpy(-alpha, ap, r);
    if (norm2(r) < 1e-12 * nb) break;
    for (std::size_t i = 0; i < n; ++i) z[i] = coarse_dinv_[i] * r[i];
    const double rz_new = dot(r, z);
    const double beta = rz_new / rz;
    for (std::size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    rz = rz_new;
  }
}

void SmoothedAggregation::chebyshev(Level& lv, std::span<const double> b, std::span<double> x, bool zero_start) {
  const double upper = 1.1 * lv.rho, lower = lv.rho / 30.0;
  const double theta = 0.5 * (upper + lower);
  const double delta = 0.5 * (upper - lower);
  const double sigma = theta / delta;
  const std::size_t n = b.size();
  auto& r = lv.res;
  auto& d = lv.d;
  auto& ad = lv.x;  // временный вектор A·d
  if (zero_start) {
    std::copy(b.begin(), b.end(), r.begin());
    std::fill(x.begin(), x.end(), 0.0);
  } else {
    lv.a.multiply(x, r);
    for (std::size_t i = 0; i < n; ++i) r[i] = b[i] - r[i];
  }
  for (std::size_t i = 0; i < n; ++i) {
    d[i] = lv.dinv[i] * r[i] / theta;
    x[i] += d[i];
  }
  double rho_old = 1.0 / sigma;
  for (int k = 0; k < degree_ - 1; ++k) {
    lv.a.multiply(d, ad);
    const double rho_new = 1.0 / (2.0 * sigma - rho_old);
    const double c1 = rho_new * rho_old;
    const double c2 = 2.0 * rho_new / delta;
    for (std::size_t i = 0; i < n; ++i) {
      r[i] -= ad[i];
      d[i] = c1 * d[i] + c2 * (lv.dinv[i] * r[i]);
      x[i] += d[i];
    }
    rho_old = rho_new;
  }
}

void SmoothedAggregation::vcycle_level(std::size_t l, std::span<const double> b, std::span<double> x) {
  if (l == levels_.size()) {
    coarse_solve(b, x);
    return;
  }
  Level& lv = levels_[l];
  const std::size_t n = b.size();
  chebyshev(lv, b, x, true);
  lv.a.multiply(x, lv.res);
  for (std::size_t i = 0; i < n; ++i) lv.res[i] = b[i] - lv.res[i];
  lv.r.multiply(lv.res, lv.bc);
  vcycle_level(l + 1, lv.bc, lv.xc);
  lv.p.multiply(lv.xc, lv.res);
  for (std::size_t i = 0; i < n; ++i) x[i] += lv.res[i];
  chebyshev(lv, b, x, false);
}

void SmoothedAggregation::vcycle(std::span<const double> b, std::span<double> x) { vcycle_level(0, b, x); }

PcgResult SmoothedAggregation::solve(std::span<const double> b, std::span<double> x, double tol, int maxiter) {
  const CsrMatrix& a = matrix();
  const std::size_t n = b.size();
  std::fill(x.begin(), x.end(), 0.0);
  PcgResult res;
  const double nb = norm2(b);
  if (nb == 0.0) return res;
  std::vector<double> r(b.begin(), b.end()), z(n), p(n), ap(n);
  vcycle(r, z);
  p = z;
  double rz = dot(r, z);
  res.rel_residual = 1.0;
  for (int it = 1; it <= maxiter; ++it) {
    res.iterations = it;
    a.multiply(p, ap);
    const double alpha = rz / dot(p, ap);
    axpy(alpha, p, x);
    axpy(-alpha, ap, r);
    res.rel_residual = norm2(r) / nb;
    if (res.rel_residual < tol) break;
    vcycle(r, z);
    const double rz_new = dot(r, z);
    const double beta = rz_new / rz;
    for (std::size_t i = 0; i < n; ++i) p[i] = z[i] + beta * p[i];
    rz = rz_new;
  }
  return res;
}

}  // namespace kika::linalg
