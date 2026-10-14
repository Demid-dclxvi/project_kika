// Настройки нарезки как в OrcaSlicer: ширина линий по видам, толщина верха и низа, рисунки
// сплошных слоёв, глажка, поправки размеров, мосты, скорости; масштаб и положение детали.

#include <cmath>
#include <format>
#include <map>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/job.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/geometry/mesh.hpp"
#include "kika/slicer/print_job.hpp"
#include "kika/slicer/profiles.hpp"
#include "kika/slicer/slicer.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::geometry::TriangleMesh;
namespace ks = kika::slicer;

namespace {

TriangleMesh box(double a, double b, double c, double x = 0, double y = 0, double z = 0) {
  TriangleMesh m;
  for (int i = 0; i < 8; ++i)
    m.vertices.push_back({x + ((i & 1) ? a : 0.0), y + ((i & 2) ? b : 0.0), z + ((i & 4) ? c : 0.0)});
  m.triangles = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                 {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  return m;
}

// Труба: наружный радиус R, отверстие r, высота hgt, центр в (cx, cy).
TriangleMesh tube(double R, double r, double hgt, double cx, double cy) {
  TriangleMesh m;
  const int seg = 96;
  const double rr[2] = {R, r};
  for (int k = 0; k < 2; ++k)
    for (int z = 0; z < 2; ++z)
      for (int i = 0; i < seg; ++i) {
        const double a = 2 * std::numbers::pi * i / seg;
        m.vertices.push_back({cx + rr[k] * std::cos(a), cy + rr[k] * std::sin(a), hgt * z});
      }
  auto v = [&](int k, int z, int i) { return static_cast<std::uint32_t>((k * 2 + z) * seg + (i % seg)); };
  for (int i = 0; i < seg; ++i) {
    m.triangles.push_back({v(0, 0, i), v(0, 0, i + 1), v(0, 1, i + 1)});
    m.triangles.push_back({v(0, 0, i), v(0, 1, i + 1), v(0, 1, i)});
    m.triangles.push_back({v(1, 0, i), v(1, 1, i + 1), v(1, 0, i + 1)});
    m.triangles.push_back({v(1, 0, i), v(1, 1, i), v(1, 1, i + 1)});
    m.triangles.push_back({v(0, 1, i), v(0, 1, i + 1), v(1, 1, i + 1)});
    m.triangles.push_back({v(0, 1, i), v(1, 1, i + 1), v(1, 1, i)});
    m.triangles.push_back({v(0, 0, i), v(1, 0, i + 1), v(0, 0, i + 1)});
    m.triangles.push_back({v(0, 0, i), v(1, 0, i), v(1, 0, i + 1)});
  }
  return m;
}

// Движение с выдавливанием из G-code (относительная экструзия M83).
struct Move {
  double x0, y0, x1, y1, e, f;
  std::string type;
  int layer;
  double len() const { return std::hypot(x1 - x0, y1 - y0); }
};

struct Scan {
  std::vector<Move> moves;
  int layers = 0;
  std::vector<std::string> travels_in;  // тип, в котором было перемещение G0
  int retracts = 0;
};

std::optional<double> word(const std::string& l, char c) {
  for (std::size_t i = 0; i + 1 < l.size(); ++i)
    if (l[i] == c && (i == 0 || l[i - 1] == ' ')) return std::stod(l.substr(i + 1));
  return std::nullopt;
}

Scan scan(const std::string& g) {
  Scan s;
  std::istringstream in(g);
  double x = 0, y = 0, f = 0;
  std::string type;
  int layer = -1;
  for (std::string l; std::getline(in, l);) {
    if (l.starts_with(";LAYER_CHANGE")) {
      ++layer;
      type.clear();
      continue;
    }
    if (l.starts_with(";TYPE:")) {
      type = l.substr(6);
      continue;
    }
    if (!(l.starts_with("G0 ") || l.starts_with("G1 "))) continue;
    const auto nx = word(l, 'X'), ny = word(l, 'Y'), ne = word(l, 'E'), nf = word(l, 'F');
    if (nf) f = *nf;
    if (ne && !nx && !ny && *ne < 0) ++s.retracts;
    if (l.starts_with("G0 ") && layer >= 0) s.travels_in.push_back(type);
    const double x1 = nx.value_or(x), y1 = ny.value_or(y);
    if (ne && *ne > 0 && (nx || ny) && layer >= 0) s.moves.push_back({x, y, x1, y1, *ne, f, type, layer});
    x = x1;
    y = y1;
  }
  s.layers = layer + 1;
  return s;
}

double bead(double w, double h) { return (w - h) * h + std::numbers::pi * 0.25 * h * h; }

// Пластик на миллиметр линии для вида type на слое layer (среднее по длине).
double e_per_mm(const Scan& sc, const std::string& type, int layer) {
  double e = 0, l = 0;
  for (const auto& m : sc.moves)
    if (m.type == type && m.layer == layer) e += m.e, l += m.len();
  return l > 0 ? e / l : 0;
}

ks::Settings basic() {
  ks::Settings s;
  s.filament.flow = 1.0;
  s.filament.slowdown_time = 0;  // без замедления коротких слоёв — скорости как в настройках
  s.skirt = false;
  return s;
}

TriangleMesh on_bed(TriangleMesh m, const ks::Settings& s) {
  m.transform(kika::geometry::place_on_bed(m, s.printer.bed_x / 2, s.printer.bed_y / 2));
  return m;
}

}  // namespace

TEST_CASE("Настройки: ширина линий по видам и первого слоя", "[slicer][settings]") {
  auto s = basic();
  s.outer_wall_width = 0.6;
  s.first_layer_width = 0.5;
  const auto m = on_bed(box(20, 20, 2), s);
  const auto r = ks::slice(m, s);
  const auto sc = scan(r.gcode);
  const double fil = std::numbers::pi * 0.25 * 1.75 * 1.75;
  // слой 1: наружная стенка 0,6, внутренняя — по умолчанию 0,42
  CHECK_THAT(e_per_mm(sc, "Outer wall", 1) * fil, WithinRel(bead(0.6, 0.2), 1e-3));
  CHECK_THAT(e_per_mm(sc, "Inner wall", 1) * fil, WithinRel(bead(0.42, 0.2), 1e-3));
  // первый слой — все линии 0,5
  CHECK_THAT(e_per_mm(sc, "Outer wall", 0) * fil, WithinRel(bead(0.5, 0.2), 1e-3));
  CHECK_THAT(e_per_mm(sc, "Bottom surface", 0) * fil, WithinRel(bead(0.5, 0.2), 1e-3));
  // наружная стенка на слое 1 — в 0,3 мм от края детали
  double xmin = 1e9;
  for (const auto& mv : sc.moves)
    if (mv.type == "Outer wall" && mv.layer == 1) xmin = std::min({xmin, mv.x0, mv.x1});
  CHECK_THAT(xmin, WithinAbs(m.bbox().min[0] + 0.3, 0.01));
  // внутренняя стенка вплотную: (0,6 + 0,42)/2 − перекрытие валиков
  double xin = 1e9;
  for (const auto& mv : sc.moves)
    if (mv.type == "Inner wall" && mv.layer == 1) xin = std::min({xin, mv.x0, mv.x1});
  const double ov = 0.2 * (1 - std::numbers::pi / 4);
  CHECK_THAT(xin - xmin, WithinAbs(0.5 * (0.6 - ov + 0.42 - ov), 0.01));
  CHECK(r.gcode.find("; outer_wall_line_width = 0.6") != std::string::npos);
}

TEST_CASE("Настройки: толщина верха и низа добавляет сплошные слои", "[slicer][settings]") {
  auto s = basic();
  s.top_layers = 3;
  s.bottom_layers = 2;
  s.top_thickness = 1.0;     // 5 слоёв по 0,2
  s.bottom_thickness = 0.8;  // 4 слоя
  const auto r = ks::slice(on_bed(box(20, 20, 10), s), s);
  const auto sc = scan(r.gcode);
  std::set<int> sparse;
  for (const auto& m : sc.moves)
    if (m.type == "Sparse infill") sparse.insert(m.layer);
  CHECK(sc.layers == 50);
  CHECK(static_cast<int>(sparse.size()) == 50 - 5 - 4);
  CHECK(*sparse.begin() == 4);
  CHECK(*sparse.rbegin() == 44);
  CHECK(r.gcode.find("; top_shell_layers = 5") != std::string::npos);
  CHECK(r.gcode.find("; bottom_shell_layers = 4") != std::string::npos);
}

TEST_CASE("Настройки: рисунки верха — монотонный, без соединений, концентрический", "[slicer][settings]") {
  auto s = basic();
  s.infill_density = 0.15;
  const auto m = on_bed(box(20, 20, 2), s);
  const int last = 9;
  auto top_moves = [&](ks::SurfacePattern p) {
    s.top_pattern = p;
    const auto sc = scan(ks::slice(m, s).gcode);
    std::vector<Move> out;
    for (const auto& mv : sc.moves)
      if (mv.type == "Top surface" && mv.layer == last) out.push_back(mv);
    int travels = 0;
    // перемещения внутри верха: подписи у перемещений нет, считаем по G0 между линиями верха
    for (std::size_t k = 1; k < out.size(); ++k)
      if (std::hypot(out[k].x0 - out[k - 1].x1, out[k].y0 - out[k - 1].y1) > 1e-6) ++travels;
    return std::pair{out, travels};
  };
  // без соединений: все линии под углом верха, ряд за рядом в одну сторону по нормали
  {
    const auto [mv, travels] = top_moves(ks::SurfacePattern::MonotonicLine);
    REQUIRE(mv.size() > 20);
    const double ang = (last % 2 == 0 ? 45.0 : -45.0) * std::numbers::pi / 180;
    const double nx = -std::sin(ang), ny = std::cos(ang);
    int dir = 0;
    bool monotonic = true;
    for (std::size_t k = 0; k < mv.size(); ++k) {
      const double c = std::abs((mv[k].x1 - mv[k].x0) * std::cos(ang) + (mv[k].y1 - mv[k].y0) * std::sin(ang)) / mv[k].len();
      CHECK(c > 0.9999);
      if (k == 0) continue;
      const double t0 = mv[k - 1].x0 * nx + mv[k - 1].y0 * ny, t1 = mv[k].x0 * nx + mv[k].y0 * ny;
      const int d = t1 > t0 + 1e-6 ? 1 : (t1 < t0 - 1e-6 ? -1 : 0);
      if (dir == 0) dir = d;
      monotonic = monotonic && (d == 0 || d == dir);
    }
    CHECK(monotonic);
    CHECK(travels == static_cast<int>(mv.size()) - 1);  // каждая линия отдельно
  }
  // монотонный с соединениями: квадрат — одна непрерывная линия
  {
    const auto [mv, travels] = top_moves(ks::SurfacePattern::Monotonic);
    REQUIRE(mv.size() > 20);
    CHECK(travels == 0);
  }
  // концентрический: петли вдоль сторон квадрата
  {
    const auto [mv, travels] = top_moves(ks::SurfacePattern::Concentric);
    REQUIRE(mv.size() > 20);
    double along = 0, all = 0;
    for (const auto& x : mv) {
      all += x.len();
      if (std::abs(x.x1 - x.x0) < 1e-6 || std::abs(x.y1 - x.y0) < 1e-6) along += x.len();
    }
    CHECK(along > 0.95 * all);
  }
  // ключи вариантов как в Orca
  CHECK(ks::surface_from_key("monotonicline") == ks::SurfacePattern::MonotonicLine);
  CHECK(ks::surface_from_key("zig-zag") == ks::SurfacePattern::Rectilinear);
  CHECK(!ks::surface_from_key("hilbertcurve").has_value());
}

TEST_CASE("Настройки: глажка верха — только самый верх или все верхние поверхности", "[slicer][settings]") {
  // ступенька: широкое основание 30 × 30 × 4 и узкая башня 10 × 10 × 4 сверху
  auto s = basic();
  TriangleMesh m = box(30, 30, 4);
  m.append(box(10, 10, 4, 10, 10, 4));
  m = on_bed(m, s);
  const auto plain = ks::slice(m, s);
  auto ironed_layers = [&](ks::Ironing kind) {
    s.ironing = kind;
    const auto r = ks::slice(m, s);
    std::set<int> layers;
    for (const auto& mv : scan(r.gcode).moves)
      if (mv.type == "Ironing") layers.insert(mv.layer);
    return std::pair{layers, r};
  };
  const auto [top_all, r_all] = ironed_layers(ks::Ironing::Top);
  CHECK(top_all == std::set<int>{19, 39});  // верх основания (кольцо) и верх башни
  const auto [topmost, r_most] = ironed_layers(ks::Ironing::Topmost);
  CHECK(topmost == std::set<int>{39});
  // поток глажки: доля обычного слоя на той же площади
  const auto sc = scan(r_most.gcode);
  const double fil = std::numbers::pi * 0.25 * 1.75 * 1.75;
  CHECK_THAT(e_per_mm(sc, "Ironing", 39) * fil, WithinRel(s.ironing_flow * s.ironing_spacing * 0.2, 1e-3));
  for (const auto& mv : sc.moves)
    if (mv.type == "Ironing") REQUIRE_THAT(mv.f, WithinAbs(s.speed_ironing * 60, 1e-6));
  // глажка не деталь: в расчёт не идёт, объём детали тот же
  CHECK_THAT(kika::gcode::parse(r_most.gcode).total_volume(), WithinRel(kika::gcode::parse(plain.gcode).total_volume(), 1e-9));
  CHECK(plain.gcode.find(";TYPE:Ironing") == std::string::npos);
}

TEST_CASE("Настройки: поправки отверстий, контура и «слоновьей ноги»", "[slicer][settings]") {
  auto s = basic();
  const auto t = tube(10, 6, 2, 135, 135);
  auto radius = [&](const ks::Result& r, int layer, bool hole) {
    double best = hole ? 1e9 : 0;
    for (const auto& mv : scan(r.gcode).moves)
      if (mv.type == "Outer wall" && mv.layer == layer) {
        const double d = std::hypot(mv.x0 - 135, mv.y0 - 135);
        best = hole ? std::min(best, d) : std::max(best, d);
      }
    return best;
  };
  const auto r0 = ks::slice(t, s);
  CHECK_THAT(radius(r0, 5, true), WithinAbs(6.21, 0.03));
  CHECK_THAT(radius(r0, 5, false), WithinAbs(9.79, 0.03));
  s.xy_hole_comp = 0.1;
  s.xy_contour_comp = -0.05;
  s.elephant_foot = 0.15;
  const auto r1 = ks::slice(t, s);
  CHECK_THAT(radius(r1, 5, true), WithinAbs(6.31, 0.03));
  CHECK_THAT(radius(r1, 5, false), WithinAbs(9.74, 0.03));
  // первый слой: ещё на 0,15 мм уже снаружи и шире в отверстии
  CHECK_THAT(radius(r1, 0, false), WithinAbs(9.59, 0.03));
  CHECK_THAT(radius(r1, 0, true), WithinAbs(6.46, 0.03));
}

TEST_CASE("Настройки: мост между опорами — линии поперёк пролёта, свой поток и скорость", "[slicer][settings]") {
  // две опоры 6 × 10 × 5 на расстоянии 20 мм и плита 32 × 10 × 2 на них
  auto s = basic();
  s.bridge_flow = 0.85;
  s.speed_bridge = 40;
  TriangleMesh m = box(6, 10, 5);
  m.append(box(6, 10, 5, 26, 0, 0));
  m.append(box(32, 10, 2, 0, 0, 5));
  m = on_bed(m, s);
  const auto r = ks::slice(m, s);
  const auto sc = scan(r.gcode);
  double along = 0, all = 0, e = 0;
  std::set<int> layers;
  for (const auto& mv : sc.moves)
    if (mv.type == "Bridge") {
      layers.insert(mv.layer);
      all += mv.len();
      e += mv.e;
      if (std::abs(mv.y1 - mv.y0) < 1e-6) along += mv.len();
      REQUIRE_THAT(mv.f, WithinAbs(40 * 60, 1e-6));
    }
  REQUIRE(!layers.empty());
  CHECK(*layers.begin() == 25);    // первый слой плиты
  CHECK(along > 0.9 * all);        // вдоль X — от опоры до опоры
  const double fil = std::numbers::pi * 0.25 * 1.75 * 1.75;
  CHECK_THAT(e / all * fil, WithinRel(0.85 * bead(0.42, 0.2), 0.02));
  // обдув мостов — как у нависаний
  CHECK(r.gcode.find(std::format("M106 S{}", std::lround(s.filament.overhang_fan * 255))) != std::string::npos);
}

TEST_CASE("Настройки: скорости мелких периметров и первого слоя", "[slicer][settings]") {
  auto s = basic();
  s.speed_first_layer = 30;
  s.speed_first_layer_infill = 80;
  s.filament.max_volumetric = 100;  // без предела расхода — скорости как заданы
  const auto t = tube(10, 6, 2, 135, 135);
  const auto sc = scan(ks::slice(t, s).gcode);
  std::set<double> hole_f, outer_f, first_wall_f, first_fill_f;
  for (const auto& mv : sc.moves) {
    if (mv.layer == 0) {
      if (mv.type == "Outer wall" || mv.type == "Inner wall") first_wall_f.insert(mv.f);
      if (mv.type == "Bottom surface") first_fill_f.insert(mv.f);
      continue;
    }
    if (mv.type != "Outer wall") continue;
    (std::hypot(mv.x0 - 135, mv.y0 - 135) < 8 ? hole_f : outer_f).insert(mv.f);
  }
  // отверстие ⌀ 12 мм: периметр короче окружности радиуса 6,5 — половина скорости наружной стенки
  CHECK(hole_f == std::set<double>{0.5 * s.speed_outer_wall * 60});
  CHECK(outer_f == std::set<double>{s.speed_outer_wall * 60});
  CHECK(first_wall_f == std::set<double>{30 * 60});
  CHECK(first_fill_f == std::set<double>{80 * 60});
}

TEST_CASE("Настройки: юбка в несколько слоёв, откаты внутри детали", "[slicer][settings]") {
  auto s = basic();
  s.skirt = true;
  s.skirt_layers = 3;
  s.infill_density = 0.2;
  TriangleMesh m = box(20, 20, 2);
  m.append(box(5, 5, 2, 30, 0, 0));  // два острова: между ними перемещения с откатом
  m = on_bed(m, s);
  const auto r = ks::slice(m, s);
  std::set<int> skirt;
  const auto sc = scan(r.gcode);
  for (const auto& mv : sc.moves)
    if (mv.type == "Skirt") skirt.insert(mv.layer);
  CHECK(skirt == std::set<int>{0, 1, 2});
  s.reduce_retraction = false;
  const auto sc2 = scan(ks::slice(m, s).gcode);
  CHECK(sc2.retracts > sc.retracts + 10);
}

TEST_CASE("Настройки: страницы и разделы как в Orca, новые поля в JSON", "[slicer][settings][profiles]") {
  CHECK(ks::pages(ks::Section::Process) == std::vector<std::string>{"Качество", "Прочность", "Скорость", "Поддержки", "Прочее"});
  CHECK(ks::pages(ks::Section::Printer) == std::vector<std::string>{"Основное", "Экструдер", "Движение", "G-code"});
  CHECK(ks::pages(ks::Section::Filament) == std::vector<std::string>{"Пластик", "Охлаждение", "Откат", "G-code"});
  for (const auto& f : ks::fields()) CHECK(!f.page.empty());
  ks::Settings a;
  ks::section_from_json(a, ks::Section::Process,
                        kika::json::parse(R"({"top_pattern": "concentric", "bottom_pattern": "monotonicline",
      "ironing": "topmost", "ironing_flow": 12, "bridge_flow": 90, "small_perimeter_speed": 40,
      "outer_wall_width": 0.45, "top_thickness": 0.8, "elephant_foot": 0.1, "skirt_layers": 2,
      "reduce_retraction": false})"),
                        "print");
  CHECK(a.top_pattern == ks::SurfacePattern::Concentric);
  CHECK(a.bottom_pattern == ks::SurfacePattern::MonotonicLine);
  CHECK(a.ironing == ks::Ironing::Topmost);
  CHECK_THAT(a.ironing_flow, WithinAbs(0.12, 1e-12));
  CHECK_THAT(a.bridge_flow, WithinAbs(0.9, 1e-12));
  CHECK_THAT(a.small_perimeter_speed, WithinAbs(0.4, 1e-12));
  CHECK(a.outer_wall_width == 0.45);
  CHECK(a.skirt_layers == 2);
  CHECK_FALSE(a.reduce_retraction);
  const auto j = ks::section_to_json(a, ks::Section::Process);
  ks::Settings b;
  ks::section_from_json(b, ks::Section::Process, j, "print");
  CHECK(kika::json::dump(ks::section_to_json(b, ks::Section::Process)) == kika::json::dump(j));
  CHECK_THROWS_AS(ks::section_from_json(b, ks::Section::Process, kika::json::parse(R"({"ironing": "всё"})"), "print"),
                  kika::analysis::JobError);
}

TEST_CASE("Задание: масштаб и положение детали на столе", "[slicer][settings]") {
  const auto j = kika::json::parse(R"({"rotate": [0, 0, 90], "scale": [2, 1, 1.5], "position": [60, 80]})");
  const auto p = ks::print_from_json(&j, "PLA");
  CHECK(p.scale == std::array<double, 3>{2, 1, 1.5});
  REQUIRE(p.position.has_value());
  // брусок 10 × 20 × 4: после поворота вокруг Z — 20 × 10, после масштаба по осям стола — 40 × 10 × 6
  const auto on = ks::placed(box(10, 20, 4), p);
  const auto bb = on.bbox();
  CHECK_THAT(bb.size()[0], WithinAbs(40, 1e-9));
  CHECK_THAT(bb.size()[1], WithinAbs(10, 1e-9));
  CHECK_THAT(bb.size()[2], WithinAbs(6, 1e-9));
  CHECK_THAT(bb.center()[0], WithinAbs(60, 1e-9));
  CHECK_THAT(bb.center()[1], WithinAbs(80, 1e-9));
  CHECK_THAT(bb.min[2], WithinAbs(0, 1e-12));
  // обратно в JSON и снова
  const auto j2 = ks::print_to_json(p);
  const auto p2 = ks::print_from_json(&j2, "PLA");
  CHECK(p2.scale == p.scale);
  CHECK(*p2.position == *p.position);
  // одинаковый масштаб пишется одним числом
  ks::PrintJob u;
  u.scale = {1.5, 1.5, 1.5};
  CHECK(ks::print_to_json(u).find("scale")->as_double() == 1.5);
  CHECK(ks::print_to_json(ks::PrintJob{}).find("scale") == nullptr);
  // переход между постановками: L(to) = change · L(from)
  ks::PrintJob a, b;
  a.rotate = {90, 0, 0};
  a.scale = {1, 2, 1};
  b.rotate = {0, 90, 0};
  b.scale = {3, 1, 0.5};
  const auto c = ks::change_between(a, b) * ks::linear_of(a);
  const auto lb = ks::linear_of(b);
  for (std::size_t i = 0; i < 9; ++i) CHECK_THAT(c.r[i], WithinAbs(lb.r[i], 1e-12));
  // ошибки по-русски
  for (const char* bad : {R"({"scale": 0})", R"({"scale": [1, 2]})", R"({"position": [1]})", R"({"scale": "2"})"}) {
    const auto jb = kika::json::parse(bad);
    CHECK_THROWS_AS(ks::print_from_json(&jb, "PLA"), kika::analysis::JobError);
  }
}

// ---------------------------------------------------------------------------- как в Orca, часть 2

namespace {

std::vector<std::string> lines_of(const std::string& g) {
  std::vector<std::string> out;
  std::istringstream in(g);
  for (std::string l; std::getline(in, l);) out.push_back(l);
  return out;
}

int count_lines(const std::string& g, const std::string& exact) {
  int n = 0;
  for (const auto& l : lines_of(g))
    if (l == exact) ++n;
  return n;
}

}  // namespace

TEST_CASE("Настройки: зазор шва — замкнутая стенка обрывается перед началом", "[slicer][settings]") {
  auto s = basic();
  s.wall_loops = 1;
  s.seam_gap = 0.5;  // 50 % сопла 0,4 — 0,2 мм
  const auto m = on_bed(box(20, 20, 1), s);
  const auto sc = scan(ks::slice(m, s).gcode);
  std::vector<Move> wall;
  for (const auto& mv : sc.moves)
    if (mv.layer == 2 && mv.type == "Outer wall") wall.push_back(mv);
  REQUIRE(wall.size() >= 4);
  const double gap = std::hypot(wall.back().x1 - wall.front().x0, wall.back().y1 - wall.front().y0);
  CHECK_THAT(gap, WithinAbs(0.2, 1e-3));
  s.seam_gap = 0;
  const auto sc0 = scan(ks::slice(m, s).gcode);
  wall.clear();
  for (const auto& mv : sc0.moves)
    if (mv.layer == 2 && mv.type == "Outer wall") wall.push_back(mv);
  CHECK(std::hypot(wall.back().x1 - wall.front().x0, wall.back().y1 - wall.front().y0) < 1e-6);
}

TEST_CASE("Настройки: откат — протирка, подача, смещение Z, без отката при смене слоя", "[slicer][settings][printer]") {
  auto s = basic();
  s.infill_density = 0.2;
  TriangleMesh m = box(20, 20, 2);
  m.append(box(5, 5, 2, 30, 0, 0));
  m = on_bed(m, s);
  s.printer.wipe = true;
  s.printer.wipe_distance = 2;
  s.printer.retract_restart_extra = 0.2;
  s.printer.deretract_speed = 20;
  s.printer.z_offset = 0.1;
  const auto r = ks::slice(m, s);
  const auto ls = lines_of(r.gcode);
  // протирка: назад по линии на 2 мм, откат целиком на ходу
  int wipes = 0;
  for (std::size_t i = 0; i < ls.size(); ++i) {
    if (ls[i] != ";WIPE_START") continue;
    ++wipes;
    double e = 0;
    for (std::size_t k = i + 1; k < ls.size() && ls[k] != ";WIPE_END"; ++k) e += word(ls[k], 'E').value_or(0);
    CHECK_THAT(e, WithinAbs(-0.8, 1e-4));
  }
  CHECK(wipes > 5);
  // подача: откат + добавка, своя скорость (после последнего отката подачи нет)
  CHECK(count_lines(r.gcode, "G1 E1.00000 F1200") == wipes + count_lines(r.gcode, "G1 E-0.80000 F1800") - 1);
  // высоты слоёв — со смещением
  std::vector<std::string> layer_z;
  for (std::size_t i = 0; i < ls.size(); ++i)
    if (ls[i].starts_with(";HEIGHT:"))
      for (std::size_t k = i + 1; k < ls.size(); ++k)
        if (ls[k].starts_with("G1 Z")) {
          layer_z.push_back(ls[k]);
          break;
        }
  REQUIRE(layer_z.size() == 10);
  CHECK(layer_z[0] == "G1 Z0.300 F1200");
  CHECK(layer_z[1] == "G1 Z0.500 F1200");
  // без отката при смене слоя — откатов меньше (перемещения внутри детали — без отката)
  s.printer.wipe = false;
  const auto one = on_bed(box(20, 20, 2), s);
  const int with = scan(ks::slice(one, s).gcode).retracts;
  s.printer.retract_layer_change = false;
  const int without = scan(ks::slice(one, s).gcode).retracts;
  CHECK(without < with);
}

TEST_CASE("Настройки: ваза — одна стенка спиралью, Z растёт вдоль линии", "[slicer][settings]") {
  auto s = basic();
  s.spiral_vase = true;
  s.bottom_layers = 3;
  s.infill_density = 0.3;  // в вазе — не используется
  const auto r = ks::slice(on_bed(box(20, 20, 6), s), s);
  CHECK(r.warnings.empty());
  const auto ls = lines_of(r.gcode);
  int layer = -1, spiral_moves = 0, z_steps = 0;
  double last_z = 0;
  std::set<std::string> types_after;
  for (const auto& l : ls) {
    if (l == "; MACHINE_END_GCODE_START") break;
    if (l == ";LAYER_CHANGE") ++layer;
    if (l.starts_with(";TYPE:") && layer >= 3) types_after.insert(l.substr(6));
    if (l.starts_with("G1 Z") && layer >= 4) ++z_steps;  // слой 3 — переход с дна на спираль
    if (l.starts_with("G1 X") && l.find(" Z") != std::string::npos && l.find(" E") != std::string::npos) {
      const double z = *word(l, 'Z');
      CHECK(z >= last_z - 1e-9);  // только вверх
      last_z = z;
      ++spiral_moves;
    }
  }
  CHECK(types_after == std::set<std::string>{"Outer wall"});
  CHECK(z_steps == 0);  // над дном — ни одного отдельного подъёма
  CHECK(spiral_moves > 50);
  CHECK_THAT(last_z, WithinAbs(6.0, 1e-3));
}

TEST_CASE("Настройки: нечёткая поверхность — наружная стенка в полосе ±толщина/2, кроме первого слоя", "[slicer][settings]") {
  auto s = basic();
  s.wall_loops = 2;
  s.seam_gap = 0;
  s.fuzzy_skin = ks::FuzzySkin::Contour;
  s.fuzzy_thickness = 0.4;
  s.fuzzy_point_dist = 0.6;
  const auto m = on_bed(box(20, 20, 1), s);
  const auto sc = scan(ks::slice(m, s).gcode);
  const double c = s.printer.bed_x / 2;  // центр бруска; стенка — в 10 − w/2 от центра
  const double half = 10 - s.line_width / 2;
  double dev0 = 0, dev = 0;
  std::size_t pts1 = 0, pts0 = 0;
  for (const auto& mv : sc.moves) {
    if (mv.type != "Outer wall") continue;
    const double dx = std::abs(mv.x1 - c), dy = std::abs(mv.y1 - c);
    const double d = std::abs(std::max(dx, dy) - half);  // от номинальной линии стенки
    if (mv.layer == 0) {
      dev0 = std::max(dev0, d);
      ++pts0;
    } else {
      dev = std::max(dev, d);
      if (mv.layer == 1) ++pts1;
    }
  }
  CHECK(dev0 < 1e-3);
  CHECK(dev <= 0.2 + 1e-3);
  CHECK(dev > 0.1);
  CHECK(pts1 > 5 * pts0);  // 80 мм контура через ~0,6 мм
  // внутренние стенки не дрожат
  s.fuzzy_skin = ks::FuzzySkin::All;
  const auto sc2 = scan(ks::slice(m, s).gcode);
  std::size_t inner = 0;
  for (const auto& mv : sc2.moves)
    if (mv.type == "Inner wall" && mv.layer == 2) ++inner;
  CHECK(inner > 50);
}

TEST_CASE("Настройки: подложка под деталью", "[slicer][settings][support]") {
  auto s = basic();
  s.raft_layers = 3;
  s.raft_expansion = 2;
  s.raft_contact_distance = 0.1;
  const auto m = on_bed(box(20, 20, 2), s);
  const auto r = ks::slice(m, s);
  CHECK(r.layers == 10 + 3);
  const auto sc = scan(r.gcode);
  std::map<int, std::set<std::string>> types;
  double raft_max_x = -1e9, part_max_x = -1e9;
  for (const auto& mv : sc.moves) {
    types[mv.layer].insert(mv.type);
    if (mv.layer == 0) raft_max_x = std::max(raft_max_x, mv.x1);
    if (mv.layer == 3 && mv.type == "Outer wall") part_max_x = std::max(part_max_x, mv.x1);
  }
  CHECK(types[0] == std::set<std::string>{"Support"});
  CHECK(types[2] == std::set<std::string>{"Support interface"});
  CHECK(types[3].count("Outer wall") == 1);
  // деталь поднята: подложка 0,2 + 0,2 + 0,2, зазор 0,1, первый слой детали 0,2
  std::vector<double> zs;
  for (const auto& l : lines_of(r.gcode))
    if (l.starts_with(";Z:")) zs.push_back(std::stod(l.substr(3)));
  REQUIRE(zs.size() == 13);
  CHECK_THAT(zs[2], WithinAbs(0.6, 1e-6));
  CHECK_THAT(zs[3], WithinAbs(0.9, 1e-6));
  CHECK_THAT(zs.back(), WithinAbs(2.7, 1e-6));
  CHECK(raft_max_x > part_max_x + 1.5);
}

TEST_CASE("Настройки: кайма снаружи, в отверстиях, нигде", "[slicer][settings]") {
  auto s = basic();
  s.brim_width = 3;
  const double cx = s.printer.bed_x / 2, cy = s.printer.bed_y / 2;
  const auto m = on_bed(tube(15, 6, 2, 0, 0), s);
  auto brim_r = [&](ks::BrimType t) {
    s.brim_type = t;
    double rmin = 1e9, rmax = 0;
    for (const auto& mv : scan(ks::slice(m, s).gcode).moves)
      if (mv.type == "Brim") {
        const double rr = std::hypot(mv.x1 - cx, mv.y1 - cy);
        rmin = std::min(rmin, rr);
        rmax = std::max(rmax, rr);
      }
    return std::pair{rmin, rmax};
  };
  const auto out = brim_r(ks::BrimType::Outer);
  CHECK(out.first > 15);
  CHECK(out.second < 18.5);
  const auto in = brim_r(ks::BrimType::Inner);
  CHECK(in.first > 2.5);
  CHECK(in.second < 6);
  const auto both = brim_r(ks::BrimType::Both);
  CHECK(both.first < 6);
  CHECK(both.second > 15);
  CHECK(brim_r(ks::BrimType::None).second == 0);
}

TEST_CASE("Настройки: пауза на слоях, очистка воздуха, код пластика, разгон обдува", "[slicer][settings][printer]") {
  auto s = basic();
  s.pause_layers = "3, 5";
  s.filament.air_filtration = true;
  s.filament.start_gcode = "; мой старт {filament_type}";
  s.filament.end_gcode = "; мой конец";
  s.filament.fan_off_layers = 1;
  s.filament.full_fan_layer = 5;
  s.filament.fan_min = s.filament.fan_max = 1.0;
  const auto m = on_bed(box(20, 20, 2), s);
  const auto r = ks::slice(m, s);
  CHECK(count_lines(r.gcode, "PAUSE") == 2);
  CHECK(count_lines(r.gcode, "M106 P3 S153") == 1);
  CHECK(count_lines(r.gcode, "M106 P3 S204") == 1);
  CHECK(count_lines(r.gcode, "; мой старт PETG") == 1);
  CHECK(count_lines(r.gcode, "; мой конец") == 1);
  // пауза — после смены слоя 3 и 5
  int layer = 0;
  std::vector<int> paused;
  std::vector<int> fan_by_layer(20, -1);
  for (const auto& l : lines_of(r.gcode)) {
    if (l == ";LAYER_CHANGE") ++layer;
    if (l == "PAUSE") paused.push_back(layer);
    if (l.starts_with("M106 S")) fan_by_layer[static_cast<std::size_t>(layer)] = std::stoi(l.substr(6));
  }
  CHECK(paused == std::vector<int>{3, 5});
  // обдув: слой 2 — четверть, 3 — половина, 4 — три четверти, 5 — полный
  CHECK(fan_by_layer[2] == 64);
  CHECK(fan_by_layer[3] == 128);
  CHECK(fan_by_layer[4] == 191);
  CHECK(fan_by_layer[5] == 255);
  // Marlin без вентилятора камеры: M601, без M106 P3
  s.printer = *ks::find_printer("generic_220");
  const auto r2 = ks::slice(on_bed(box(20, 20, 2), s), s);
  CHECK(count_lines(r2.gcode, "M601") == 2);
  CHECK(r2.gcode.find("M106 P3") == std::string::npos);
}

TEST_CASE("Настройки: усадка пластика, предупреждения сопла и высоты слоя", "[slicer][settings]") {
  auto s = basic();
  s.filament.shrinkage = 0.98;
  const auto m = on_bed(box(20, 20, 1), s);
  double x0 = 1e9, x1 = -1e9;
  for (const auto& mv : scan(ks::slice(m, s).gcode).moves)
    if (mv.type == "Outer wall") x0 = std::min(x0, mv.x1), x1 = std::max(x1, mv.x1);
  CHECK_THAT(x1 - x0 + s.line_width, WithinAbs(20 / 0.98, 0.02));
  // абразивный пластик и латунное сопло; слой толще предела сопла
  s.filament = ks::filament_preset("PA-CF");
  s.printer.nozzle_type = ks::NozzleType::Brass;
  s.layer_height = 0.36;
  const auto r = ks::slice(m, s);
  bool brass = false, layer = false;
  for (const auto& w : r.warnings) {
    brass = brass || w.find("латунное сопло") != std::string::npos;
    layer = layer || w.find("Высота слоя 0,36 мм") != std::string::npos;
  }
  CHECK(brass);
  CHECK(layer);
  s.printer.nozzle_type = ks::NozzleType::Hardened;
  s.layer_height = 0.2;
  CHECK(ks::slice(m, s).warnings.empty());
}

TEST_CASE("Настройки: слои паузы, запретная область стола, имя файла", "[slicer][settings]") {
  CHECK(ks::parse_layer_list("12, 30 5;30") == std::vector<int>{5, 12, 30});
  CHECK(ks::parse_layer_list("").empty());
  std::string err;
  CHECK(ks::parse_layer_list("3, x", &err).empty());
  CHECK(!err.empty());
  CHECK(ks::layer_list_text({5, 12}) == "5, 12");
  const auto z = ks::parse_bed_exclude("0x0,11x0,11x16,0x16; 200×0, 210x0, 210x5");
  REQUIRE(z.size() == 2);
  CHECK(z[0][2] == std::array<double, 2>{11, 16});
  CHECK(z[1][0] == std::array<double, 2>{200, 0});
  err.clear();
  CHECK(ks::parse_bed_exclude("0x0,11x0", &err).empty());
  CHECK(!err.empty());
  // деталь в углу Q2 — предупреждение, в центре — нет
  auto s = basic();
  const auto corner = ks::slice(box(10, 10, 2, 2, 2, 0), s);
  bool warned = false;
  for (const auto& w : corner.warnings) warned = warned || w.find("запретную область") != std::string::npos;
  CHECK(warned);
  // имя файла
  ks::Result r;
  r.print_time_s = 3725;
  r.filament_g = 12.345;
  s.filename_format = "{input_filename_base}_{filament_type[0]}_{layer_height}mm_{print_time}_{total_weight}g";
  CHECK(ks::output_filename(s, "кронштейн", &r) == "кронштейн_PETG_0.2mm_1h2m_12.35g.gcode");
  s.filename_format = "a/b:{printer_model}.GCODE";
  CHECK(ks::output_filename(s, "x") == "a_b_QIDI Q2.GCODE");
}

TEST_CASE("Настройки: новые поля в JSON — ключи как в Orca, проверка строк", "[slicer][settings][profiles]") {
  ks::Settings a;
  ks::section_from_json(a, ks::Section::Process,
                        kika::json::parse(R"({"fuzzy_skin": "allwalls", "brim_type": "inner_only", "seam_gap": 15,
      "raft_layers": 2, "spiral_vase": true, "pause_layers": "4, 8", "min_sparse_area": 5})"),
                        "print");
  CHECK(a.fuzzy_skin == ks::FuzzySkin::All);
  CHECK(a.brim_type == ks::BrimType::Inner);
  CHECK_THAT(a.seam_gap, WithinAbs(0.15, 1e-12));
  CHECK(a.raft_layers == 2);
  CHECK(a.spiral_vase);
  ks::section_from_json(a, ks::Section::Printer, kika::json::parse(R"({"nozzle_type": "brass", "wipe": true})"), "printer");
  CHECK(a.printer.nozzle_type == ks::NozzleType::Brass);
  ks::section_from_json(a, ks::Section::Filament, kika::json::parse(R"({"shrinkage": 99.5, "abrasive": true})"), "filament");
  CHECK_THAT(a.filament.shrinkage, WithinAbs(0.995, 1e-12));
  CHECK_THROWS_AS(ks::section_from_json(a, ks::Section::Process, kika::json::parse(R"({"pause_layers": "4, пять"})"), "print"),
                  kika::analysis::JobError);
  CHECK_THROWS_AS(
      ks::section_from_json(a, ks::Section::Printer, kika::json::parse(R"({"bed_exclude": "0x0,1"})"), "printer"),
      kika::analysis::JobError);
  // CF-пластики — абразивные, у принтеров с Marlin — латунное сопло
  CHECK(ks::filament_preset("PETG-CF").abrasive);
  CHECK_FALSE(ks::filament_preset("PETG").abrasive);
  CHECK(ks::find_printer("sparkx_i7")->nozzle_type == ks::NozzleType::Brass);
  CHECK(ks::default_printer().nozzle_type == ks::NozzleType::Hardened);
}
