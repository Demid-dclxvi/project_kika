// Рисунки разреженного заполнения.
//
// Все рисунки построены здесь заново из геометрии (код AGPL-слайсеров не используется). Шаг
// каждого рисунка выбран так, чтобы доля объёма пластика в области была равна плотности:
// у валика сечение spacing·h, поэтому на площадь A приходится длина линий density·A/spacing.
// Рисунки привязаны к общему началу координат и к высоте — на соседних слоях линии ложатся
// друг на друга (или, у объёмных рисунков, плавно сдвигаются).

#include "infill.hpp"

#include <algorithm>
#include <numbers>

namespace kika::slicer::detail {

namespace {

constexpr double kPi = std::numbers::pi;

// Длина линий гироида в сечении на единицу площади при периоде 1, среднее по высоте: по
// стереологии (π/4)·S_V ≈ 2,43, уточнено по нарезке — доля объёма совпадает с плотностью до 1 %
// (тест «Слайсер: рисунки заполнения»).
constexpr double kGyroidLengthPerArea = 2.448;
// Средний коэффициент длины зигзага 3D-сот: наклон зигзага меняется с высотой от 0 до 1,
// среднее √(1 + s²) по s ∈ [0, 1] = (√2 + asinh 1) / 2.
constexpr double kHoney3dLengthFactor = 1.1478;

double frac(double x) { return x - std::floor(x); }
// Треугольная волна: 0 при целых u, 1 при полуцелых.
double tri01(double u) { return 1.0 - std::abs(2.0 * frac(u) - 1.0); }
long long floor_ll(double x) { return static_cast<long long>(std::floor(x)); }
long long ceil_ll(double x) { return static_cast<long long>(std::ceil(x)); }

void add_hatch(std::vector<FillPath>& out, const Paths64& area, double spacing, double angle, double phase, double min_len) {
  for (auto& l : hatch(area, spacing, angle, phase, min_len)) out.push_back({std::move(l), false});
}

void add_clipped(std::vector<FillPath>& out, const Paths64& area, const std::vector<Polyline>& raw, double min_len) {
  for (auto& l : clip_open(area, raw, min_len)) out.push_back({std::move(l), false});
}

// ---------------------------------------------------------------------------- гироид

// Сечение гироида sin X·cos Y + sin Y·cos Z + sin Z·cos X = 0 плоскостью Z: изолиния на сетке
// (марширующие квадраты), отрезки склеиваются в ломаные по общим рёбрам ячеек.
std::vector<Polyline> gyroid(const Frame& f, double period, double z) {
  const double step = period / 24.0;
  const double u0 = std::floor(f.umin / step) * step - step, v0 = std::floor(f.vmin / step) * step - step;
  const auto nu = static_cast<std::size_t>(std::ceil((f.umax - u0) / step)) + 2;
  const auto nv = static_cast<std::size_t>(std::ceil((f.vmax - v0) / step)) + 2;
  const double k = 2 * kPi / period;
  std::vector<double> su(nu + 1), cu(nu + 1), sv(nv + 1), cv(nv + 1);
  for (std::size_t i = 0; i <= nu; ++i) {
    su[i] = std::sin(k * (u0 + static_cast<double>(i) * step));
    cu[i] = std::cos(k * (u0 + static_cast<double>(i) * step));
  }
  for (std::size_t j = 0; j <= nv; ++j) {
    sv[j] = std::sin(k * (v0 + static_cast<double>(j) * step));
    cv[j] = std::cos(k * (v0 + static_cast<double>(j) * step));
  }
  const double sz = std::sin(k * z), cz = std::cos(k * z);
  auto val = [&](std::size_t i, std::size_t j) { return su[i] * cv[j] + sv[j] * cz + sz * cu[i]; };
  std::vector<double> fv((nu + 1) * (nv + 1));
  for (std::size_t j = 0; j <= nv; ++j)
    for (std::size_t i = 0; i <= nu; ++i) fv[j * (nu + 1) + i] = val(i, j);
  auto at = [&](std::size_t i, std::size_t j) { return fv[j * (nu + 1) + i]; };
  // рёбра: горизонтальные (i, j)–(i+1, j), затем вертикальные (i, j)–(i, j+1)
  const std::size_t nh = nu * (nv + 1);
  auto eh = [&](std::size_t i, std::size_t j) { return j * nu + i; };
  auto ev = [&](std::size_t i, std::size_t j) { return nh + j * (nu + 1) + i; };
  auto edge_point = [&](std::size_t e) -> Pt {
    std::size_t i, j, i2, j2;
    if (e < nh) {
      j = e / nu;
      i = e % nu;
      i2 = i + 1;
      j2 = j;
    } else {
      const std::size_t r = e - nh;
      j = r / (nu + 1);
      i = r % (nu + 1);
      i2 = i;
      j2 = j + 1;
    }
    const double fa = at(i, j), fb = at(i2, j2);
    const double t = fa / (fa - fb);
    const double u = u0 + (static_cast<double>(i) + t * static_cast<double>(i2 - i)) * step;
    const double v = v0 + (static_cast<double>(j) + t * static_cast<double>(j2 - j)) * step;
    return f.to_world(u, v);
  };
  struct Seg {
    std::size_t a, b;
  };
  std::vector<Seg> segs;
  for (std::size_t j = 0; j < nv; ++j)
    for (std::size_t i = 0; i < nu; ++i) {
      const double f0 = at(i, j), f1 = at(i + 1, j), f2 = at(i + 1, j + 1), f3 = at(i, j + 1);
      const bool b0 = f0 >= 0, b1 = f1 >= 0, b2 = f2 >= 0, b3 = f3 >= 0;
      const std::size_t e0 = eh(i, j), e1 = ev(i + 1, j), e2 = eh(i, j + 1), e3 = ev(i, j);
      std::size_t cut[4];
      int n = 0;
      if (b0 != b1) cut[n++] = e0;
      if (b1 != b2) cut[n++] = e1;
      if (b2 != b3) cut[n++] = e2;
      if (b3 != b0) cut[n++] = e3;
      if (n == 2) {
        segs.push_back({cut[0], cut[1]});
      } else if (n == 4) {
        // седло: по знаку в центре ячейки
        const bool c = (f0 + f1 + f2 + f3) >= 0;
        if (c == b0) {
          segs.push_back({e0, e1});
          segs.push_back({e2, e3});
        } else {
          segs.push_back({e3, e0});
          segs.push_back({e1, e2});
        }
      }
    }
  // склейка по рёбрам
  const std::size_t ne = nh + (nu + 1) * nv;
  std::vector<std::int32_t> s1(ne, -1), s2(ne, -1);
  for (std::size_t s = 0; s < segs.size(); ++s)
    for (std::size_t e : {segs[s].a, segs[s].b}) (s1[e] < 0 ? s1[e] : s2[e]) = static_cast<std::int32_t>(s);
  std::vector<char> used(segs.size(), 0);
  std::vector<Polyline> out;
  auto walk = [&](std::size_t s0, std::size_t from_edge) {
    Polyline pl{edge_point(from_edge)};
    std::size_t s = s0, e = from_edge;
    for (;;) {
      used[s] = 1;
      const std::size_t next_e = segs[s].a == e ? segs[s].b : segs[s].a;
      pl.push_back(edge_point(next_e));
      const std::int32_t ns = s1[next_e] == static_cast<std::int32_t>(s) ? s2[next_e] : s1[next_e];
      if (ns < 0 || used[static_cast<std::size_t>(ns)]) break;
      s = static_cast<std::size_t>(ns);
      e = next_e;
    }
    out.push_back(std::move(pl));
  };
  for (std::size_t e = 0; e < ne; ++e)  // сначала открытые ломаные — от края сетки
    if (s1[e] >= 0 && s2[e] < 0 && !used[static_cast<std::size_t>(s1[e])]) walk(static_cast<std::size_t>(s1[e]), e);
  for (std::size_t s = 0; s < segs.size(); ++s)
    if (!used[s]) walk(s, segs[s].a);
  // лишние точки: отклонение от кривой до 0,02 мм — меньше точности принтера, а G-code втрое короче
  for (auto& pl : out) {
    Path64 p = to_path(pl);
    pl = to_pts(Clipper2Lib::RamerDouglasPeucker(p, 0.02 * kScale));
  }
  return out;
}

// ---------------------------------------------------------------------------- соты

// Правильные шестиугольники с плоским верхом, расстояние между параллельными стенками d.
// Стенки на границе соседних столбцов — один зигзаг на всю высоту, горизонтальные стенки
// внутри столбца — короткие отрезки. Каждая стенка печатается один раз.
std::vector<Polyline> honeycomb(const Frame& f, double d) {
  const double s = d / std::sqrt(3.0), hh = d / 2;
  std::vector<Polyline> raw;
  const long long j0 = floor_ll((f.umin - 2 * s) / (1.5 * s)), j1 = ceil_ll((f.umax + 2 * s) / (1.5 * s));
  const long long k0 = floor_ll((f.vmin - d) / hh), k1 = ceil_ll((f.vmax + d) / hh);
  for (long long j = j0; j <= j1; ++j) {
    const double xj = 1.5 * s * static_cast<double>(j);
    const long long pj = ((j % 2) + 2) % 2;
    Polyline zig;
    for (long long k = k0; k <= k1; ++k) {
      const bool even = ((k + pj) % 2 + 2) % 2 == 0;
      zig.push_back(f.to_world(xj + (even ? s : s / 2), static_cast<double>(k) * hh));
      if (!even) raw.push_back({f.to_world(xj - s / 2, static_cast<double>(k) * hh), f.to_world(xj + s / 2, static_cast<double>(k) * hh)});
    }
    if (j % 2 != 0) std::reverse(zig.begin(), zig.end());
    raw.push_back(std::move(zig));
  }
  return raw;
}

// 3D-соты: на слое — зигзаги одного направления (через слой — поперёк), размах зигзага
// меняется с высотой треугольной волной от 0 до половины шага. Соседние зигзаги — зеркальны:
// при наибольшем размахе они сходятся вершинами, и слои складываются в замкнутые ячейки.
std::vector<Polyline> honeycomb3d(const Frame& f, double d, double z, std::size_t layer) {
  const double amp = 0.5 * d * tri01(z / (std::sqrt(2.0) * d));
  const bool along_u = layer % 2 == 0;
  const double a0 = along_u ? f.umin : f.vmin, a1 = along_u ? f.umax : f.vmax;  // вдоль линий
  const double b0 = along_u ? f.vmin : f.umin, b1 = along_u ? f.vmax : f.umax;  // поперёк
  std::vector<Polyline> raw;
  const long long m0 = floor_ll(a0 / d) - 1, m1 = ceil_ll(a1 / d) + 1;
  for (long long k = floor_ll(b0 / d) - 1; k <= ceil_ll(b1 / d) + 1; ++k) {
    Polyline pl;
    for (long long m = m0; m <= m1; ++m) {
      const double sign = ((k + m) % 2 + 2) % 2 == 0 ? 1.0 : -1.0;
      const double a = static_cast<double>(m) * d, b = static_cast<double>(k) * d + sign * amp;
      pl.push_back(along_u ? f.to_world(a, b) : f.to_world(b, a));
    }
    if (k % 2 != 0) std::reverse(pl.begin(), pl.end());
    raw.push_back(std::move(pl));
  }
  return raw;
}

}  // namespace

std::vector<FillPath> sparse_infill(const Paths64& area, const InfillParams& p) {
  std::vector<FillPath> out;
  if (area.empty() || p.density <= 0) return out;
  const double d = std::clamp(p.density, 0.01, 1.0);
  const double sp = p.spacing, a = p.angle, min_len = p.line_width;
  switch (p.pattern) {
    case Pattern::Grid:
      add_hatch(out, area, 2 * sp / d, a, 0, min_len);
      add_hatch(out, area, 2 * sp / d, a - 90, 0, min_len);
      break;
    case Pattern::Rectilinear:
      add_hatch(out, area, sp / d, p.layer % 2 == 0 ? a : a - 90, 0, min_len);
      break;
    case Pattern::Lines:
      add_hatch(out, area, sp / d, a, 0, min_len);
      break;
    case Pattern::Triangles:
      for (double t : {-45.0, 15.0, 75.0}) add_hatch(out, area, 3 * sp / d, a + t, 0, min_len);
      break;
    case Pattern::TriHexagon: {
      // три семейства через 60°, одно сдвинуто на полшага: треугольники и шестиугольники
      const double step = 3 * sp / d;
      add_hatch(out, area, step, a, 0, min_len);
      add_hatch(out, area, step, a + 60, 0, min_len);
      add_hatch(out, area, step, a + 120, step / 2, min_len);
      break;
    }
    case Pattern::Cubic: {
      // кубы, поставленные на вершину: три семейства плоскостей граней куба в сечении — три
      // семейства линий через 120°, которые с высотой z сдвигаются на z/√2 по своей нормали
      const double step = 3 * sp / d;
      for (double t : {0.0, 120.0, 240.0}) add_hatch(out, area, step, a + t, -p.z / std::sqrt(2.0), min_len);
      break;
    }
    case Pattern::Gyroid:
      add_clipped(out, area, gyroid(frame_of(area, a), kGyroidLengthPerArea * sp / d, p.z), min_len);
      break;
    case Pattern::Honeycomb:
      add_clipped(out, area, honeycomb(frame_of(area, a), 2 * sp / d), min_len);
      break;
    case Pattern::Honeycomb3D:
      add_clipped(out, area, honeycomb3d(frame_of(area, a), kHoney3dLengthFactor * sp / d, p.z, p.layer), min_len);
      break;
    case Pattern::Concentric: {
      const double step = sp / d;
      for (int k = 0;; ++k) {
        Paths64 loops = offset(area, -(step / 2 + k * step));
        if (loops.empty()) break;
        for (const auto& l : loops)
          if (l.size() >= 3 && std::abs(Clipper2Lib::Area(l)) > p.line_width * p.line_width * kScale * kScale)
            out.push_back({to_pts(l), true});
      }
      break;
    }
  }
  return out;
}

}  // namespace kika::slicer::detail
