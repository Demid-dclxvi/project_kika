// Перенос fdmfea/materials.py.

#include "kika/material/material.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

#include "util/text.hpp"

namespace kika::material {

namespace {

constexpr std::array<std::pair<int, int>, 6> kVoigt = {{{0, 0}, {1, 1}, {2, 2}, {1, 2}, {0, 2}, {0, 1}}};

using Tensor4 = std::array<double, 81>;

constexpr std::size_t t4(int i, int j, int k, int l) { return static_cast<std::size_t>(((i * 3 + j) * 3 + k) * 3 + l); }

Tensor4 voigt_to_tensor4(const Mat6& c) {
  Tensor4 t{};
  for (int I = 0; I < 6; ++I) {
    const auto [i, j] = kVoigt[static_cast<std::size_t>(I)];
    for (int J = 0; J < 6; ++J) {
      const auto [k, l] = kVoigt[static_cast<std::size_t>(J)];
      const double v = c(static_cast<std::size_t>(I), static_cast<std::size_t>(J));
      for (auto [a, b] : {std::pair{i, j}, std::pair{j, i}})
        for (auto [cc, d] : {std::pair{k, l}, std::pair{l, k}}) t[t4(a, b, cc, d)] = v;
    }
  }
  return t;
}

Mat6 tensor4_to_voigt(const Tensor4& t) {
  Mat6 c;
  for (int I = 0; I < 6; ++I) {
    const auto [i, j] = kVoigt[static_cast<std::size_t>(I)];
    for (int J = 0; J < 6; ++J) {
      const auto [k, l] = kVoigt[static_cast<std::size_t>(J)];
      c(static_cast<std::size_t>(I), static_cast<std::size_t>(J)) = t[t4(i, j, k, l)];
    }
  }
  return c;
}

linalg::Mat3 rot_z(double theta) {
  const double c = std::cos(theta);
  const double s = std::sin(theta);
  linalg::Mat3 r;
  r(0, 0) = c;
  r(0, 1) = -s;
  r(1, 0) = s;
  r(1, 1) = c;
  r(2, 2) = 1.0;
  return r;
}

struct Alias {
  std::vector<std::string_view> keys;
  std::string_view material;
};

const std::vector<Alias>& aliases() {
  static const std::vector<Alias> a = {
      {{"PAHT", "PA6-CF", "PA12-CF", "PA-CF", "PACF", "NYLON-CF", "PA-GF", "PA6-GF", "PAHT-CF"}, "PA-CF"},
      {{"PETG-CF", "PET-CF", "PETG CF"}, "PETG-CF"},
      {{"PLA-CF", "PLA CF"}, "PLA-CF"},
      {{"PETG", "PET", "PCTG"}, "PETG"},
      {{"ASA"}, "ASA"},
      {{"ABS"}, "ABS"},
      {{"PC"}, "PC"},
      {{"PA", "NYLON", "PA6", "PA12"}, "PA"},
      {{"TPU", "TPE", "FLEX"}, "TPU"},
      {{"PLA", "PLA+", "PLA PRO", "PLA-S"}, "PLA"},
  };
  return a;
}

}  // namespace

const Material* find_material(std::string_view key) {
  for (const Material& m : builtin_materials())
    if (m.key == key) return &m;
  return nullptr;
}

std::optional<std::string> guess_material(std::string_view filament_type) {
  const std::string t = util::to_upper(util::trim(filament_type));
  if (t.empty()) return std::nullopt;
  for (const Alias& a : aliases())
    for (std::string_view k : a.keys)
      if (t == k || t.starts_with(k)) return std::string(a.material);
  for (const Alias& a : aliases())
    for (std::string_view k : a.keys)
      if (t.find(k) != std::string::npos) return std::string(a.material);
  return std::nullopt;
}

Mat6 orthotropic_stiffness(double E1, double E2, double E3, double G12, double G13, double G23, double nu12,
                           double nu13, double nu23) {
  Mat6 s;
  s(0, 0) = 1 / E1;
  s(1, 1) = 1 / E2;
  s(2, 2) = 1 / E3;
  s(0, 1) = s(1, 0) = -nu12 / E1;
  s(0, 2) = s(2, 0) = -nu13 / E1;
  s(1, 2) = s(2, 1) = -nu23 / E2;
  s(3, 3) = 1 / G23;
  s(4, 4) = 1 / G13;
  s(5, 5) = 1 / G12;
  const auto c = linalg::inverse(s);
  if (!c || !linalg::is_positive_definite(*c))
    throw std::invalid_argument(
        "Свойства материала дают неположительно определённую матрицу жёсткости "
        "(проверьте коэффициенты Пуассона).");
  return *c;
}

Mat6 stiffness(const Material& m) {
  return orthotropic_stiffness(m.E1, m.E2, m.E3, m.G12, m.G13, m.G23, m.nu12, m.nu13, m.nu23);
}

double infill_kz(std::string_view pattern) {
  std::string p;
  for (char c : pattern)
    if (c != '_' && c != ' ') p += util::to_lower(c);
  static const std::vector<std::pair<std::string_view, double>> table = {
      {"grid", 1.0},          {"triangles", 1.0},         {"tri-hexagon", 1.0},     {"trihexagon", 1.0},
      {"honeycomb", 1.0},     {"3dhoneycomb", 0.5},       {"cubic", 0.75},          {"adaptivecubic", 0.6},
      {"supportcubic", 0.4},  {"gyroid", 0.5},            {"rectilinear", 0.35},    {"alignedrectilinear", 0.35},
      {"line", 0.35},         {"lines", 0.35},            {"zig-zag", 0.35},        {"zigzag", 0.35},
      {"crosshatch", 0.45},   {"cross", 0.3},             {"concentric", 1.0},      {"lightning", 0.1},
      {"octagram", 0.4},      {"archimedeanchords", 0.4}, {"hilbertcurve", 0.4},    {"quartercubic", 0.7},
      {"tpmsd", 0.5},         {"tpmsfk", 0.5},
  };
  for (const auto& [k, v] : table)
    if (k == p) return v;
  return 0.6;
}

Mat6 rotate_z(const Mat6& c_local, double theta) {
  const auto r = rot_z(theta);
  const Tensor4 t = voigt_to_tensor4(c_local);
  Tensor4 g{};
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      for (int k = 0; k < 3; ++k)
        for (int l = 0; l < 3; ++l) {
          double s = 0.0;
          for (int a = 0; a < 3; ++a) {
            const double ria = r(static_cast<std::size_t>(i), static_cast<std::size_t>(a));
            if (ria == 0.0) continue;
            for (int b = 0; b < 3; ++b) {
              const double rjb = r(static_cast<std::size_t>(j), static_cast<std::size_t>(b));
              if (rjb == 0.0) continue;
              for (int c = 0; c < 3; ++c) {
                const double rkc = r(static_cast<std::size_t>(k), static_cast<std::size_t>(c));
                if (rkc == 0.0) continue;
                for (int d = 0; d < 3; ++d)
                  s += ria * rjb * rkc * r(static_cast<std::size_t>(l), static_cast<std::size_t>(d)) *
                       t[t4(a, b, c, d)];
              }
            }
          }
          g[t4(i, j, k, l)] = s;
        }
  return tensor4_to_voigt(g);
}

Mat6 strain_to_local(double theta) {
  const auto r = rot_z(theta);
  const auto rt = linalg::transpose(r);
  Mat6 m;
  for (int J = 0; J < 6; ++J) {
    // единичная инженерная деформация J → тензор (сдвиги пополам)
    linalg::Mat3 t;
    for (int I = 0; I < 6; ++I) {
      const auto [i, j] = kVoigt[static_cast<std::size_t>(I)];
      const double e = (I == J) ? 1.0 : 0.0;
      const double v = I < 3 ? e : e / 2;
      t(static_cast<std::size_t>(i), static_cast<std::size_t>(j)) = v;
      t(static_cast<std::size_t>(j), static_cast<std::size_t>(i)) = v;
    }
    const auto tl = rt * t * r;
    for (int I = 0; I < 6; ++I) {
      const auto [i, j] = kVoigt[static_cast<std::size_t>(I)];
      const double v = tl(static_cast<std::size_t>(i), static_cast<std::size_t>(j));
      m(static_cast<std::size_t>(I), static_cast<std::size_t>(J)) = I < 3 ? v : 2 * v;
    }
  }
  return m;
}

TemperatureFactor temperature_factor(const Material& m, std::optional<double> T) {
  if (!T || *T <= kRefTemperature) return {};
  const double t = *T;
  if (t <= m.hdt) return {1.0 - 0.4 * (t - kRefTemperature) / std::max(m.hdt - kRefTemperature, 1.0), {}};
  const double top = std::max(m.tg, m.hdt) + 15.0;
  const double f = 0.6 * std::max(0.05, 1.0 - (t - m.hdt) / std::max(top - m.hdt, 5.0));
  return {f, std::format("Температура {:.0f} °C выше теплостойкости {} (HDT ≈ {:.0f} °C): деталь будет "
                         "размягчаться и «плыть» под нагрузкой.",
                         t, m.name, m.hdt)};
}

double fatigue_factor(const Material& m, double cycles) {
  if (!(cycles > 1)) return 1.0;
  return std::min(1.0, std::pow(cycles, -1.0 / m.fatigue_k));
}

HoffmanCoeffs hoffman_coeffs(const Material& m, double f) {
  const double Xt = m.Xt * f, Xc = m.Xc * f, Yt = m.Yt * f, Yc = m.Yc * f, Zt = m.Zt * f, Zc = m.Zc * f;
  const double S12 = m.S12 * f, S13 = m.S13 * f, S23 = m.S23 * f;
  HoffmanCoeffs h{};
  h.C1 = 0.5 * (1 / (Yt * Yc) + 1 / (Zt * Zc) - 1 / (Xt * Xc));
  h.C2 = 0.5 * (1 / (Zt * Zc) + 1 / (Xt * Xc) - 1 / (Yt * Yc));
  h.C3 = 0.5 * (1 / (Xt * Xc) + 1 / (Yt * Yc) - 1 / (Zt * Zc));
  h.C4 = 1 / Xt - 1 / Xc;
  h.C5 = 1 / Yt - 1 / Yc;
  h.C6 = 1 / Zt - 1 / Zc;
  h.C7 = 1 / (S23 * S23);
  h.C8 = 1 / (S13 * S13);
  h.C9 = 1 / (S12 * S12);
  return h;
}

double hoffman_sf(const Vec6& sig, const HoffmanCoeffs& hc) {
  const double s1 = sig[0], s2 = sig[1], s3 = sig[2], t23 = sig[3], t13 = sig[4], t12 = sig[5];
  double a = hc.C1 * (s2 - s3) * (s2 - s3) + hc.C2 * (s3 - s1) * (s3 - s1) + hc.C3 * (s1 - s2) * (s1 - s2) +
             hc.C7 * t23 * t23 + hc.C8 * t13 * t13 + hc.C9 * t12 * t12;
  const double b = hc.C4 * s1 + hc.C5 * s2 + hc.C6 * s3;
  a = std::max(a, 0.0);
  const double disc = std::sqrt(b * b + 4 * a);
  double lam;
  if (a > 1e-30)
    lam = (-b + disc) / (2 * a);
  else if (b > 1e-30)
    lam = 1.0 / b;
  else
    lam = std::numeric_limits<double>::infinity();
  return std::isfinite(lam) ? lam : 1e6;
}

std::string_view failure_mode_name(FailureMode mode) {
  switch (mode) {
    case FailureMode::FiberTension: return "Разрыв вдоль нити";
    case FailureMode::FiberCompression: return "Смятие вдоль нити";
    case FailureMode::TransverseTension: return "Отрыв соседних нитей (в слое)";
    case FailureMode::TransverseCompression: return "Смятие поперёк нити";
    case FailureMode::InterlayerTension: return "Расслоение между слоями (отрыв по Z)";
    case FailureMode::InterlayerCompression: return "Смятие по Z";
    case FailureMode::InterlayerShear: return "Межслойный сдвиг";
    case FailureMode::InPlaneShear: return "Сдвиг в плоскости слоя";
  }
  return "?";
}

FailureMode failure_mode(const Vec6& sig, const Material& m) {
  const double s1 = sig[0], s2 = sig[1], s3 = sig[2], t23 = sig[3], t13 = sig[4], t12 = sig[5];
  const std::array<double, kFailureModeCount> r = {
      s1 > 0 ? s1 / m.Xt : 0.0,
      s1 < 0 ? -s1 / m.Xc : 0.0,
      s2 > 0 ? s2 / m.Yt : 0.0,
      s2 < 0 ? -s2 / m.Yc : 0.0,
      s3 > 0 ? s3 / m.Zt : 0.0,
      s3 < 0 ? -s3 / m.Zc : 0.0,
      std::max(std::abs(t13) / m.S13, std::abs(t23) / m.S23),
      std::abs(t12) / m.S12,
  };
  // как numpy.argmax: при равенстве — первый
  int best = 0;
  for (int k = 1; k < kFailureModeCount; ++k)
    if (r[static_cast<std::size_t>(k)] > r[static_cast<std::size_t>(best)]) best = k;
  return static_cast<FailureMode>(best);
}

LaminateCheck laminate_check(const Material& m, const std::vector<double>& angles_deg, int load_dir) {
  const Mat6 c = stiffness(m);
  Mat6 cavg;
  for (double a : angles_deg) cavg = cavg + rotate_z(c, a * std::numbers::pi / 180.0);
  cavg = (1.0 / static_cast<double>(angles_deg.size())) * cavg;
  const auto sav = linalg::inverse(cavg);
  if (!sav) throw std::runtime_error("laminate_check: вырожденная матрица");
  if (load_dir == 2) return {m.Zt, 1.0 / (*sav)(2, 2)};
  const Vec6 unit = {1, 0, 0, 0, 0, 0};
  const Vec6 eps = *sav * unit;
  const auto hc = hoffman_coeffs(m);
  double sf = std::numeric_limits<double>::infinity();
  for (double a : angles_deg) {
    const Vec6 sig = c * (strain_to_local(a * std::numbers::pi / 180.0) * eps);
    sf = std::min(sf, hoffman_sf(sig, hc));
  }
  return {sf, 1.0 / (*sav)(0, 0)};
}

}  // namespace kika::material
