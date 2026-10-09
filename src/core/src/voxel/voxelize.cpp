// Перенос fdmfea/voxelize.py. Порядок вычислений (и суммирования) повторяет прототип,
// чтобы результаты совпадали с точностью до округления.

#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <stdexcept>

#include "kika/voxel/voxel_model.hpp"

namespace kika::voxel {

namespace {

using gcode::Role;
using gcode::Toolpaths;

double median(std::vector<double> v) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  return n % 2 ? v[n / 2] : (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

double round_even(double v) { return std::nearbyint(v); }  // как numpy.round / round() в Python

// numpy.mod для чисел с плавающей точкой: знак результата — как у делителя
double py_mod(double a, double b) {
  double m = std::fmod(a, b);
  if (m != 0.0) {
    if ((b < 0) != (m < 0)) m += b;
  } else {
    m = std::copysign(0.0, b);
  }
  return m;
}

double round3(double v) { return std::nearbyint(v * 1000.0) / 1000.0; }

struct LayerGroups {
  std::vector<double> edges;
  std::vector<std::int32_t> grp;
};

// Группирует слои в воксельные слои ~dz_target: границы и номер группы для каждого слоя.
LayerGroups layer_groups(const std::vector<double>& tops, const std::vector<double>& hs, double dz_target) {
  LayerGroups out;
  out.edges.push_back(tops[0] - hs[0]);
  out.grp.assign(tops.size(), 0);
  double acc = 0.0;
  std::int32_t g = 0;
  for (std::size_t k = 0; k < tops.size(); ++k) {
    acc += hs[k];
    out.grp[k] = g;
    if (acc >= dz_target - 0.5 * hs[k] + 1e-9) {
      out.edges.push_back(tops[k]);
      acc = 0.0;
      ++g;
    }
  }
  if (acc > 0) {
    if (acc < 0.5 * dz_target && out.edges.size() > 1) {
      out.edges.back() = tops.back();
      for (auto& v : out.grp)
        if (v == g) v = g - 1;
    } else {
      out.edges.push_back(tops.back());
    }
  }
  return out;
}

struct Deposit {
  std::int64_t key;  // плоский номер вокселя
  double w;          // объём, мм³
  Role role;
  std::uint8_t bin;  // корзина направления
};

// Раскладка объёма каждого отрезка по вокселям: точки вдоль отрезка через s/3,
// пятно шириной валика делится между соседними вокселями по площади перекрытия.
std::vector<Deposit> deposit(const Toolpaths& tp, double sx, double sy, double X0, double Y0, int nx, int ny,
                             const std::vector<std::int32_t>& layer_grp) {
  const double s = std::min(sx, sy);
  struct Sub {
    double lo_x, lo_y, ix0, iy0, fx0, fy0, dv;
    std::int64_t iz;
    Role role;
    std::uint8_t bin;
  };
  std::vector<Sub> subs;
  subs.reserve(tp.size() * 4);
  for (const auto& sg : tp.segments) {
    const double L = std::hypot(sg.x1 - sg.x0, sg.y1 - sg.y0);
    if (!(L > 1e-6)) continue;
    const double w = std::max(sg.volume / (L * sg.h), 0.05);
    const double ang = py_mod(std::atan2(sg.y1 - sg.y0, sg.x1 - sg.x0), std::numbers::pi);
    const auto b = static_cast<std::int64_t>(round_even(ang / std::numbers::pi * kBins));
    const auto bin = static_cast<std::uint8_t>(((b % kBins) + kBins) % kBins);
    const auto nsub = std::max<std::int64_t>(1, static_cast<std::int64_t>(std::ceil(L / (s / 3.0))));
    const double dv = sg.volume / static_cast<double>(nsub);
    const double wsx = std::min(w / sx, 1.0);
    const double wsy = std::min(w / sy, 1.0);
    const std::int64_t iz = layer_grp[static_cast<std::size_t>(sg.layer)];
    for (std::int64_t j = 0; j < nsub; ++j) {
      const double t = (static_cast<double>(j) + 0.5) / static_cast<double>(nsub);
      const double px = sg.x0 + t * (sg.x1 - sg.x0);
      const double py = sg.y0 + t * (sg.y1 - sg.y0);
      const double lo_x = (px - X0) / sx - wsx / 2;
      const double lo_y = (py - Y0) / sy - wsy / 2;
      const double ix0 = std::floor(lo_x);
      const double iy0 = std::floor(lo_y);
      const double fx0 = std::clamp((ix0 + 1 - lo_x) / wsx, 0.0, 1.0);
      const double fy0 = std::clamp((iy0 + 1 - lo_y) / wsy, 0.0, 1.0);
      subs.push_back({lo_x, lo_y, ix0, iy0, fx0, fy0, dv, iz, sg.role, bin});
    }
  }
  std::vector<Deposit> out;
  out.reserve(subs.size() * 4);
  for (int dxi = 0; dxi < 2; ++dxi)
    for (int dyi = 0; dyi < 2; ++dyi)
      for (const Sub& p : subs) {
        const double fx = dxi == 0 ? p.fx0 : 1.0 - p.fx0;
        const double fy = dyi == 0 ? p.fy0 : 1.0 - p.fy0;
        const double wt = fx * fy;
        if (!(wt > 1e-6)) continue;
        const auto ixx = std::clamp<std::int64_t>(static_cast<std::int64_t>(p.ix0) + dxi, 0, nx - 1);
        const auto iyy = std::clamp<std::int64_t>(static_cast<std::int64_t>(p.iy0) + dyi, 0, ny - 1);
        out.push_back({(p.iz * ny + iyy) * nx + ixx, p.dv * wt, p.role, p.bin});
      }
  return out;
}

// Равномерный фильтр вдоль линии с нулями за краем — как scipy.ndimage.uniform_filter1d
// (mode="constant", origin=0), включая скользящую сумму.
void uniform_line(const double* in, double* out, std::size_t n, std::ptrdiff_t stride, int size) {
  const int size1 = size / 2;
  std::vector<double> buf(n + static_cast<std::size_t>(size), 0.0);
  for (std::size_t i = 0; i < n; ++i) buf[i + static_cast<std::size_t>(size1)] = in[static_cast<std::ptrdiff_t>(i) * stride];
  double tmp = 0.0;
  for (int k = 0; k < size; ++k) tmp += buf[static_cast<std::size_t>(k)];
  tmp /= size;
  out[0] = tmp;
  for (std::size_t i = 1; i < n; ++i) {
    tmp += (buf[i - 1 + static_cast<std::size_t>(size)] - buf[i - 1]) / size;
    out[static_cast<std::ptrdiff_t>(i) * stride] = tmp;
  }
}

// Сглаживание по окну winy × winx в каждом слое (оси y, затем x), как uniform_filter(size=(1, winy, winx)).
std::vector<double> uniform_filter_yx(const std::vector<double>& a, int nx, int ny, int nz, int winy, int winx) {
  std::vector<double> cur = a;
  std::vector<double> next(a.size());
  if (winy > 1) {
    for (int k = 0; k < nz; ++k)
      for (int x = 0; x < nx; ++x) {
        const std::size_t base = static_cast<std::size_t>(k) * ny * nx + static_cast<std::size_t>(x);
        uniform_line(&cur[base], &next[base], static_cast<std::size_t>(ny), nx, winy);
      }
    std::swap(cur, next);
  }
  if (winx > 1) {
    for (int k = 0; k < nz; ++k)
      for (int y = 0; y < ny; ++y) {
        const std::size_t base = (static_cast<std::size_t>(k) * ny + static_cast<std::size_t>(y)) * nx;
        uniform_line(&cur[base], &next[base], static_cast<std::size_t>(nx), 1, winx);
      }
    std::swap(cur, next);
  }
  return cur;
}

// Связные компоненты по граням (4-связность в слое / 6-связность в объёме).
// Номера — в порядке первого появления при обходе по строкам, как scipy.ndimage.label.
int label_components(const std::vector<char>& mask, int nx, int ny, int nz, std::vector<std::int32_t>& lab) {
  lab.assign(mask.size(), 0);
  int n = 0;
  std::vector<std::int64_t> stack;
  const std::int64_t sxy = static_cast<std::int64_t>(nx) * ny;
  for (std::int64_t c = 0; c < static_cast<std::int64_t>(mask.size()); ++c) {
    if (!mask[static_cast<std::size_t>(c)] || lab[static_cast<std::size_t>(c)]) continue;
    ++n;
    lab[static_cast<std::size_t>(c)] = n;
    stack.assign(1, c);
    while (!stack.empty()) {
      const std::int64_t q = stack.back();
      stack.pop_back();
      const std::int64_t k = q / sxy;
      const std::int64_t rem = q % sxy;
      const std::int64_t y = rem / nx;
      const std::int64_t x = rem % nx;
      const std::int64_t nb[6][3] = {{x - 1, y, k}, {x + 1, y, k}, {x, y - 1, k},
                                     {x, y + 1, k}, {x, y, k - 1}, {x, y, k + 1}};
      for (const auto& p : nb) {
        if (p[0] < 0 || p[0] >= nx || p[1] < 0 || p[1] >= ny || p[2] < 0 || p[2] >= nz) continue;
        const std::int64_t r = (p[2] * ny + p[1]) * nx + p[0];
        if (mask[static_cast<std::size_t>(r)] && !lab[static_cast<std::size_t>(r)]) {
          lab[static_cast<std::size_t>(r)] = n;
          stack.push_back(r);
        }
      }
    }
  }
  return n;
}

struct BuildInput {
  double w_typ;
  double layer_h;
  std::vector<double> tops;
  std::vector<double> hs;
  gcode::BBox bb;
};

VoxelModel build(const Toolpaths& tp, double s, const BuildInput& in, const Options& opt) {
  const auto& info = tp.info;
  // Сетка выравнивается по габариту детали (по наружной кромке валиков);
  // шаг по X и Y слегка подгоняется, чтобы края детали совпали с гранями вокселей.
  const double xl = in.bb.min[0] - in.w_typ / 2, xh = in.bb.max[0] + in.w_typ / 2;
  const double yl = in.bb.min[1] - in.w_typ / 2, yh = in.bb.max[1] + in.w_typ / 2;
  const int npx = std::max(1, static_cast<int>(round_even((xh - xl) / s)));
  const int npy = std::max(1, static_cast<int>(round_even((yh - yl) / s)));
  const double sx = (xh - xl) / npx;
  const double sy = (yh - yl) / npy;
  const double X0 = xl - sx, Y0 = yl - sy;
  const int nx = npx + 2, ny = npy + 2;
  const double dz_target = std::max(in.layer_h, s);
  const LayerGroups lg = layer_groups(in.tops, in.hs, dz_target);
  const int nz = static_cast<int>(lg.edges.size()) - 1;
  const std::int64_t N = static_cast<std::int64_t>(nx) * ny * nz;
  if (N > 60'000'000)
    throw std::length_error("Слишком мелкий воксель для такой детали — увеличьте размер вокселя.");
  const std::int64_t sxy = static_cast<std::int64_t>(nx) * ny;
  std::vector<double> vvox(static_cast<std::size_t>(nz));
  double dz_sum = 0.0;
  for (int k = 0; k < nz; ++k) {
    const double dz = lg.edges[static_cast<std::size_t>(k) + 1] - lg.edges[static_cast<std::size_t>(k)];
    vvox[static_cast<std::size_t>(k)] = sx * sy * dz;
    dz_sum += dz;
  }

  const std::vector<Deposit> dep = deposit(tp, sx, sy, X0, Y0, nx, ny, lg.grp);

  std::vector<double> rho_sh(static_cast<std::size_t>(N), 0.0);
  std::vector<double> rho_sp_raw(static_cast<std::size_t>(N), 0.0);
  double total_dep = 0.0;
  bool has_sparse = false;
  for (const Deposit& d : dep) {
    total_dep += d.w;
    if (d.role == Role::Sparse) {
      rho_sp_raw[static_cast<std::size_t>(d.key)] += d.w;
      has_sparse = true;
    } else {
      rho_sh[static_cast<std::size_t>(d.key)] += d.w;
    }
  }
  for (std::int64_t c = 0; c < N; ++c) {
    const double v = vvox[static_cast<std::size_t>(c / sxy)];
    rho_sh[static_cast<std::size_t>(c)] /= v;
    rho_sp_raw[static_cast<std::size_t>(c)] /= v;
  }

  // ---- области разреженного заполнения в каждом слое ----
  std::vector<double> rho_sp(static_cast<std::size_t>(N), 0.0);
  double infill_density = info.infill_density.value_or(0.0);
  if (has_sparse) {
    std::vector<char> region(static_cast<std::size_t>(N), 0);
    std::vector<char> open(static_cast<std::size_t>(sxy));
    std::vector<std::int32_t> lab;
    for (int k = 0; k < nz; ++k) {
      const std::size_t base = static_cast<std::size_t>(k * sxy);
      bool any = false;
      for (std::int64_t q = 0; q < sxy; ++q) any = any || rho_sp_raw[base + static_cast<std::size_t>(q)] > 0;
      if (!any) continue;
      for (std::int64_t q = 0; q < sxy; ++q) open[static_cast<std::size_t>(q)] = !(rho_sh[base + static_cast<std::size_t>(q)] > 0.15);
      const int nl = label_components(open, nx, ny, 1, lab);
      if (nl == 0) continue;
      std::vector<char> has(static_cast<std::size_t>(nl) + 1, 0);
      for (std::int64_t q = 0; q < sxy; ++q)
        if (rho_sp_raw[base + static_cast<std::size_t>(q)] > 0) has[static_cast<std::size_t>(lab[static_cast<std::size_t>(q)])] = 1;
      for (int x = 0; x < nx; ++x) {
        has[static_cast<std::size_t>(lab[static_cast<std::size_t>(x)])] = 0;
        has[static_cast<std::size_t>(lab[static_cast<std::size_t>((ny - 1) * nx + x)])] = 0;
      }
      for (int y = 0; y < ny; ++y) {
        has[static_cast<std::size_t>(lab[static_cast<std::size_t>(y * nx)])] = 0;
        has[static_cast<std::size_t>(lab[static_cast<std::size_t>(y * nx + nx - 1)])] = 0;
      }
      has[0] = 0;
      for (std::int64_t q = 0; q < sxy; ++q)
        region[base + static_cast<std::size_t>(q)] = has[static_cast<std::size_t>(lab[static_cast<std::size_t>(q)])];
    }
    // оценка доли заполнения
    double rsum = 0.0;
    std::int64_t rcnt = 0;
    for (std::int64_t c = 0; c < N; ++c)
      if (region[static_cast<std::size_t>(c)]) {
        rsum += rho_sp_raw[static_cast<std::size_t>(c)];
        ++rcnt;
      }
    const double est = rcnt ? rsum / static_cast<double>(rcnt) : 0.0;
    if (!(infill_density > 0)) infill_density = est;
    const double d = std::max(infill_density != 0.0 ? infill_density : est, 0.03);
    const int winx = static_cast<int>(std::clamp(round_even(2.0 * in.w_typ / (d * sx)) + 1, 1.0, 15.0));
    const int winy = static_cast<int>(std::clamp(round_even(2.0 * in.w_typ / (d * sy)) + 1, 1.0, 15.0));
    if (std::max(winx, winy) > 1) {
      std::vector<double> m(static_cast<std::size_t>(N));
      std::vector<double> rm(static_cast<std::size_t>(N));
      for (std::int64_t c = 0; c < N; ++c) {
        m[static_cast<std::size_t>(c)] = region[static_cast<std::size_t>(c)] ? 1.0 : 0.0;
        rm[static_cast<std::size_t>(c)] = rho_sp_raw[static_cast<std::size_t>(c)] * m[static_cast<std::size_t>(c)];
      }
      const auto num = uniform_filter_yx(rm, nx, ny, nz, winy, winx);
      const auto den = uniform_filter_yx(m, nx, ny, nz, winy, winx);
      for (std::int64_t c = 0; c < N; ++c) {
        const auto i = static_cast<std::size_t>(c);
        const double sm = den[i] > 1e-6 ? num[i] / den[i] : 0.0;
        rho_sp[i] = region[i] ? sm : rho_sp_raw[i];
      }
    } else {
      rho_sp = rho_sp_raw;
    }
  }
  for (std::int64_t c = 0; c < N; ++c) {
    const auto i = static_cast<std::size_t>(c);
    rho_sh[i] = std::clamp(rho_sh[i], 0.0, 1.0);
    rho_sp[i] = std::max(rho_sp[i], 0.0);
    rho_sp[i] = std::min(rho_sp[i], 1.0 - rho_sh[i]);
  }

  std::vector<char> active(static_cast<std::size_t>(N));
  bool any_active = false;
  for (std::int64_t c = 0; c < N; ++c) {
    const auto i = static_cast<std::size_t>(c);
    active[i] = (rho_sh[i] >= opt.rho_min_shell) ||
                ((rho_sp[i] >= opt.rho_min_sparse) && (rho_sh[i] + rho_sp[i] >= opt.rho_min_sparse));
    any_active = any_active || active[i];
  }
  double removed_frac = 0.0;
  if (opt.keep_largest && any_active) {
    std::vector<std::int32_t> lab;
    const int nl = label_components(active, nx, ny, nz, lab);
    if (nl > 1) {
      std::vector<std::int64_t> cnt(static_cast<std::size_t>(nl) + 1, 0);
      for (auto l : lab) ++cnt[static_cast<std::size_t>(l)];
      cnt[0] = 0;
      const auto big = static_cast<std::int32_t>(std::max_element(cnt.begin(), cnt.end()) - cnt.begin());
      double mass_all = 0.0, mass_keep = 0.0;
      for (std::int64_t c = 0; c < N; ++c) {
        const auto i = static_cast<std::size_t>(c);
        if (!active[i]) continue;
        const double mass = (rho_sh[i] + rho_sp[i]) * vvox[static_cast<std::size_t>(c / sxy)];
        mass_all += mass;
        if (lab[i] == big) mass_keep += mass;
      }
      removed_frac = 1.0 - mass_keep / std::max(mass_all, 1e-12);
      for (std::int64_t c = 0; c < N; ++c) active[static_cast<std::size_t>(c)] = lab[static_cast<std::size_t>(c)] == big;
    }
  }

  VoxelModel vm;
  vm.s = 0.5 * (sx + sy);
  vm.sx = sx;
  vm.sy = sy;
  vm.x0 = X0;
  vm.y0 = Y0;
  vm.nx = nx;
  vm.ny = ny;
  vm.nz = nz;
  vm.z_edges = lg.edges;
  std::vector<std::int64_t> flat;
  for (std::int64_t c = 0; c < N; ++c)
    if (active[static_cast<std::size_t>(c)]) {
      flat.push_back(c);
      vm.iz.push_back(static_cast<std::int32_t>(c / sxy));
      vm.iy.push_back(static_cast<std::int32_t>((c % sxy) / nx));
      vm.ix.push_back(static_cast<std::int32_t>(c % nx));
    }
  const std::size_t n = flat.size();
  auto find = [&flat](std::int64_t key) -> std::int64_t {
    const auto it = std::lower_bound(flat.begin(), flat.end(), key);
    return (it != flat.end() && *it == key) ? it - flat.begin() : -1;
  };

  // ---- гистограммы направлений стенок и роли (по всем отложениям) ----
  vm.hist_shell.assign(n * kBins, 0.0);
  std::vector<double> rh(n * 4, 0.0);
  auto role_index = [](Role r) {
    switch (r) {
      case Role::OuterWall: return 0;
      case Role::InnerWall: return 1;
      case Role::Sparse: return 3;
      default: return 2;
    }
  };
  std::vector<double> hz(static_cast<std::size_t>(nz) * kBins, 0.0);
  for (const Deposit& d : dep) {
    const std::int64_t e = find(d.key);
    if (d.role == Role::Sparse)
      hz[static_cast<std::size_t>(d.key / sxy) * kBins + d.bin] += d.w;
    if (e < 0) continue;
    if (d.role != Role::Sparse) vm.hist_shell[static_cast<std::size_t>(e) * kBins + d.bin] += d.w;
    rh[static_cast<std::size_t>(e) * 4 + static_cast<std::size_t>(role_index(d.role))] += d.w;
  }
  for (std::size_t e = 0; e < n; ++e) {
    double sm = 0.0;
    for (int b = 0; b < kBins; ++b) sm += vm.hist_shell[e * kBins + static_cast<std::size_t>(b)];
    for (int b = 0; b < kBins; ++b) {
      double& h = vm.hist_shell[e * kBins + static_cast<std::size_t>(b)];
      h = sm > 0 ? h / std::max(sm, 1e-30) : 0.0;
    }
  }

  // ---- направления разреженного заполнения: по слоям вокселей ----
  vm.hist_sparse.assign(n * kBins, 0.0);
  if (has_sparse) {
    std::vector<int> good;
    for (int k = 0; k < nz; ++k) {
      double tot = 0.0;
      for (int b = 0; b < kBins; ++b) tot += hz[static_cast<std::size_t>(k) * kBins + static_cast<std::size_t>(b)];
      if (tot > 0) good.push_back(k);
    }
    std::vector<double> hz2 = hz;
    if (!good.empty()) {
      for (int k = 0; k < nz; ++k) {
        auto it = std::lower_bound(good.begin(), good.end(), k);
        const int nearest = it == good.end() ? good.back() : *it;  // пустые слои — берём соседние
        for (int b = 0; b < kBins; ++b)
          hz2[static_cast<std::size_t>(k) * kBins + static_cast<std::size_t>(b)] =
              hz[static_cast<std::size_t>(nearest) * kBins + static_cast<std::size_t>(b)];
      }
    }
    for (int k = 0; k < nz; ++k) {
      double sm = 0.0;
      for (int b = 0; b < kBins; ++b) sm += hz2[static_cast<std::size_t>(k) * kBins + static_cast<std::size_t>(b)];
      for (int b = 0; b < kBins; ++b) hz2[static_cast<std::size_t>(k) * kBins + static_cast<std::size_t>(b)] /= std::max(sm, 1e-30);
    }
    for (std::size_t e = 0; e < n; ++e)
      for (int b = 0; b < kBins; ++b)
        vm.hist_sparse[e * kBins + static_cast<std::size_t>(b)] =
            hz2[static_cast<std::size_t>(vm.iz[e]) * kBins + static_cast<std::size_t>(b)];
  }

  vm.rho_shell.resize(n);
  vm.rho_sparse.resize(n);
  for (std::size_t e = 0; e < n; ++e) {
    vm.rho_shell[e] = rho_sh[static_cast<std::size_t>(flat[e])];
    vm.rho_sparse[e] = rho_sp[static_cast<std::size_t>(flat[e])];
  }
  auto hist_sum = [](const std::vector<double>& h, std::size_t e) {
    double total = 0.0;
    for (int b = 0; b < kBins; ++b) total += h[e * kBins + static_cast<std::size_t>(b)];
    return total;
  };
  for (std::size_t e = 0; e < n; ++e) {
    // элемент без гистограммы стенок (только заполнение) — переносим долю в заполнение
    if (hist_sum(vm.hist_shell, e) <= 0) {
      vm.rho_sparse[e] = std::min(1.0, vm.rho_sparse[e] + vm.rho_shell[e]);
      vm.rho_shell[e] = 0.0;
    }
    if (has_sparse && hist_sum(vm.hist_sparse, e) <= 0) {
      vm.rho_shell[e] = std::min(1.0, vm.rho_shell[e] + vm.rho_sparse[e]);
      vm.rho_sparse[e] = 0.0;
    }
  }

  // доминирующая роль
  constexpr Role kRoleCodes[4] = {Role::OuterWall, Role::InnerWall, Role::Solid, Role::Sparse};
  vm.role.resize(n);
  double model_vol = 0.0;
  for (std::size_t e = 0; e < n; ++e) {
    const double v = vvox[static_cast<std::size_t>(vm.iz[e])];
    rh[e * 4 + 3] += vm.rho_sparse[e] * v;  // сглаженное заполнение
    int best = 0;
    double sum = rh[e * 4];
    for (int r = 1; r < 4; ++r) {
      sum += rh[e * 4 + static_cast<std::size_t>(r)];
      if (rh[e * 4 + static_cast<std::size_t>(r)] > rh[e * 4 + static_cast<std::size_t>(best)]) best = r;
    }
    vm.role[e] = sum <= 0 ? Role::Sparse : kRoleCodes[best];
    model_vol += (vm.rho_shell[e] + vm.rho_sparse[e]) * v;
  }

  vm.infill_density = infill_density;
  vm.infill_pattern = info.infill_pattern;
  vm.layer_height = in.layer_h;
  vm.stats.deposited_volume = total_dep;
  vm.stats.model_volume = model_vol;
  vm.stats.removed_fraction = removed_frac;
  vm.stats.grid = {nx, ny, nz};
  vm.stats.line_width = in.w_typ;
  vm.stats.dz_mean = dz_sum / nz;
  return vm;
}

}  // namespace

VoxelModel voxelize(const Toolpaths& tp, const Options& opt) {
  if (tp.segments.empty()) throw std::invalid_argument("voxelize: нет отрезков экструзии");
  BuildInput in;
  in.bb = tp.bbox();
  // толщины слоёв: средняя толщина отрезков на каждом уникальном Z
  std::vector<double> zr(tp.size());
  for (std::size_t i = 0; i < tp.size(); ++i) zr[i] = round3(tp.segments[i].z);
  in.tops = zr;
  std::sort(in.tops.begin(), in.tops.end());
  in.tops.erase(std::unique(in.tops.begin(), in.tops.end()), in.tops.end());
  std::vector<double> hsum(in.tops.size(), 0.0);
  std::vector<double> hcnt(in.tops.size(), 0.0);
  for (std::size_t i = 0; i < tp.size(); ++i) {
    const auto k = static_cast<std::size_t>(std::lower_bound(in.tops.begin(), in.tops.end(), zr[i]) - in.tops.begin());
    hsum[k] += tp.segments[i].h;
    hcnt[k] += 1.0;
  }
  in.hs.resize(in.tops.size());
  for (std::size_t k = 0; k < in.tops.size(); ++k) in.hs[k] = hsum[k] / std::max(hcnt[k], 1.0);
  in.layer_h = in.hs.empty() ? 0.2 : median(in.hs);
  std::vector<double> widths(tp.size());
  for (std::size_t i = 0; i < tp.size(); ++i) {
    const auto& sg = tp.segments[i];
    widths[i] = sg.volume / std::max(std::hypot(sg.x1 - sg.x0, sg.y1 - sg.y0) * sg.h, 1e-9);
  }
  in.w_typ = std::clamp(median(widths), 0.2, 2.0);

  const bool fixed_voxel = opt.voxel && *opt.voxel > 0;
  double s = 0.0;
  if (fixed_voxel) {
    s = *opt.voxel;
  } else {
    const double vbb = std::max(in.bb.max[0] - in.bb.min[0], 0.1) * std::max(in.bb.max[1] - in.bb.min[1], 0.1) *
                       std::max(in.bb.max[2] - in.bb.min[2], 0.1);
    s = std::max(0.6 * in.w_typ, std::pow(vbb / opt.max_elems, 1.0 / 3.0));
  }
  // Подбор размера вокселя под заданное число элементов: до 4 попыток.
  VoxelModel vm;
  for (int attempt = 1;; ++attempt) {
    vm = build(tp, s, in, opt);
    if (fixed_voxel || attempt >= 4) break;
    const double n = static_cast<double>(vm.size());
    if (n > 1.1 * opt.max_elems) {
      s = s * std::pow(n / opt.max_elems, 1.0 / 3.0) * 1.03;
    } else if (n < 0.6 * opt.max_elems && s > 0.6 * in.w_typ * 1.05) {
      const double s_new = std::max(0.6 * in.w_typ, s * std::pow(n / (0.9 * opt.max_elems), 1.0 / 3.0));
      if (s_new >= s * 0.97) break;
      s = s_new;
    } else {
      break;
    }
  }
  return vm;
}

}  // namespace kika::voxel
