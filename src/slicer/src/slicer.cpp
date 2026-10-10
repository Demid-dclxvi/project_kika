// Свой слайсер: сечения → стенки → заполнение → G-code.
//
// Сечение слоя — в середине его высоты. Отрезки сечения склеиваются в контуры по рёбрам сетки
// (у соседних треугольников общее ребро — общая точка), направление — по нормали треугольника,
// поэтому внешние контуры идут против часовой стрелки, отверстия — по часовой, и правило
// «ненулевой обмотки» правильно объединяет пересекающиеся тела.
// Стенки — смещения контура внутрь, шаг между валиками как в Slic3r/Orca: w − h·(1 − π/4).
// Сплошные слои — где сверху или снизу в пределах top/bottom слоёв нет материала.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <numbers>
#include <numeric>
#include <unordered_map>

#include <clipper2/clipper.h>

#include "kika/slicer/slicer.hpp"
#include "kika/version.hpp"

namespace kika::slicer {

namespace {

using Clipper2Lib::ClipType;
using Clipper2Lib::FillRule;
using Clipper2Lib::JoinType;
using Clipper2Lib::EndType;
using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;
using geometry::TriangleMesh;
using geometry::Vec3;

constexpr double kScale = 10000.0;  // единиц Clipper в миллиметре (0,1 мкм)

std::int64_t q(double v) { return std::llround(v * kScale); }
double mm(std::int64_t v) { return static_cast<double>(v) / kScale; }

using Pt = std::array<double, 2>;

// Роль линии и её название в G-code (как у OrcaSlicer — их понимает разбор G-code).
enum class Role { OuterWall, InnerWall, Solid, Top, Bottom, Sparse, Skirt };

const char* role_label(Role r) {
  switch (r) {
    case Role::OuterWall: return "Outer wall";
    case Role::InnerWall: return "Inner wall";
    case Role::Solid: return "Internal solid infill";
    case Role::Top: return "Top surface";
    case Role::Bottom: return "Bottom surface";
    case Role::Sparse: return "Sparse infill";
    case Role::Skirt: return "Skirt";
  }
  return "Custom";
}

struct Line {
  Role role;
  std::vector<Pt> pts;
  bool closed = false;
};

struct Layer {
  double z = 0, h = 0;
  std::vector<Line> lines;
};

// ---------------------------------------------------------------------------- сечения

struct Cut {
  Point64 a, b;
  std::uint64_t ka, kb;  // рёбра сетки, на которых лежат концы
};

std::uint64_t edge_key(std::uint32_t i, std::uint32_t j) {
  return (static_cast<std::uint64_t>(std::min(i, j)) << 32) | std::max(i, j);
}

// Точка на ребре (i, j) на высоте z. Считается всегда от меньшего номера вершины к большему,
// чтобы у двух треугольников с общим ребром точка совпала до бита.
Point64 edge_point(const TriangleMesh& m, std::uint32_t i, std::uint32_t j, double z) {
  if (i > j) std::swap(i, j);
  const Vec3& p = m.vertices[i];
  const Vec3& r = m.vertices[j];
  const double t = (z - p[2]) / (r[2] - p[2]);
  return {q(p[0] + t * (r[0] - p[0])), q(p[1] + t * (r[1] - p[1]))};
}

// Склеить отрезки сечения в замкнутые контуры. Незамкнутые цепочки (дыры в сетке) соединяются
// с ближайшими, если щель меньше tol; остальные отбрасываются и считаются в open.
Paths64 chain(const std::vector<Cut>& cuts, std::size_t& open) {
  std::unordered_map<std::uint64_t, std::uint32_t> start_of;
  start_of.reserve(cuts.size() * 2);
  for (std::uint32_t i = 0; i < cuts.size(); ++i) start_of.emplace(cuts[i].ka, i);
  std::vector<char> used(cuts.size(), 0);
  Paths64 loops;
  std::vector<Path64> chains;
  for (std::uint32_t s = 0; s < cuts.size(); ++s) {
    if (used[s]) continue;
    Path64 path;
    std::uint32_t cur = s;
    bool closed = false;
    for (;;) {
      used[cur] = 1;
      path.push_back(cuts[cur].a);
      const auto it = start_of.find(cuts[cur].kb);
      if (it == start_of.end()) {
        path.push_back(cuts[cur].b);
        break;
      }
      if (it->second == s) {
        closed = true;
        break;
      }
      if (used[it->second]) {
        path.push_back(cuts[cur].b);
        break;
      }
      cur = it->second;
    }
    if (closed) {
      if (path.size() >= 3) loops.push_back(std::move(path));
    } else {
      chains.push_back(std::move(path));
    }
  }
  // незамкнутые цепочки: стыкуем конец с ближайшим началом
  const double tol2 = std::pow(1.0 * kScale, 2);
  auto d2 = [](const Point64& a, const Point64& b) {
    const double dx = static_cast<double>(a.x - b.x), dy = static_cast<double>(a.y - b.y);
    return dx * dx + dy * dy;
  };
  std::vector<char> taken(chains.size(), 0);
  for (std::size_t i = 0; i < chains.size(); ++i) {
    if (taken[i]) continue;
    taken[i] = 1;
    Path64 path = std::move(chains[i]);
    for (;;) {
      if (path.size() >= 3 && d2(path.back(), path.front()) <= tol2) {
        loops.push_back(std::move(path));
        path.clear();
        break;
      }
      std::size_t best = chains.size();
      double bd = tol2;
      for (std::size_t j = 0; j < chains.size(); ++j)
        if (!taken[j] && d2(path.back(), chains[j].front()) <= bd) {
          bd = d2(path.back(), chains[j].front());
          best = j;
        }
      if (best == chains.size()) break;
      taken[best] = 1;
      path.insert(path.end(), chains[best].begin(), chains[best].end());
    }
    if (!path.empty()) ++open;
  }
  return loops;
}

// Сечения на высотах zs (по возрастанию) за один проход по треугольникам.
std::vector<Paths64> slice_layers(const TriangleMesh& m, const std::vector<double>& zs, std::size_t& open,
                                  const Progress& progress) {
  const std::size_t nt = m.triangles.size();
  std::vector<double> tmin(nt), tmax(nt);
  for (std::size_t t = 0; t < nt; ++t) {
    const auto& tr = m.triangles[t];
    tmin[t] = std::min({m.vertices[tr[0]][2], m.vertices[tr[1]][2], m.vertices[tr[2]][2]});
    tmax[t] = std::max({m.vertices[tr[0]][2], m.vertices[tr[1]][2], m.vertices[tr[2]][2]});
  }
  std::vector<std::uint32_t> order(nt);
  std::iota(order.begin(), order.end(), 0u);
  std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) { return tmin[a] < tmin[b]; });
  std::vector<Paths64> out(zs.size());
  std::vector<std::uint32_t> active;
  std::vector<Cut> cuts;
  std::size_t next = 0;
  for (std::size_t li = 0; li < zs.size(); ++li) {
    const double z = zs[li];
    while (next < nt && tmin[order[next]] < z) active.push_back(order[next++]);
    std::erase_if(active, [&](std::uint32_t t) { return tmax[t] < z; });
    cuts.clear();
    for (auto t : active) {
      const auto& tr = m.triangles[t];
      // вершина на плоскости считается выше неё
      const bool up[3] = {m.vertices[tr[0]][2] >= z, m.vertices[tr[1]][2] >= z, m.vertices[tr[2]][2] >= z};
      if (up[0] == up[1] && up[1] == up[2]) continue;
      // Направление — чтобы тело было слева (d = ẑ × n). У треугольника с наружной нормалью
      // (вершины против часовой стрелки снаружи) отрезок начинается на ребре, которое в порядке
      // вершин идёт сверху вниз, и кончается на ребре снизу вверх. Правило не зависит от длины
      // отрезка: отрезок нулевой длины (вершина на плоскости, узкий треугольник) тоже остаётся
      // в цепочке — иначе контур рвётся на стыке рёбер.
      Cut cut{Point64(0, 0), Point64(0, 0), 0, 0};
      int found = 0;
      for (int e = 0; e < 3; ++e) {
        const int f = (e + 1) % 3;
        if (up[e] == up[f]) continue;
        const auto i = tr[static_cast<std::size_t>(e)], j = tr[static_cast<std::size_t>(f)];
        if (up[e]) {
          cut.a = edge_point(m, i, j, z);
          cut.ka = edge_key(i, j);
        } else {
          cut.b = edge_point(m, i, j, z);
          cut.kb = edge_key(i, j);
        }
        ++found;
      }
      if (found == 2) cuts.push_back(cut);
    }
    Paths64 loops = chain(cuts, open);
    if (!loops.empty()) {
      Paths64 u = Clipper2Lib::Union(loops, FillRule::NonZero);
      out[li] = Clipper2Lib::SimplifyPaths(u, 0.003 * kScale, true);
    }
    if (progress && li % 16 == 0) progress(0.3 * static_cast<double>(li) / static_cast<double>(zs.size()), "Сечения слоёв");
  }
  return out;
}

// ---------------------------------------------------------------------------- линии

Paths64 offset(const Paths64& p, double delta_mm) {
  if (p.empty()) return {};
  return Clipper2Lib::InflatePaths(p, delta_mm * kScale, JoinType::Miter, EndType::Polygon, 3.0);
}

// Параллельные линии с шагом spacing под углом angle (градусы) внутри области. Линии привязаны
// к началу координат, поэтому на соседних слоях лежат точно друг над другом.
std::vector<std::vector<Pt>> hatch(const Paths64& area, double spacing, double angle, double phase, double min_len) {
  std::vector<std::vector<Pt>> out;
  if (area.empty() || spacing <= 0) return out;
  const double a = angle * std::numbers::pi / 180.0;
  const double ux = std::cos(a), uy = std::sin(a), nx = -uy, ny = ux;
  double tmin = 1e300, tmax = -1e300, smin = 1e300, smax = -1e300;
  for (const auto& path : area)
    for (const auto& p : path) {
      const double x = mm(p.x), y = mm(p.y);
      tmin = std::min(tmin, x * nx + y * ny);
      tmax = std::max(tmax, x * nx + y * ny);
      smin = std::min(smin, x * ux + y * uy);
      smax = std::max(smax, x * ux + y * uy);
    }
  Paths64 lines;
  for (double k = std::ceil((tmin - phase) / spacing); phase + k * spacing <= tmax; k += 1) {
    const double t = phase + k * spacing;
    lines.push_back({Point64(q(nx * t + ux * (smin - 1)), q(ny * t + uy * (smin - 1))),
                     Point64(q(nx * t + ux * (smax + 1)), q(ny * t + uy * (smax + 1)))});
  }
  if (lines.empty()) return out;
  Clipper2Lib::Clipper64 c;
  c.AddOpenSubject(lines);
  c.AddClip(area);
  Paths64 closed, open;
  c.Execute(ClipType::Intersection, FillRule::NonZero, closed, open);
  // по порядку: номер линии, затем положение вдоль неё; через одну — в обратную сторону
  struct Piece {
    long long k;
    double s0;
    std::vector<Pt> pts;
  };
  std::vector<Piece> pieces;
  for (const auto& path : open) {
    if (path.size() < 2) continue;
    std::vector<Pt> pts;
    for (const auto& p : path) pts.push_back({mm(p.x), mm(p.y)});
    const double len = std::hypot(pts.back()[0] - pts.front()[0], pts.back()[1] - pts.front()[1]);
    if (len < min_len) continue;
    const double s0 = pts.front()[0] * ux + pts.front()[1] * uy, s1 = pts.back()[0] * ux + pts.back()[1] * uy;
    if (s1 < s0) std::reverse(pts.begin(), pts.end());
    const double t = pts.front()[0] * nx + pts.front()[1] * ny;
    pieces.push_back({std::llround((t - phase) / spacing), std::min(s0, s1), std::move(pts)});
  }
  std::sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) { return x.k != y.k ? x.k < y.k : x.s0 < y.s0; });
  for (auto& p : pieces) {
    if (p.k % 2 != 0) std::reverse(p.pts.begin(), p.pts.end());
    out.push_back(std::move(p.pts));
  }
  return out;
}

std::vector<Pt> to_pts(const Path64& p) {
  std::vector<Pt> v;
  v.reserve(p.size());
  for (const auto& x : p) v.push_back({mm(x.x), mm(x.y)});
  return v;
}

// Острова слоя: внешний контур со своими отверстиями.
std::vector<Paths64> islands(const Paths64& region) {
  std::vector<Paths64> out;
  if (region.empty()) return out;
  Clipper2Lib::PolyTree64 tree;
  Clipper2Lib::Clipper64 c;
  c.AddSubject(region);
  c.Execute(ClipType::Union, FillRule::NonZero, tree);
  std::vector<const Clipper2Lib::PolyPath64*> stack;
  for (const auto& child : tree) stack.push_back(child.get());
  while (!stack.empty()) {
    const auto* node = stack.back();
    stack.pop_back();
    Paths64 isl{node->Polygon()};
    for (const auto& hole : *node) {
      isl.push_back(hole->Polygon());
      for (const auto& inner : *hole) stack.push_back(inner.get());  // остров внутри отверстия
    }
    out.push_back(std::move(isl));
  }
  return out;
}

double area_mm2(const Paths64& p) { return Clipper2Lib::Area(p) / (kScale * kScale); }

// Маленькие куски разреженного заполнения печатаются сплошными (как solid_infill_threshold_area в Orca).
void move_small_to_solid(Paths64& sparse, Paths64& solid, double threshold_mm2) {
  if (sparse.empty()) return;
  Paths64 keep, small;
  for (auto& isl : islands(sparse)) {
    if (area_mm2(isl) < threshold_mm2)
      small.insert(small.end(), isl.begin(), isl.end());
    else
      keep.insert(keep.end(), isl.begin(), isl.end());
  }
  if (!small.empty()) solid = Clipper2Lib::Union(solid, small, FillRule::NonZero);
  sparse = std::move(keep);
}

// ---------------------------------------------------------------------------- порядок печати

double dist2(const Pt& a, const Pt& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }

// Замкнутый контур начинаем с вершины, ближайшей к текущему положению сопла.
void rotate_to_nearest(std::vector<Pt>& loop, const Pt& from) {
  std::size_t best = 0;
  double bd = 1e300;
  for (std::size_t i = 0; i < loop.size(); ++i)
    if (dist2(loop[i], from) < bd) {
      bd = dist2(loop[i], from);
      best = i;
    }
  std::rotate(loop.begin(), loop.begin() + static_cast<std::ptrdiff_t>(best), loop.end());
}

// Открытые линии — жадно: следующая та, чей ближайший конец ближе всего (линия может развернуться).
void add_open_lines(std::vector<Line>& out, std::vector<std::vector<Pt>> lines, Role role, Pt& pos) {
  std::vector<char> done(lines.size(), 0);
  for (std::size_t n = 0; n < lines.size(); ++n) {
    std::size_t best = 0;
    bool flip = false;
    double bd = 1e300;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (done[i]) continue;
      const double d0 = dist2(lines[i].front(), pos), d1 = dist2(lines[i].back(), pos);
      if (d0 < bd) bd = d0, best = i, flip = false;
      if (d1 < bd) bd = d1, best = i, flip = true;
    }
    done[best] = 1;
    auto& l = lines[best];
    if (flip) std::reverse(l.begin(), l.end());
    pos = l.back();
    out.push_back({role, std::move(l), false});
  }
}

// ---------------------------------------------------------------------------- G-code

class Writer {
 public:
  Writer(const Settings& s) : s_(s) {
    fil_area_ = std::numbers::pi * 0.25 * s.printer.filament_diameter * s.printer.filament_diameter;
  }

  std::string& text() { return out_; }
  void line(std::string_view l) {
    out_.append(l);
    out_.push_back('\n');
  }

  void travel(const Pt& p, bool allow_retract = true) {
    const double d = std::hypot(p[0] - x_, p[1] - y_);
    if (d < 1e-4) return;
    if (allow_retract && d > s_.retract_min_travel && !retracted_ && s_.retract_length > 0) {
      out_ += std::format("G1 E{:.5f} F{:.0f}\n", -s_.retract_length, s_.retract_speed * 60);
      retracted_ = true;
      time_ += s_.retract_length / s_.retract_speed;
    }
    out_ += std::format("G0 X{:.3f} Y{:.3f} F{:.0f}\n", p[0], p[1], s_.speed_travel * 60);
    time_ += d / s_.speed_travel;
    x_ = p[0];
    y_ = p[1];
  }

  void extrude(const std::vector<Pt>& pts, bool closed, double width, double h, double speed) {
    if (pts.size() < 2) return;
    travel(pts[0]);
    if (retracted_) {
      out_ += std::format("G1 E{:.5f} F{:.0f}\n", s_.retract_length, s_.retract_speed * 60);
      retracted_ = false;
      time_ += s_.retract_length / s_.retract_speed;
    }
    // площадь сечения валика: прямоугольник со скруглёнными боками (как в Slic3r/Orca)
    const double area = (width - h) * h + std::numbers::pi * 0.25 * h * h;
    const double e_per_mm = area / fil_area_;
    bool first = true;
    auto go = [&](const Pt& p) {
      const double l = std::hypot(p[0] - x_, p[1] - y_);
      if (l < 1e-4) return;
      const double e = l * e_per_mm;
      if (first)
        out_ += std::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}\n", p[0], p[1], e, speed * 60);
      else
        out_ += std::format("G1 X{:.3f} Y{:.3f} E{:.5f}\n", p[0], p[1], e);
      first = false;
      x_ = p[0];
      y_ = p[1];
      e_total_ += e;
      time_ += l / speed;
    };
    for (std::size_t i = 1; i < pts.size(); ++i) go(pts[i]);
    if (closed) go(pts[0]);
  }

  double filament() const { return e_total_; }
  double time() const { return time_; }
  void add_time(double t) { time_ += t; }

 private:
  const Settings& s_;
  std::string out_;
  double fil_area_ = 1;
  double x_ = 0, y_ = 0;
  bool retracted_ = false;
  double e_total_ = 0, time_ = 0;
};

std::string substitute(std::string t, const Settings& s) {
  const std::pair<const char*, std::string> vars[] = {
      {"{nozzle_temp}", std::format("{:.0f}", s.filament.nozzle_temp)},
      {"{first_layer_nozzle_temp}", std::format("{:.0f}", s.filament.first_layer_nozzle_temp)},
      {"{bed_temp}", std::format("{:.0f}", s.filament.bed_temp)},
      {"{first_layer_bed_temp}", std::format("{:.0f}", s.filament.first_layer_bed_temp)},
      {"{bed_x}", std::format("{:g}", s.printer.bed_x)},
      {"{bed_y}", std::format("{:g}", s.printer.bed_y)},
      {"{max_z}", std::format("{:g}", s.printer.max_z)},
      {"{filament_type}", s.filament.type},
  };
  for (const auto& [k, v] : vars)
    for (std::size_t p = t.find(k); p != std::string::npos; p = t.find(k, p + v.size())) t.replace(p, std::strlen(k), v);
  return t;
}

}  // namespace

// ============================================================================

std::vector<std::vector<std::array<double, 2>>> section(const TriangleMesh& mesh, double z) {
  std::size_t open = 0;
  const auto layers = slice_layers(mesh, {z}, open, {});
  std::vector<std::vector<std::array<double, 2>>> out;
  for (const auto& p : layers[0]) out.push_back(to_pts(p));
  return out;
}

Result slice(const TriangleMesh& mesh, const Settings& in, const Progress& progress, std::string_view model_name) {
  Settings s = in;
  Result res;
  if (mesh.empty()) throw std::invalid_argument("Модель пустая — нечего нарезать.");
  s.wall_loops = std::max(1, s.wall_loops);
  s.layer_height = std::clamp(s.layer_height, 0.04, 0.8 * s.printer.nozzle);
  s.first_layer_height = std::clamp(s.first_layer_height, 0.04, 0.8 * s.printer.nozzle);
  s.line_width = std::clamp(s.line_width, 0.6 * s.printer.nozzle, 2.0 * s.printer.nozzle);
  s.infill_density = std::clamp(s.infill_density, 0.0, 1.0);
  if (s.filament.nozzle_temp > s.printer.max_nozzle_temp || s.filament.first_layer_nozzle_temp > s.printer.max_nozzle_temp) {
    res.warnings.push_back(std::format("Температура сопла для {} выше предела принтера — снижена до {:.0f} °C.",
                                       s.filament.type, s.printer.max_nozzle_temp));
    s.filament.nozzle_temp = std::min(s.filament.nozzle_temp, s.printer.max_nozzle_temp);
    s.filament.first_layer_nozzle_temp = std::min(s.filament.first_layer_nozzle_temp, s.printer.max_nozzle_temp);
  }
  if (s.filament.bed_temp > s.printer.max_bed_temp || s.filament.first_layer_bed_temp > s.printer.max_bed_temp) {
    res.warnings.push_back(std::format("Температура стола для {} выше предела принтера — снижена до {:.0f} °C.",
                                       s.filament.type, s.printer.max_bed_temp));
    s.filament.bed_temp = std::min(s.filament.bed_temp, s.printer.max_bed_temp);
    s.filament.first_layer_bed_temp = std::min(s.filament.first_layer_bed_temp, s.printer.max_bed_temp);
  }
  const auto box = mesh.bbox();
  if (box.min[0] < -1e-6 || box.min[1] < -1e-6 || box.max[0] > s.printer.bed_x + 1e-6 || box.max[1] > s.printer.bed_y + 1e-6 ||
      box.max[2] > s.printer.max_z + 1e-6)
    res.warnings.push_back(std::format("Модель {:.0f} × {:.0f} × {:.0f} мм не помещается в область печати {} "
                                       "({:g} × {:g} × {:g} мм).",
                                       box.size()[0], box.size()[1], box.size()[2], s.printer.name, s.printer.bed_x,
                                       s.printer.bed_y, s.printer.max_z));
  const double s_speed_cap = s.filament.max_speed;
  auto speed = [&](double v) { return std::min(v, s_speed_cap); };

  // высоты слоёв: верх первого — first_layer_height, дальше через layer_height, не выше модели
  const double zmax = box.max[2];
  std::vector<double> tops{s.first_layer_height}, hs{s.first_layer_height};
  if (zmax < s.first_layer_height * 0.5) throw std::invalid_argument("Модель тоньше первого слоя — нечего печатать.");
  while (tops.back() + s.layer_height <= zmax + 1e-6) {
    tops.push_back(tops.back() + s.layer_height);
    hs.push_back(s.layer_height);
  }
  const std::size_t n = tops.size();
  std::vector<double> mids(n);
  for (std::size_t i = 0; i < n; ++i) mids[i] = tops[i] - 0.5 * hs[i];
  if (progress) progress(0.0, "Сечения слоёв");
  std::size_t open = 0;
  const std::vector<Paths64> regions = slice_layers(mesh, mids, open, progress);
  if (open > 0)
    res.warnings.push_back(std::format("Сетка не замкнута: в {} местах контур слоя не сошёлся — там будут пустоты.", open));

  const double w = s.line_width;
  {
    // Нависания: часть слоя, под которой нет опоры даже с запасом в полширины линии и наклоном
    // 45°. Узкие полоски (пологие стенки, верх горизонтальных отверстий) не в счёт. Поддержки
    // свой слайсер не строит — только предупреждает.
    const double allow = 0.5 * w + s.layer_height;
    double worst = 0, worst_z = 0;
    for (std::size_t i = 1; i < n; ++i) {
      if (regions[i].empty()) continue;
      Paths64 free = Clipper2Lib::Difference(regions[i], offset(regions[i - 1], allow), FillRule::NonZero);
      if (free.empty()) continue;
      free = offset(offset(free, -0.3), 0.3);
      const double a = area_mm2(free);
      if (a > worst) {
        worst = a;
        worst_z = mids[i];
      }
    }
    // опора на стол: деталь на ребре или вершине держится плохо
    double amax = 0;
    for (const auto& r : regions) amax = std::max(amax, area_mm2(r));
    const double a0 = area_mm2(regions[0]);
    if (a0 < 0.1 * amax && a0 < 100)
      res.warnings.push_back(
          a0 < 1 ? std::format("Деталь стоит на столе вершиной или ребром (самый широкий слой — {:.0f} мм²) — оторвётся "
                               "при печати. Поставьте её на стол плоской гранью.",
                               amax)
                 : std::format("Деталь касается стола всего {:.0f} мм² (самый широкий слой — {:.0f} мм²) — может "
                               "оторваться при печати. Поставьте её на стол плоской гранью.",
                               a0, amax));
    if (worst > 20) {
      std::string zs = std::format("{:.1f}", worst_z);
      std::replace(zs.begin(), zs.end(), '.', ',');
      res.warnings.push_back(std::format(
          "Нависание без опоры: на высоте {} мм в воздухе {:.0f} мм² слоя. Поддержки свой слайсер не строит — "
          "поверните деталь (обычно большой плоской гранью на стол) или нарежьте её в слайсере с поддержками.",
          zs, worst));
    }
  }
  std::vector<Layer> layers(n);
  Pt pos{0, 0};
  for (std::size_t i = 0; i < n; ++i) {
    Layer& L = layers[i];
    L.z = tops[i];
    L.h = hs[i];
    const double h = hs[i];
    const double sp = w - h * (1.0 - std::numbers::pi / 4.0);  // шаг между соседними валиками
    if (regions[i].empty()) continue;
    // юбка вокруг первого слоя
    if (i == 0 && s.skirt) {
      Paths64 sk = offset(regions[0], s.skirt_distance + w / 2);
      sk = Clipper2Lib::Union(sk, FillRule::NonZero);
      for (const auto& p : sk)
        if (Clipper2Lib::IsPositive(p)) {
          auto pts = to_pts(p);
          rotate_to_nearest(pts, pos);
          pos = pts.front();
          L.lines.push_back({Role::Skirt, std::move(pts), true});
        }
    }
    // что сверху и снизу: где в пределах top/bottom слоёв нет материала — сплошное заполнение
    Paths64 cover;
    bool first_cover = true;
    auto intersect_with = [&](std::ptrdiff_t k) {
      const Paths64 empty;
      const Paths64& r = (k >= 0 && static_cast<std::size_t>(k) < n) ? regions[static_cast<std::size_t>(k)] : empty;
      if (first_cover) {
        cover = r;
        first_cover = false;
      } else if (!cover.empty()) {
        cover = Clipper2Lib::Intersect(cover, r, FillRule::NonZero);
      }
    };
    for (int j = 1; j <= s.top_layers; ++j) intersect_with(static_cast<std::ptrdiff_t>(i) + j);
    for (int j = 1; j <= s.bottom_layers; ++j) intersect_with(static_cast<std::ptrdiff_t>(i) - j);
    if (first_cover) cover = regions[i];
    const Paths64 empty;
    const Paths64& above = i + 1 < n ? regions[i + 1] : empty;
    const Paths64& below = i > 0 ? regions[i - 1] : empty;

    // острова по очереди: ближайший к соплу
    auto isl = islands(regions[i]);
    std::vector<char> done(isl.size(), 0);
    for (std::size_t m = 0; m < isl.size(); ++m) {
      std::size_t best = 0;
      double bd = 1e300;
      for (std::size_t k = 0; k < isl.size(); ++k) {
        if (done[k]) continue;
        for (const auto& p : isl[k][0]) {
          const double d = dist2({mm(p.x), mm(p.y)}, pos);
          if (d < bd) bd = d, best = k;
        }
      }
      done[best] = 1;
      const Paths64& island = isl[best];
      // стенки: от внутренней к внешней
      std::vector<Paths64> walls;
      for (int k = 0; k < s.wall_loops; ++k) {
        Paths64 loop = offset(island, -(w / 2 + k * sp));
        if (loop.empty()) break;
        walls.push_back(std::move(loop));
      }
      for (std::size_t k = walls.size(); k-- > 0;)
        for (const auto& p : walls[k]) {
          if (p.size() < 3 || std::abs(Clipper2Lib::Area(p)) < 0.01 * kScale * kScale) continue;
          auto pts = to_pts(p);
          rotate_to_nearest(pts, pos);
          pos = pts.front();
          L.lines.push_back({k == 0 ? Role::OuterWall : Role::InnerWall, std::move(pts), true});
        }
      // заполнение
      // сплошное — вплотную к стенке, разреженное заходит на неё (лучше держится), как в Orca
      const double inset = w / 2 + (s.wall_loops - 1) * sp + sp / 2;
      Paths64 inner = offset(island, -inset);
      if (inner.empty()) continue;
      Paths64 solid = Clipper2Lib::Difference(inner, cover, FillRule::NonZero);
      const bool full = s.infill_density >= 0.999;
      Paths64 sparse = Clipper2Lib::Intersect(
          (!full && s.infill_overlap > 0) ? offset(island, -(inset - s.infill_overlap * w)) : inner, cover, FillRule::NonZero);
      move_small_to_solid(sparse, solid, 15.0);
      if (full) {
        solid = Clipper2Lib::Union(solid, sparse, FillRule::NonZero);
        sparse.clear();
      }
      if (!solid.empty()) {
        // верх и низ — отдельными подписями (для разбора G-code это одно и то же — сплошной материал)
        Paths64 top = Clipper2Lib::Difference(solid, above, FillRule::NonZero);
        Paths64 rest = Clipper2Lib::Intersect(solid, above, FillRule::NonZero);
        Paths64 bottom = Clipper2Lib::Difference(rest, below, FillRule::NonZero);
        Paths64 mid = Clipper2Lib::Intersect(rest, below, FillRule::NonZero);
        const double ang = (i % 2 == 0 || !s.alternate_solid) ? s.infill_angle : s.infill_angle - 90.0;
        for (auto [area, role] : {std::pair{&bottom, Role::Bottom}, std::pair{&mid, Role::Solid}, std::pair{&top, Role::Top}})
          add_open_lines(L.lines, hatch(*area, sp, ang, 0.0, w), role, pos);
      }
      if (!sparse.empty() && s.infill_density > 0) {
        std::vector<double> angles;
        double spacing = w / s.infill_density;
        switch (s.pattern) {
          case Pattern::Grid:
            angles = {s.infill_angle, s.infill_angle - 90.0};
            spacing *= 2;
            break;
          case Pattern::Triangles:
            angles = {s.infill_angle - 45.0, s.infill_angle + 15.0, s.infill_angle + 75.0};
            spacing *= 3;
            break;
          case Pattern::Rectilinear:
            angles = {(i % 2 == 0) ? s.infill_angle : s.infill_angle - 90.0};
            break;
          case Pattern::Lines:
            angles = {s.infill_angle};
            break;
        }
        std::vector<std::vector<Pt>> all;
        for (double a : angles) {
          auto lines = hatch(sparse, spacing, a, 0.0, w);
          all.insert(all.end(), std::make_move_iterator(lines.begin()), std::make_move_iterator(lines.end()));
        }
        add_open_lines(L.lines, std::move(all), Role::Sparse, pos);
      }
    }
    if (progress && i % 8 == 0) progress(0.3 + 0.6 * static_cast<double>(i) / static_cast<double>(n), "Стенки и заполнение");
  }

  // ---------------------------------------------------------------- G-code
  if (progress) progress(0.92, "G-code");
  Writer g(s);
  const std::string title = model_name.empty() ? std::string("модель") : std::string(model_name);
  g.line(std::format("; generated by Kika {} (свой слайсер)", kVersion));
  g.line("; HEADER_BLOCK_START");
  g.line("; model: " + title);
  g.line(std::format("; total layer number: {}", n));
  g.line(std::format("; printer: {}", s.printer.name));
  g.line("; HEADER_BLOCK_END");
  g.line("");
  g.line("; MACHINE_START_GCODE_START");
  g.line(substitute(s.printer.start_gcode, s));
  g.line("; MACHINE_START_GCODE_END");
  // экструзия ниже — в относительных единицах, что бы ни включил стартовый код
  g.line("G90");
  g.line("M83");
  g.line("G92 E0");
  for (std::size_t i = 0; i < n; ++i) {
    const Layer& L = layers[i];
    g.line(";LAYER_CHANGE");
    g.line(std::format(";Z:{:.3f}", L.z));
    g.line(std::format(";HEIGHT:{:.3f}", L.h));
    g.line(std::format("G1 Z{:.3f} F600", L.z));
    g.add_time(L.h / 10.0);
    if (i == 1) {
      if (s.filament.nozzle_temp != s.filament.first_layer_nozzle_temp) g.line(std::format("M104 S{:.0f}", s.filament.nozzle_temp));
      if (s.filament.bed_temp != s.filament.first_layer_bed_temp) g.line(std::format("M140 S{:.0f}", s.filament.bed_temp));
      if (s.filament.fan > 0) g.line(std::format("M106 S{:.0f}", std::round(s.filament.fan * 255)));
    }
    std::optional<Role> cur;
    for (const auto& ln : L.lines) {
      if (cur != ln.role) {
        g.line(std::string(";TYPE:") + role_label(ln.role));
        cur = ln.role;
      }
      double v = 0;
      switch (ln.role) {
        case Role::OuterWall: v = s.speed_outer_wall; break;
        case Role::InnerWall: v = s.speed_inner_wall; break;
        case Role::Solid:
        case Role::Bottom: v = s.speed_solid; break;
        case Role::Top: v = s.speed_top; break;
        case Role::Sparse: v = s.speed_sparse; break;
        case Role::Skirt: v = s.speed_first_layer; break;
      }
      if (i == 0) v = std::min(v, s.speed_first_layer);
      g.extrude(ln.pts, ln.closed, w, L.h, speed(v));
    }
  }
  g.line("; MACHINE_END_GCODE_START");
  g.line(substitute(s.printer.end_gcode, s));
  g.line("");
  // настройки — как у Orca, их читает разбор G-code
  g.line("; CONFIG_BLOCK_START");
  g.line(std::format("; printer_model = {}", s.printer.name));
  g.line(std::format("; layer_height = {:g}", s.layer_height));
  g.line(std::format("; initial_layer_print_height = {:g}", s.first_layer_height));
  g.line(std::format("; nozzle_diameter = {:g}", s.printer.nozzle));
  g.line(std::format("; filament_diameter = {:g}", s.printer.filament_diameter));
  g.line(std::format("; filament_type = {}", s.filament.type));
  g.line(std::format("; filament_density = {:g}", s.filament.density));
  g.line(std::format("; line_width = {:g}", s.line_width));
  g.line(std::format("; wall_loops = {}", s.wall_loops));
  g.line(std::format("; top_shell_layers = {}", s.top_layers));
  g.line(std::format("; bottom_shell_layers = {}", s.bottom_layers));
  g.line(std::format("; sparse_infill_density = {:g}%", s.infill_density * 100));
  g.line(std::format("; sparse_infill_pattern = {}", pattern_key(s.pattern)));
  g.line(std::format("; nozzle_temperature = {:g}", s.filament.nozzle_temp));
  g.line(std::format("; hot_plate_temp = {:g}", s.filament.bed_temp));
  g.line("; CONFIG_BLOCK_END");

  res.layers = static_cast<int>(n);
  res.filament_mm = g.filament();
  res.filament_g = g.filament() * std::numbers::pi * 0.25 * s.printer.filament_diameter * s.printer.filament_diameter * 1e-3 *
                   s.filament.density;
  res.print_time_s = g.time();
  res.gcode = std::move(g.text());
  if (progress) progress(1.0, "Готово");
  return res;
}

}  // namespace kika::slicer
