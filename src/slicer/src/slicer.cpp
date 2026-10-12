// Свой слайсер: сечения → стенки → заполнение → G-code.
//
// Сечение слоя — в середине его высоты. Отрезки сечения склеиваются в контуры по рёбрам сетки
// (у соседних треугольников общее ребро — общая точка), направление — по нормали треугольника,
// поэтому внешние контуры идут против часовой стрелки, отверстия — по часовой, и правило
// «ненулевой обмотки» правильно объединяет пересекающиеся тела.
// Стенки — смещения контура внутрь, шаг между валиками как в Slic3r/Orca: w − h·(1 − π/4).
// Сплошные слои — где сверху или снизу в пределах top/bottom слоёв нет материала.

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <format>
#include <numbers>
#include <numeric>
#include <unordered_map>

#include "common.hpp"
#include "infill.hpp"
#include "motion.hpp"
#include "support.hpp"
#include "thumbnail.hpp"
#include "kika/slicer/slicer.hpp"
#include "kika/version.hpp"

namespace kika::slicer {

namespace {

using namespace detail;
using geometry::TriangleMesh;
using geometry::Vec3;

// Роль линии и её название в G-code (как у OrcaSlicer — их понимает разбор G-code).
enum class Role { OuterWall, InnerWall, Solid, Top, Bottom, Sparse, Skirt, Brim, Support, SupportInterface };

const char* role_label(Role r) {
  switch (r) {
    case Role::OuterWall: return "Outer wall";
    case Role::InnerWall: return "Inner wall";
    case Role::Solid: return "Internal solid infill";
    case Role::Top: return "Top surface";
    case Role::Bottom: return "Bottom surface";
    case Role::Sparse: return "Sparse infill";
    case Role::Skirt: return "Skirt";
    case Role::Brim: return "Brim";
    case Role::Support: return "Support";
    case Role::SupportInterface: return "Support interface";
  }
  return "Custom";
}

struct Line {
  Role role;
  std::vector<Pt> pts;
  bool closed = false;
  std::vector<char> over;  // по отрезкам: над пустотой (нависание, мост); пусто — нигде
  int zone = -1;           // остров слоя: перемещение внутри него — без отката
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

// Замкнутый контур начинаем с вершины, ближайшей к точке from.
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
    out.push_back({role, std::move(l), false, {}});
  }
}

// Вершина для шва. Aligned: ближе к шву слоя ниже (ref), с предпочтением углов — там шов
// незаметнее; Rear: самая задняя; Random: случайная; Nearest: ближе к соплу.
std::size_t seam_vertex(const std::vector<Pt>& loop, Seam seam, const Pt& nozzle, const Pt& ref, std::uint64_t& rng) {
  const std::size_t n = loop.size();
  if (n == 0) return 0;
  std::size_t best = 0;
  double bs = 1e300;
  switch (seam) {
    case Seam::Nearest:
      for (std::size_t i = 0; i < n; ++i)
        if (dist2(loop[i], nozzle) < bs) bs = dist2(loop[i], nozzle), best = i;
      return best;
    case Seam::Random:
      rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
      return static_cast<std::size_t>((rng >> 33) % n);
    case Seam::Rear:
      for (std::size_t i = 0; i < n; ++i) {
        const double sc = -loop[i][1] + 0.01 * std::abs(loop[i][0] - ref[0]);
        if (sc < bs) bs = sc, best = i;
      }
      return best;
    case Seam::Aligned:
      for (std::size_t i = 0; i < n; ++i) {
        const Pt& a = loop[(i + n - 1) % n];
        const Pt& b = loop[i];
        const Pt& c = loop[(i + 1) % n];
        const double ux = b[0] - a[0], uy = b[1] - a[1], vx = c[0] - b[0], vy = c[1] - b[1];
        const double lu = std::hypot(ux, uy), lv = std::hypot(vx, vy);
        double corner = 0;
        if (lu > 0.2 && lv > 0.2) corner = 1.0 - (ux * vx + uy * vy) / (lu * lv);  // 0 — прямо, 1 — прямой угол
        const double sc = std::sqrt(dist2(b, ref)) - 1.5 * std::min(corner, 1.0);
        if (sc < bs) bs = sc, best = i;
      }
      return best;
  }
  return 0;
}

// Область с габаритами путей — для быстрой проверки «точка внутри».
struct Area {
  Paths64 paths;
  std::vector<Clipper2Lib::Rect64> boxes;
  Clipper2Lib::Rect64 box;
  explicit Area(Paths64 p) : paths(std::move(p)) {
    box = Clipper2Lib::GetBounds(paths);
    for (const auto& path : paths) boxes.push_back(Clipper2Lib::GetBounds(path));
  }
  bool empty() const { return paths.empty(); }
  bool contains(const Pt& p) const {
    const Point64 pt(q(p[0]), q(p[1]));
    if (paths.empty() || pt.x < box.left || pt.x > box.right || pt.y < box.top || pt.y > box.bottom) return false;
    int n = 0;
    for (std::size_t i = 0; i < paths.size(); ++i) {
      const auto& b = boxes[i];
      if (pt.x < b.left || pt.x > b.right || pt.y < b.top || pt.y > b.bottom) continue;
      if (Clipper2Lib::PointInPolygon(pt, paths[i]) != Clipper2Lib::PointInPolygonResult::IsOutside) ++n;
    }
    return n % 2 == 1;
  }
};

// Отрезки линии над пустотой: середина отрезка — в зоне нависания.
void mark_over(Line& ln, const Area& zone) {
  if (zone.empty() || ln.pts.size() < 2) return;
  const std::size_t segs = ln.closed ? ln.pts.size() : ln.pts.size() - 1;
  std::vector<char> over(segs, 0);
  bool any = false;
  for (std::size_t i = 0; i < segs; ++i) {
    const Pt& a = ln.pts[i];
    const Pt& b = ln.pts[(i + 1) % ln.pts.size()];
    over[i] = zone.contains({0.5 * (a[0] + b[0]), 0.5 * (a[1] + b[1])}) ? 1 : 0;
    any = any || over[i];
  }
  if (any) ln.over = std::move(over);
}

// Выпуклая оболочка (монотонная цепь), против часовой стрелки.
std::vector<Pt> convex_hull(std::vector<Pt> p) {
  std::sort(p.begin(), p.end());
  p.erase(std::unique(p.begin(), p.end()), p.end());
  if (p.size() < 3) return p;
  auto cross = [](const Pt& o, const Pt& a, const Pt& b) {
    return (a[0] - o[0]) * (b[1] - o[1]) - (a[1] - o[1]) * (b[0] - o[0]);
  };
  std::vector<Pt> h(2 * p.size());
  std::size_t k = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    while (k >= 2 && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
    h[k++] = p[i];
  }
  for (std::size_t i = p.size() - 1, t = k + 1; i-- > 0;) {
    while (k >= t && cross(h[k - 2], h[k - 1], p[i]) <= 0) --k;
    h[k++] = p[i];
  }
  h.resize(k - 1);
  return h;
}

// Имя детали для EXCLUDE_OBJECT: латиница, цифры, «_» и «-».
std::string object_name(std::string_view name) {
  std::string out;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '_' || c == '-')
      out.push_back(c);
    else if (!out.empty() && out.back() != '_')
      out.push_back('_');
  }
  while (!out.empty() && out.back() == '_') out.pop_back();
  bool letters = false;
  for (char c : out) letters = letters || std::isalpha(static_cast<unsigned char>(c));
  return letters ? out : "part";
}

// ---------------------------------------------------------------------------- G-code

// Скорости, ускорения и обдув после прижатия к пределам принтера и пластика.
struct Motion {
  double travel = 0, accel_travel = 0, retract_length = 0, retract_speed = 0, z_hop = 0, min_travel = 0, speed_z = 0;
};

class Writer {
 public:
  Writer(const Settings& s, const Motion& m)
      : s_(s), m_(m), est_(s.printer.corner_speed, s.printer.max_accel) {
    fil_area_ = std::numbers::pi * 0.25 * s.printer.filament_diameter * s.printer.filament_diameter;
  }

  std::string& text() { return out_; }
  void line(std::string_view l) {
    out_.append(l);
    out_.push_back('\n');
  }
  // Положение после стартового кода неизвестно: первое перемещение — от (0, 0).
  void set_z(double z, double speed_z) {
    retract();
    out_ += std::format("G1 Z{:.3f} F{:.0f}\n", z, speed_z * 60);
    est_.stop(std::abs(z - z_) / std::max(speed_z, 1.0));
    z_ = z;
  }

  void set_accel(double a) {
    a = std::round(a);
    if (a == accel_) return;
    accel_ = a;
    out_ += std::format("M204 S{:.0f}\n", a);
  }
  void set_fan(int v) {
    v = std::clamp(v, 0, 255);
    if (v == fan_) return;
    fan_ = v;
    out_ += std::format("M106 S{}\n", v);
  }
  int fan() const { return fan_; }

  void retract() {
    if (retracted_ || m_.retract_length <= 0) return;
    out_ += std::format("G1 E{:.5f} F{:.0f}\n", -m_.retract_length, m_.retract_speed * 60);
    retracted_ = true;
    est_.stop(m_.retract_length / m_.retract_speed);
  }

  // quiet — перемещение внутри острова детали: подтёк останется внутри, откат не нужен.
  void travel(const Pt& p, bool quiet = false) {
    const double d = std::hypot(p[0] - x_, p[1] - y_);
    if (d < 1e-4) return;
    if (d > m_.min_travel && !quiet) {
      retract();
      if (m_.z_hop > 0 && retracted_ && !hopped_) {
        out_ += std::format("G1 Z{:.3f} F{:.0f}\n", z_ + m_.z_hop, m_.speed_z * 60);
        est_.stop(m_.z_hop / m_.speed_z);
        hopped_ = true;
      }
    }
    set_accel(m_.accel_travel);
    out_ += std::format("G0 X{:.3f} Y{:.3f} F{:.0f}\n", p[0], p[1], m_.travel * 60);
    est_.move(p[0] - x_, p[1] - y_, 0, m_.travel, accel_);
    x_ = p[0];
    y_ = p[1];
  }

  // Линия: скорость v, над пустотой — over_v и обдув over_fan (если больше текущего).
  void extrude(const Line& ln, double width, double h, double v, double over_v, int over_fan, double accel, bool support,
               bool quiet = false) {
    const auto& pts = ln.pts;
    if (pts.size() < 2) return;
    travel(pts[0], quiet);
    if (hopped_) {
      out_ += std::format("G1 Z{:.3f} F{:.0f}\n", z_, m_.speed_z * 60);
      est_.stop(m_.z_hop / m_.speed_z);
      hopped_ = false;
    }
    if (retracted_) {
      out_ += std::format("G1 E{:.5f} F{:.0f}\n", m_.retract_length, m_.retract_speed * 60);
      retracted_ = false;
      est_.stop(m_.retract_length / m_.retract_speed);
    }
    set_accel(accel);
    // площадь сечения валика: прямоугольник со скруглёнными боками (как в Slic3r/Orca)
    const double area = (width - h) * h + std::numbers::pi * 0.25 * h * h;
    const double e_per_mm = area / fil_area_ * s_.filament.flow;
    const int layer_fan = fan_;
    double cur_f = -1;
    const std::size_t segs = ln.closed ? pts.size() : pts.size() - 1;
    for (std::size_t i = 0; i < segs; ++i) {
      const Pt& p = pts[(i + 1) % pts.size()];
      const double l = std::hypot(p[0] - x_, p[1] - y_);
      if (l < 1e-4) continue;
      const bool over = !ln.over.empty() && ln.over[i];
      set_fan(over ? std::max(layer_fan, over_fan) : layer_fan);
      const double sp = over ? std::min(v, over_v) : v;
      const double e = l * e_per_mm;
      if (sp != cur_f)
        out_ += std::format("G1 X{:.3f} Y{:.3f} E{:.5f} F{:.0f}\n", p[0], p[1], e, sp * 60);
      else
        out_ += std::format("G1 X{:.3f} Y{:.3f} E{:.5f}\n", p[0], p[1], e);
      cur_f = sp;
      est_.move(p[0] - x_, p[1] - y_, 0, sp, accel_);
      x_ = p[0];
      y_ = p[1];
      e_total_ += e;
      if (support) e_support_ += e;
    }
    set_fan(layer_fan);
  }

  Pt pos() const { return {x_, y_}; }
  double filament() const { return e_total_; }
  double support_filament() const { return e_support_; }
  double time() {
    est_.flush();
    return est_.total();
  }

 private:
  const Settings& s_;
  Motion m_;
  TimeEstimator est_;
  std::string out_;
  double fil_area_ = 1;
  double x_ = 0, y_ = 0, z_ = 0;
  double accel_ = -1;
  int fan_ = 0;
  bool retracted_ = false, hopped_ = false;
  double e_total_ = 0, e_support_ = 0;
};

std::string num0(double v) { return std::format("{:.0f}", v); }

std::string substitute(std::string t, const std::vector<std::pair<std::string, std::string>>& vars) {
  for (const auto& [k, v] : vars)
    for (std::size_t p = t.find(k); p != std::string::npos; p = t.find(k, p + v.size())) t.replace(p, k.size(), v);
  return t;
}

// Время для итогов в стиле PrusaSlicer: «1h 2m 3s» (так его читает Moonraker).
std::string prusa_time(double sec) {
  long long t = std::llround(sec);
  const long long d = t / 86400, h = t / 3600 % 24, m = t / 60 % 60, s = t % 60;
  std::string out;
  if (d) out += std::format("{}d ", d);
  if (d || h) out += std::format("{}h ", h);
  if (d || h || m) out += std::format("{}m ", m);
  out += std::format("{}s", s);
  return out;
}

}  // namespace

// ============================================================================

std::string now_stamp() {
  const std::time_t t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d at %H:%M:%S", &tm);
  return buf;
}

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
  const double chamber = s.printer.max_chamber_temp > 0 ? std::min(s.filament.chamber_temp, s.printer.max_chamber_temp) : 0.0;
  if (s.filament.chamber_temp > 0 && s.printer.max_chamber_temp <= 0)
    res.warnings.push_back(std::format("Пластику {} нужна тёплая камера ({:.0f} °C), а у принтера {} её подогрева нет — "
                                       "возможны трещины между слоями и отрыв углов.",
                                       s.filament.name, s.filament.chamber_temp, s.printer.name));
  const auto box = mesh.bbox();
  if (box.min[0] < -1e-6 || box.min[1] < -1e-6 || box.max[0] > s.printer.bed_x + 1e-6 || box.max[1] > s.printer.bed_y + 1e-6 ||
      box.max[2] > s.printer.max_z + 1e-6)
    res.warnings.push_back(std::format("Модель {:.0f} × {:.0f} × {:.0f} мм не помещается в область печати {} "
                                       "({:g} × {:g} × {:g} мм).",
                                       box.size()[0], box.size()[1], box.size()[2], s.printer.name, s.printer.bed_x,
                                       s.printer.bed_y, s.printer.max_z));

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
    // 45°. Узкие полоски (пологие стенки, верх горизонтальных отверстий) не в счёт.
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
                               "оторваться при печати. Поставьте её на стол плоской гранью{}.",
                               a0, amax, s.brim_width > 0 ? "" : " или включите кайму"));
    if (worst > 20 && s.support == Support::None) {
      std::string zs = std::format("{:.1f}", worst_z);
      std::replace(zs.begin(), zs.end(), '.', ',');
      res.warnings.push_back(std::format(
          "Нависание без опоры: на высоте {} мм в воздухе {:.0f} мм² слоя. Включите поддержки или поверните "
          "деталь (обычно большой плоской гранью на стол).",
          zs, worst));
    }
  }
  SupportPlan supports;
  if (s.support != Support::None) {
    if (progress) progress(0.3, "Поддержки");
    supports = plan_supports(regions, mids, hs, s);
    if (supports.unsupported_mm2 > 20) {
      std::string zs = std::format("{:.1f}", supports.unsupported_z);
      std::replace(zs.begin(), zs.end(), '.', ',');
      res.warnings.push_back(std::format(
          "Часть нависания осталась без поддержки: на высоте {} мм — {:.0f} мм²{}.", zs, supports.unsupported_mm2,
          s.support_on_bed_only ? " (поддержки только от стола, а под этим местом — деталь)" : ""));
    }
  }

  // ------------------------------------------------------------- линии слоёв
  struct LayerOut {
    double z = 0, h = 0;
    std::vector<Line> pre;   // юбка (вне детали)
    std::vector<Line> part;  // кайма, поддержки и деталь
    std::vector<Area> zones; // острова детали (чуть меньше): перемещения внутри — без отката
  };
  std::vector<LayerOut> layers(n);
  Pt pos{s.printer.bed_x / 2, s.printer.bed_y / 2};
  std::uint64_t rng = 0x9E3779B97F4A7C15ULL;
  std::vector<Pt> prev_seams, seams;
  const Pt rear{0.5 * (box.min[0] + box.max[0]), box.max[1]};
  for (std::size_t i = 0; i < n; ++i) {
    LayerOut& L = layers[i];
    L.z = tops[i];
    L.h = hs[i];
    const double h = hs[i];
    const double sp = w - h * (1.0 - std::numbers::pi / 4.0);  // шаг между соседними валиками
    // юбка и кайма вокруг первого слоя (и его поддержек)
    if (i == 0 && !regions[0].empty()) {
      Paths64 first = regions[0];
      if (!supports.layers.empty())
        first = Clipper2Lib::Union(first, Clipper2Lib::Union(supports.layers[0].base, supports.layers[0].interface, FillRule::NonZero),
                                   FillRule::NonZero);
      first = Clipper2Lib::Union(first, FillRule::NonZero);
      Paths64 outer;  // только наружные контуры: кайма не лезет в отверстия
      for (const auto& p : first)
        if (Clipper2Lib::IsPositive(p)) outer.push_back(p);
      Paths64 footprint = outer;
      if (s.brim_width > 0) {
        const int loops = std::max(1, static_cast<int>(std::floor((s.brim_width - w / 2) / sp)) + 1);
        // снаружи внутрь: последний виток — у детали
        for (int k = loops; k-- > 0;) {
          const Paths64 ring = Clipper2Lib::Union(offset(outer, s.brim_gap + w / 2 + k * sp), FillRule::NonZero);
          for (const auto& p : ring)
            if (Clipper2Lib::IsPositive(p)) {
              auto pts = to_pts(p);
              rotate_to_nearest(pts, pos);
              pos = pts.front();
              L.part.push_back({Role::Brim, std::move(pts), true, {}});
            }
        }
        footprint = offset(outer, s.brim_gap + loops * sp);
      }
      if (s.skirt && s.skirt_loops > 0) {
        for (int k = s.skirt_loops; k-- > 0;) {
          const Paths64 sk = Clipper2Lib::Union(offset(footprint, s.skirt_distance + w / 2 + k * sp), FillRule::NonZero);
          for (const auto& p : sk)
            if (Clipper2Lib::IsPositive(p)) {
              auto pts = to_pts(p);
              rotate_to_nearest(pts, pos);
              pos = pts.front();
              L.pre.push_back({Role::Skirt, std::move(pts), true, {}});
            }
        }
      }
    }
    // поддержки слоя — до детали
    if (!supports.layers.empty()) {
      auto sl = support_lines(supports.layers[i], s, i, sp);
      for (auto [lines, role] : {std::pair{&sl.base, Role::Support}, std::pair{&sl.interface, Role::SupportInterface}}) {
        std::vector<Polyline> open_lines;
        for (auto& fp : *lines) {
          if (fp.closed) {
            rotate_to_nearest(fp.pts, pos);
            pos = fp.pts.front();
            L.part.push_back({role, std::move(fp.pts), true, {}});
          } else {
            open_lines.push_back(std::move(fp.pts));
          }
        }
        add_open_lines(L.part, std::move(open_lines), role, pos);
      }
    }
    if (regions[i].empty()) continue;
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
    // зона над пустотой: здесь линии печатаются медленнее и с сильным обдувом
    const Area over_zone(i > 0 ? Clipper2Lib::Difference(regions[i], offset(below, 0.1 * w), FillRule::NonZero) : Paths64{});

    // острова по очереди: ближайший к соплу
    auto isl = islands(regions[i]);
    std::vector<char> done(isl.size(), 0);
    seams.clear();
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
      const int zone = static_cast<int>(L.zones.size());
      L.zones.emplace_back(offset(island, -0.25 * w));
      const std::size_t first_line = L.part.size();
      std::vector<Paths64> walls;
      for (int k = 0; k < s.wall_loops; ++k) {
        Paths64 loop = offset(island, -(w / 2 + k * sp));
        if (loop.empty()) break;
        walls.push_back(std::move(loop));
      }
      // шов: на наружном контуре острова; остальные стенки начинаются рядом с ним
      Pt seam_pt = pos;
      if (!walls.empty() && s.seam != Seam::Nearest) {
        const Path64* main = nullptr;
        double amax = 0;
        for (const auto& p : walls[0]) {
          const double a = Clipper2Lib::Area(p);
          if (a > amax) amax = a, main = &p;
        }
        if (main) {
          const auto pts = to_pts(*main);
          Pt ref = rear;
          if (s.seam == Seam::Aligned) {
            const auto bb = Clipper2Lib::GetBounds(*main);
            const Pt c{mm((bb.left + bb.right) / 2), mm((bb.top + bb.bottom) / 2)};
            double rd = 1e300;
            for (const auto& ps : prev_seams)
              if (dist2(ps, c) < rd) rd = dist2(ps, c), ref = ps;
            if (prev_seams.empty()) ref = {mm((bb.left + bb.right) / 2), mm(bb.bottom)};
          }
          seam_pt = pts[seam_vertex(pts, s.seam, pos, ref, rng)];
          seams.push_back(seam_pt);
        }
      }
      auto add_walls = [&](std::size_t k) {
        for (const auto& p : walls[k]) {
          if (p.size() < 3 || std::abs(Clipper2Lib::Area(p)) < 0.01 * kScale * kScale) continue;
          auto pts = to_pts(p);
          rotate_to_nearest(pts, s.seam == Seam::Nearest ? pos : seam_pt);
          pos = pts.front();
          Line ln{k == 0 ? Role::OuterWall : Role::InnerWall, std::move(pts), true, {}};
          mark_over(ln, over_zone);
          L.part.push_back(std::move(ln));
        }
      };
      if (s.outer_wall_first)
        for (std::size_t k = 0; k < walls.size(); ++k) add_walls(k);
      else
        for (std::size_t k = walls.size(); k-- > 0;) add_walls(k);
      // заполнение
      // сплошное — вплотную к стенке, разреженное заходит на неё (лучше держится), как в Orca
      const double inset = w / 2 + (s.wall_loops - 1) * sp + sp / 2;
      Paths64 inner = offset(island, -inset);
      if (inner.empty()) {
        for (std::size_t k = first_line; k < L.part.size(); ++k) L.part[k].zone = zone;
        continue;
      }
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
        for (auto [area, role] : {std::pair{&bottom, Role::Bottom}, std::pair{&mid, Role::Solid}, std::pair{&top, Role::Top}}) {
          const std::size_t from = L.part.size();
          add_open_lines(L.part, hatch(*area, sp, ang, 0.0, w), role, pos);
          if (role != Role::Solid)
            for (std::size_t k = from; k < L.part.size(); ++k) mark_over(L.part[k], over_zone);
        }
      }
      if (!sparse.empty() && s.infill_density > 0) {
        InfillParams ip;
        ip.pattern = s.pattern;
        ip.density = s.infill_density;
        ip.spacing = sp;
        ip.line_width = w;
        ip.angle = s.infill_angle;
        ip.layer = i;
        ip.z = mids[i];
        std::vector<Polyline> open_lines;
        for (auto& fp : sparse_infill(sparse, ip)) {
          if (fp.closed) {
            rotate_to_nearest(fp.pts, pos);
            pos = fp.pts.front();
            L.part.push_back({Role::Sparse, std::move(fp.pts), true, {}});
          } else {
            open_lines.push_back(std::move(fp.pts));
          }
        }
        add_open_lines(L.part, std::move(open_lines), Role::Sparse, pos);
      }
      for (std::size_t k = first_line; k < L.part.size(); ++k) L.part[k].zone = zone;
    }
    if (!seams.empty()) prev_seams = seams;
    if (progress && i % 8 == 0) progress(0.3 + 0.6 * static_cast<double>(i) / static_cast<double>(n), "Стенки и заполнение");
  }

  // ------------------------------------------------------------- скорости и обдув
  const Printer& pr = s.printer;
  const Filament& fl = s.filament;
  Motion mo;
  mo.travel = std::min(s.speed_travel, pr.max_speed);
  auto clamp_accel = [&](double a) { return std::clamp(a > 0 ? a : s.accel_default, 100.0, pr.max_accel); };
  mo.accel_travel = clamp_accel(s.accel_travel);
  mo.retract_length = fl.retract_length >= 0 ? fl.retract_length : pr.retract_length;
  mo.retract_speed = std::max(1.0, fl.retract_speed > 0 ? fl.retract_speed : pr.retract_speed);
  mo.z_hop = pr.z_hop;
  mo.min_travel = pr.retract_min_travel;
  mo.speed_z = std::max(1.0, pr.max_speed_z);
  // скорость роли с пределами принтера, пластика и расхода сопла
  auto role_speed = [&](Role r, std::size_t layer, double h) {
    double v = 0;
    switch (r) {
      case Role::OuterWall: v = s.speed_outer_wall; break;
      case Role::InnerWall: v = s.speed_inner_wall; break;
      case Role::Solid:
      case Role::Bottom: v = s.speed_solid; break;
      case Role::Top: v = s.speed_top; break;
      case Role::Sparse: v = s.speed_sparse; break;
      case Role::Skirt:
      case Role::Brim: v = s.speed_first_layer; break;
      case Role::Support: v = s.speed_support; break;
      case Role::SupportInterface: v = s.speed_support_interface; break;
    }
    if (layer == 0) v = std::min(v, s.speed_first_layer);
    v = std::min({v, pr.max_speed, fl.max_speed});
    const double area = (w - h) * h + std::numbers::pi * 0.25 * h * h;
    if (fl.max_volumetric > 0) v = std::min(v, fl.max_volumetric / std::max(area, 1e-6));
    return std::max(v, 1.0);
  };
  auto role_accel = [&](Role r, std::size_t layer) {
    if (layer == 0) return clamp_accel(s.accel_first_layer);
    switch (r) {
      case Role::OuterWall: return clamp_accel(s.accel_outer_wall);
      case Role::InnerWall: return clamp_accel(s.accel_inner_wall);
      case Role::Top: return clamp_accel(s.accel_top);
      case Role::Sparse: return clamp_accel(s.accel_sparse);
      case Role::Skirt:
      case Role::Brim: return clamp_accel(s.accel_first_layer);
      default: return clamp_accel(s.accel_default);
    }
  };
  auto line_len = [](const Line& ln) {
    double l = 0;
    for (std::size_t k = 1; k < ln.pts.size(); ++k) l += std::sqrt(dist2(ln.pts[k - 1], ln.pts[k]));
    if (ln.closed && ln.pts.size() > 2) l += std::sqrt(dist2(ln.pts.back(), ln.pts.front()));
    return l;
  };

  // ------------------------------------------------------------- G-code
  if (progress) progress(0.92, "G-code");
  Writer g(s, mo);
  const std::string title = model_name.empty() ? std::string("модель") : std::string(model_name);
  const bool klipper = pr.flavor == Flavor::Klipper;
  const bool label = klipper && s.label_objects;
  const std::string obj = object_name(model_name.empty() ? std::string_view("part") : model_name);
  double max_layer_z = layers.empty() ? 0.0 : layers.back().z;
  if (!s.generated_at.empty())
    g.line(std::format("; generated by Kika {} on {}", kVersion, s.generated_at));
  else
    g.line(std::format("; generated by Kika {} (свой слайсер)", kVersion));
  g.line("; HEADER_BLOCK_START");
  g.line("; model: " + title);
  g.line(std::format("; total layer number: {}", n));
  g.line(std::format("; printer: {}", pr.name));
  g.line("; HEADER_BLOCK_END");
  if (pr.thumbnails)
    for (const auto& [tw, th] : {std::pair{300, 300}, std::pair{50, 50}}) g.text() += thumbnail_block(mesh, tw, th);
  g.line("");
  const std::vector<std::pair<std::string, std::string>> vars = {
      {"{nozzle_temp}", num0(fl.nozzle_temp)},
      {"{first_layer_nozzle_temp}", num0(fl.first_layer_nozzle_temp)},
      {"{bed_temp}", num0(fl.bed_temp)},
      {"{first_layer_bed_temp}", num0(fl.first_layer_bed_temp)},
      {"{chamber_temp}", num0(chamber)},
      {"{bed_x}", std::format("{:g}", pr.bed_x)},
      {"{bed_y}", std::format("{:g}", pr.bed_y)},
      {"{max_z}", std::format("{:g}", pr.max_z)},
      {"{filament_type}", fl.type},
      {"{total_layers}", std::format("{}", n)},
      {"{max_layer_z}", std::format("{:.3f}", max_layer_z)},
      {"{lift_z}", std::format("{:.3f}", std::min(pr.max_z, max_layer_z + 3))},
      {"{park_z}", std::format("{:.3f}", std::min(pr.max_z, std::max(max_layer_z + 3, 0.5 * pr.max_z)))},
  };
  // разметка детали: многоугольник — выпуклая оболочка следа на столе (с каймой)
  if (label) {
    std::vector<Pt> pts;
    pts.reserve(mesh.vertices.size());
    for (const auto& v : mesh.vertices) pts.push_back({std::round(v[0] * 10) / 10, std::round(v[1] * 10) / 10});
    for (const auto& ln : layers.front().part)
      if (ln.role == Role::Brim)
        for (const auto& p : ln.pts) pts.push_back({std::round(p[0] * 10) / 10, std::round(p[1] * 10) / 10});
    const auto hull = convex_hull(std::move(pts));
    std::string poly;
    double cx = 0, cy = 0;
    for (const auto& p : hull) {
      poly += std::format("{}[{:.3f},{:.3f}]", poly.empty() ? "" : ",", p[0], p[1]);
      cx += p[0];
      cy += p[1];
    }
    if (!hull.empty()) {
      cx /= static_cast<double>(hull.size());
      cy /= static_cast<double>(hull.size());
    }
    g.line(std::format("EXCLUDE_OBJECT_DEFINE NAME={} CENTER={:.3f},{:.3f} POLYGON=[{}]", obj, cx, cy, poly));
  }
  g.line("; MACHINE_START_GCODE_START");
  g.line(substitute(pr.start_gcode, vars));
  g.line("; MACHINE_START_GCODE_END");
  // экструзия ниже — в относительных единицах, что бы ни включил стартовый код
  g.line("G90");
  g.line("M83");
  g.line("G92 E0");
  if (klipper && fl.pressure_advance >= 0) g.line(std::format("SET_PRESSURE_ADVANCE ADVANCE={:.4f}", fl.pressure_advance));
  g.set_fan(0);
  for (std::size_t i = 0; i < n; ++i) {
    const LayerOut& L = layers[i];
    // время слоя без замедления: для обдува и замедления коротких слоёв
    double t_layer = 0;
    Pt at{-1e9, -1e9};
    for (const auto* list : {&L.pre, &L.part})
      for (const auto& ln : *list) {
        if (ln.pts.empty()) continue;
        if (at[0] > -1e8) t_layer += std::sqrt(dist2(at, ln.pts.front())) / mo.travel;
        t_layer += line_len(ln) / role_speed(ln.role, i, L.h);
        at = ln.closed ? ln.pts.front() : ln.pts.back();
      }
    double slow = 1.0;
    if (fl.slowdown_time > 0 && t_layer > 0 && t_layer < fl.slowdown_time) slow = t_layer / fl.slowdown_time;
    // обдув: первые слои — без; короткий слой — сильнее, вплоть до наибольшего
    double fan = 0;
    if (static_cast<int>(i) >= fl.fan_off_layers) {
      if (t_layer < fl.slowdown_time || fl.fan_full_time <= fl.slowdown_time) {
        fan = t_layer < fl.fan_full_time ? fl.fan_max : fl.fan_min;
      } else if (t_layer < fl.fan_full_time) {
        const double k = (t_layer - fl.slowdown_time) / (fl.fan_full_time - fl.slowdown_time);
        fan = fl.fan_max - (fl.fan_max - fl.fan_min) * k;
      } else {
        fan = fl.fan_min;
      }
    }
    const int over_fan = static_cast<int>(i) >= fl.fan_off_layers ? static_cast<int>(std::lround(fl.overhang_fan * 255)) : 0;
    // как у PrusaSlicer: по этой паре строк Moonraker узнаёт высоту детали
    g.line(";BEFORE_LAYER_CHANGE");
    g.line(std::format(";{:.3f}", L.z));
    g.line(";LAYER_CHANGE");
    g.line(std::format(";Z:{:.3f}", L.z));
    g.line(std::format(";HEIGHT:{:.3f}", L.h));
    g.set_z(L.z, mo.speed_z);
    if (!pr.layer_gcode.empty()) {
      auto lv = vars;
      lv.push_back({"{layer_num}", std::format("{}", i + 1)});
      lv.push_back({"{layer_z}", std::format("{:.3f}", L.z)});
      g.line(substitute(pr.layer_gcode, lv));
    }
    if (i == 1) {
      if (fl.nozzle_temp != fl.first_layer_nozzle_temp) g.line(std::format("M104 S{:.0f}", fl.nozzle_temp));
      if (fl.bed_temp != fl.first_layer_bed_temp) g.line(std::format("M140 S{:.0f}", fl.bed_temp));
    }
    if (pr.aux_fan && static_cast<int>(i) == fl.fan_off_layers && fl.aux_fan > 0)
      g.line(std::format("M106 P2 S{:.0f}", std::round(fl.aux_fan * 255)));
    g.set_fan(static_cast<int>(std::lround(fan * 255)));
    std::optional<Role> cur;
    int last_zone = -1;
    // перемещение целиком внутри острова (проверка по точкам через ~1 мм)
    auto inside_zone = [&](int zone, const Pt& a, const Pt& b) {
      if (zone < 0 || zone != last_zone) return false;
      const Area& z = L.zones[static_cast<std::size_t>(zone)];
      const double len = std::sqrt(dist2(a, b));
      const int k = std::clamp(static_cast<int>(std::ceil(len / 1.0)), 1, 32);
      for (int j = 0; j <= k; ++j) {
        const double t = static_cast<double>(j) / k;
        if (!z.contains({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t})) return false;
      }
      return true;
    };
    auto emit = [&](const Line& ln) {
      if (cur != ln.role) {
        g.line(std::string(";TYPE:") + role_label(ln.role));
        cur = ln.role;
      }
      double v = role_speed(ln.role, i, L.h);
      if (slow < 1.0) v = std::max(v * slow, std::min(v, fl.slowdown_speed));
      const double over_v = std::min(v, s.speed_overhang);
      const bool quiet = !ln.pts.empty() && inside_zone(ln.zone, g.pos(), ln.pts.front());
      g.extrude(ln, w, L.h, v, over_v, over_fan, role_accel(ln.role, i),
                ln.role == Role::Support || ln.role == Role::SupportInterface, quiet);
      last_zone = ln.zone;
    };
    for (const auto& ln : L.pre) emit(ln);
    if (!L.part.empty()) {
      if (label) g.line("EXCLUDE_OBJECT_START NAME=" + obj);
      cur.reset();
      for (const auto& ln : L.part) emit(ln);
      if (label) g.line("EXCLUDE_OBJECT_END NAME=" + obj);
    }
  }
  g.retract();
  g.line("; MACHINE_END_GCODE_START");
  g.line(substitute(pr.end_gcode, vars));
  g.line("; MACHINE_END_GCODE_END");
  g.line("");

  const double fil_area = std::numbers::pi * 0.25 * pr.filament_diameter * pr.filament_diameter;
  res.layers = static_cast<int>(n);
  res.filament_mm = g.filament();
  res.filament_g = g.filament() * fil_area * 1e-3 * fl.density;
  res.print_time_s = g.time();
  res.support_g = g.support_filament() * fil_area * 1e-3 * fl.density;
  res.cost = res.filament_g * 1e-3 * fl.cost;

  // итоги — как у PrusaSlicer: по ним Moonraker показывает время и пластик
  g.line(std::format("; filament used [mm] = {:.2f}", res.filament_mm));
  g.line(std::format("; filament used [cm3] = {:.2f}", res.filament_mm * fil_area * 1e-3));
  g.line(std::format("; filament used [g] = {:.2f}", res.filament_g));
  g.line(std::format("; filament cost = {:.2f}", res.cost));
  g.line(std::format("; total filament used [g] = {:.2f}", res.filament_g));
  g.line(std::format("; total layers count = {}", n));
  g.line(std::format("; estimated printing time (normal mode) = {}", prusa_time(res.print_time_s)));
  g.line("");
  // настройки — как у Orca и PrusaSlicer, их читает разбор G-code
  g.line("; CONFIG_BLOCK_START");
  g.line(std::format("; printer_model = {}", pr.name));
  g.line(std::format("; layer_height = {:g}", s.layer_height));
  g.line(std::format("; first_layer_height = {:g}", s.first_layer_height));
  g.line(std::format("; initial_layer_print_height = {:g}", s.first_layer_height));
  g.line(std::format("; nozzle_diameter = {:g}", pr.nozzle));
  g.line(std::format("; filament_diameter = {:g}", pr.filament_diameter));
  g.line(std::format("; filament_type = {}", fl.type));
  g.line(std::format("; filament_settings_id = \"{}\"", fl.name));
  g.line(std::format("; filament_density = {:g}", fl.density));
  g.line(std::format("; line_width = {:g}", s.line_width));
  g.line(std::format("; wall_loops = {}", s.wall_loops));
  g.line(std::format("; top_shell_layers = {}", s.top_layers));
  g.line(std::format("; bottom_shell_layers = {}", s.bottom_layers));
  g.line(std::format("; sparse_infill_density = {:g}%", s.infill_density * 100));
  g.line(std::format("; sparse_infill_pattern = {}", pattern_key(s.pattern)));
  g.line(std::format("; enable_support = {}", s.support == Support::None ? 0 : 1));
  if (s.support != Support::None) {
    g.line(std::format("; support_type = {}", s.support == Support::Tree ? "tree(auto)" : "normal(auto)"));
    g.line(std::format("; support_threshold_angle = {:g}", s.support_angle));
    g.line(std::format("; support_on_build_plate_only = {}", s.support_on_bed_only ? 1 : 0));
  }
  g.line(std::format("; nozzle_temperature = {:g}", fl.nozzle_temp));
  g.line(std::format("; first_layer_temperature = {:g}", fl.first_layer_nozzle_temp));
  g.line(std::format("; hot_plate_temp = {:g}", fl.bed_temp));
  g.line(std::format("; first_layer_bed_temperature = {:g}", fl.first_layer_bed_temp));
  g.line(std::format("; chamber_temperature = {:g}", chamber));
  g.line("; CONFIG_BLOCK_END");

  res.gcode = std::move(g.text());
  if (progress) progress(1.0, "Готово");
  return res;
}

}  // namespace kika::slicer
