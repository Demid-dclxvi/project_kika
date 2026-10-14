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
  CHECK(ks::pages(ks::Section::Filament) == std::vector<std::string>{"Пластик", "Охлаждение", "Откат"});
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
