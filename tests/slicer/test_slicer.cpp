// Свой слайсер: сечения, G-code, его разбор и расчёт по нему.

#include <cmath>
#include <filesystem>
#include <map>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/geometry/import.hpp"
#include "kika/geometry/mesh.hpp"
#include "kika/material/material.hpp"
#include "kika/slicer/print_job.hpp"
#include "kika/slicer/slicer.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::geometry::TriangleMesh;

namespace {

// Параллелепипед a × b × c с углом в (x, y, z), нормали наружу.
TriangleMesh box(double a, double b, double c, double x = 0, double y = 0, double z = 0) {
  TriangleMesh m;
  for (int i = 0; i < 8; ++i)
    m.vertices.push_back({x + ((i & 1) ? a : 0.0), y + ((i & 2) ? b : 0.0), z + ((i & 4) ? c : 0.0)});
  m.triangles = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                 {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  return m;
}

double loop_area(const std::vector<std::array<double, 2>>& p) {
  double a = 0;
  for (std::size_t i = 0; i < p.size(); ++i) {
    const auto& u = p[i];
    const auto& v = p[(i + 1) % p.size()];
    a += u[0] * v[1] - v[0] * u[1];
  }
  return a / 2;
}

}  // namespace

TEST_CASE("Слайсер: сечение — контур против часовой, тела объединяются", "[slicer]") {
  auto m = box(10, 20, 5);
  auto sec = kika::slicer::section(m, 2.5);
  REQUIRE(sec.size() == 1);
  CHECK_THAT(loop_area(sec[0]), WithinRel(200.0, 1e-9));
  // два пересекающихся бруска — одна область площадью объединения
  TriangleMesh two = box(10, 10, 5);
  two.append(box(10, 10, 5, 5, 5, 0));
  sec = kika::slicer::section(two, 2.5);
  REQUIRE(sec.size() == 1);
  CHECK_THAT(loop_area(sec[0]), WithinRel(175.0, 1e-9));
  // брусок с вывернутыми нормалями — всё равно тело (правило ненулевой обмотки)
  auto flipped = box(10, 10, 5);
  flipped.flip();
  sec = kika::slicer::section(flipped, 2.5);
  REQUIRE(sec.size() == 1);
  CHECK_THAT(std::abs(loop_area(sec[0])), WithinRel(100.0, 1e-9));
  // плоскость точно по верхним вершинам — вершина считается выше плоскости, сечение есть;
  // по нижним — касание, сечения нет
  sec = kika::slicer::section(m, 5.0);
  REQUIRE(sec.size() == 1);
  CHECK_THAT(loop_area(sec[0]), WithinRel(200.0, 1e-9));
  CHECK(kika::slicer::section(m, 0.0).empty());
}

TEST_CASE("Слайсер: куб 20 мм — слои, роли, объём пластика", "[slicer]") {
  auto m = box(20, 20, 20);
  m.transform(kika::geometry::place_on_bed(m, 130, 130));
  kika::slicer::Settings s;
  s.printer = *kika::slicer::find_printer("sparkx_i7");
  s.filament = kika::slicer::filament_preset("PETG");
  s.filament.flow = 1.0;  // объём пластика — ровно по геометрии
  s.infill_density = 1.0;
  const auto r = kika::slicer::slice(m, s, {}, "куб");
  CHECK(r.layers == 100);
  CHECK(r.warnings.empty());
  CHECK(r.filament_g > 0);
  CHECK(r.print_time_s > 0);
  const auto tp = kika::gcode::parse(r.gcode);
  CHECK(tp.info.slicer == "Kika");
  CHECK(tp.info.n_layers == 100);
  CHECK(tp.info.filament_type == "PETG");
  CHECK_THAT(*tp.info.layer_height, WithinAbs(0.2, 1e-12));
  CHECK(*tp.info.wall_loops == 2);
  CHECK_THAT(*tp.info.infill_density, WithinAbs(1.0, 1e-12));
  // юбка и прочистка не попали в деталь; весь куб заполнен (с перекрытием заполнения со стенками)
  const auto bb = tp.bbox();
  CHECK_THAT(bb.min[0], WithinAbs(120.21, 0.02));
  CHECK_THAT(bb.max[0], WithinAbs(139.79, 0.02));
  CHECK_THAT(bb.max[2], WithinAbs(20.0, 1e-9));
  CHECK_THAT(tp.total_volume(), WithinRel(8000.0, 0.03));
  bool outer = false, inner = false, solid = false;
  for (const auto& sg : tp.segments) {
    outer = outer || sg.role == kika::gcode::Role::OuterWall;
    inner = inner || sg.role == kika::gcode::Role::InnerWall;
    solid = solid || sg.role == kika::gcode::Role::Solid;
  }
  CHECK((outer && inner && solid));

  // 15 % сетки: разреженное заполнение есть, пластика меньше
  s.infill_density = 0.15;
  const auto tp2 = kika::gcode::parse(kika::slicer::slice(m, s).gcode);
  bool sparse = false;
  for (const auto& sg : tp2.segments) sparse = sparse || sg.role == kika::gcode::Role::Sparse;
  CHECK(sparse);
  CHECK(tp2.total_volume() < 0.6 * 8000.0);
  CHECK(tp2.total_volume() > 0.3 * 8000.0);
  CHECK(*tp2.info.infill_density == 0.15);
  CHECK(tp2.info.infill_pattern == "grid");
}

TEST_CASE("Слайсер: не помещается на стол, пластик горячее принтера", "[slicer]") {
  auto m = box(300, 20, 5);
  kika::slicer::Settings s;
  s.filament = kika::slicer::filament_preset("PC");
  s.filament.nozzle_temp = 380;  // QIDI Q2 — до 370 °C
  const auto r = kika::slicer::slice(m, s);
  // не помещается; горячее предела принтера; горячее рекомендуемого для PC; брусок от угла стола —
  // в запретной области QIDI Q2 (рез прутка)
  REQUIRE(r.warnings.size() == 4);
  CHECK(r.warnings[0].find("выше предела принтера") != std::string::npos);
  CHECK(r.warnings[1].find("вне рекомендуемой") != std::string::npos);
  CHECK(r.warnings[2].find("не помещается") != std::string::npos);
  CHECK(r.warnings[3].find("запретную область") != std::string::npos);
}

TEST_CASE("Слайсер + расчёт: консольная балка по аналитике", "[slicer][cantilever]") {
  // как prototype/tests/test_cantilever.py, но G-code — из своего слайсера
  const auto pla = *kika::material::find_material("PLA");
  const double f = 50, l = 100, b = 10, h = 10, inertia = b * h * h * h / 12;
  const double d_an = f * l * l * l / (3 * pla.E1 * inertia) + f * l / (5.0 / 6.0 * pla.G13 * b * h);
  const double sf_an = pla.Xt / (f * l * (h / 2) / inertia);

  auto m = box(100, 10, 10);
  m.transform(kika::geometry::place_on_bed(m, 130, 130));
  kika::slicer::Settings s;
  s.filament = kika::slicer::filament_preset("PLA");
  s.filament.flow = 1.0;
  s.infill_density = 1.0;
  s.infill_angle = 0;  // все нити вдоль балки
  s.alternate_solid = false;
  s.skirt = false;
  kika::analysis::ModelOptions mo;
  mo.voxel = 1.0;
  const auto model = kika::analysis::build_model(kika::gcode::parse(kika::slicer::slice(m, s).gcode), mo);
  const auto job = kika::analysis::parse_job(kika::json::parse(R"({"material": "PLA", "cases": [{"name": "изгиб",
      "fixtures": [{"where": "xmin"}], "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -50]}]}]})"));
  const auto res = kika::analysis::run_analysis(model, job);
  const auto& c = res.cases[0];
  double sf_mid = 1e300;
  for (std::size_t e = 0; e < model.vm.size(); ++e)
    if (std::abs(model.vm.center(e)[0] - model.origin[0] - 50.0) < model.vm.s) sf_mid = std::min(sf_mid, c.sf[e]);
  INFO("прогиб " << c.summary.max_disp << " аналитика " << d_an << "; запас посередине " << sf_mid << " аналитика "
                 << 2 * sf_an << "; объём " << model.tp.total_volume() << " элементов " << model.vm.size());
  // как у прототипа с его тестовым слайсером: прогиб на 2–3 % больше аналитики, запас посередине — в пределах 2 %
  CHECK(c.summary.max_disp > d_an);
  CHECK(c.summary.max_disp < d_an * 1.035);
  CHECK_THAT(sf_mid, WithinRel(2 * sf_an, 0.02));
}

TEST_CASE("Слайсер: труба — отверстие обходится стенками, объём сходится", "[slicer]") {
  // труба R = 10, r = 6, высота 10: внешняя поверхность наружу, внутренняя — внутрь трубы
  TriangleMesh m;
  const int seg = 96;
  const double rr[2] = {10.0, 6.0};
  for (int k = 0; k < 2; ++k)
    for (int z = 0; z < 2; ++z)
      for (int i = 0; i < seg; ++i) {
        const double a = 2 * std::numbers::pi * i / seg;
        m.vertices.push_back({rr[k] * std::cos(a), rr[k] * std::sin(a), 10.0 * z});
      }
  auto v = [&](int k, int z, int i) { return static_cast<std::uint32_t>((k * 2 + z) * seg + (i % seg)); };
  for (int i = 0; i < seg; ++i) {
    // наружная стенка
    m.triangles.push_back({v(0, 0, i), v(0, 0, i + 1), v(0, 1, i + 1)});
    m.triangles.push_back({v(0, 0, i), v(0, 1, i + 1), v(0, 1, i)});
    // внутренняя стенка (нормаль к оси)
    m.triangles.push_back({v(1, 0, i), v(1, 1, i + 1), v(1, 0, i + 1)});
    m.triangles.push_back({v(1, 0, i), v(1, 1, i), v(1, 1, i + 1)});
    // верх и низ — кольца
    m.triangles.push_back({v(0, 1, i), v(0, 1, i + 1), v(1, 1, i + 1)});
    m.triangles.push_back({v(0, 1, i), v(1, 1, i + 1), v(1, 1, i)});
    m.triangles.push_back({v(0, 0, i), v(1, 0, i + 1), v(0, 0, i + 1)});
    m.triangles.push_back({v(0, 0, i), v(1, 0, i), v(1, 0, i + 1)});
  }
  REQUIRE(kika::geometry::check(m).closed());
  REQUIRE(m.volume() > 0);
  const auto sec = kika::slicer::section(m, 5.0);
  REQUIRE(sec.size() == 2);
  CHECK(loop_area(sec[0]) * loop_area(sec[1]) < 0);  // контур и отверстие обходятся в разные стороны
  m.transform(kika::geometry::place_on_bed(m, 130, 130));
  kika::slicer::Settings s;
  s.filament.flow = 1.0;
  s.infill_density = 1.0;
  const auto tp = kika::gcode::parse(kika::slicer::slice(m, s).gcode);
  CHECK_THAT(tp.total_volume(), WithinRel(m.volume(), 0.04));
  // стенки и у отверстия: внутренняя стенка проходит у радиуса 6 + w/2
  double rmin = 1e9;
  for (const auto& sg : tp.segments)
    if (sg.role == kika::gcode::Role::OuterWall) rmin = std::min(rmin, std::hypot(sg.x0 - 130, sg.y0 - 130));
  CHECK_THAT(rmin, WithinAbs(6.0 + 0.21, 0.05));
}

TEST_CASE("Задание: настройки нарезки, поворот детали", "[slicer]") {
  const auto pj = kika::slicer::print_from_json(
      nullptr, "PLA");
  CHECK(pj.settings.filament.type == "PLA");
  CHECK(pj.settings.printer.key == "qidi_q2");  // принтер по умолчанию
  const auto j = kika::json::parse(R"({"filament": "PETG", "layer_height": 0.16, "walls": 3, "infill": 25,
      "pattern": "triangles", "rotate": [90, 0, 0], "skirt": false})");
  const auto p2 = kika::slicer::print_from_json(&j, "PLA");
  CHECK(p2.settings.filament.type == "PETG");
  CHECK(p2.settings.layer_height == 0.16);
  CHECK(p2.settings.first_layer_height == 0.16);
  CHECK(p2.settings.wall_loops == 3);
  CHECK(p2.settings.infill_density == 0.25);
  CHECK(p2.settings.pattern == kika::slicer::Pattern::Triangles);
  CHECK_FALSE(p2.settings.skirt);
  // обратно в JSON и снова — то же самое
  const auto j2 = kika::slicer::print_to_json(p2);
  const auto p3 = kika::slicer::print_from_json(&j2, "PLA");
  CHECK(kika::json::dump(kika::slicer::print_to_json(p3)) == kika::json::dump(j2));
  // ошибки по-русски
  const auto bad = kika::json::parse(R"({"pattern": "соты"})");
  CHECK_THROWS_AS(kika::slicer::print_from_json(&bad, "PLA"), kika::analysis::JobError);
  const auto bad2 = kika::json::parse(R"({"walls": 0})");
  CHECK_THROWS_AS(kika::slicer::print_from_json(&bad2, "PLA"), kika::analysis::JobError);

  // поворот на 90° вокруг X ставит брусок 100×10×5 «на ребро»: высота 10
  auto m = box(100, 10, 5);
  const auto on = kika::slicer::placed(m, p2);
  const auto sz = on.bbox().size();
  CHECK_THAT(sz[0], WithinAbs(100, 1e-9));
  CHECK_THAT(sz[1], WithinAbs(5, 1e-9));
  CHECK_THAT(sz[2], WithinAbs(10, 1e-9));
  CHECK_THAT(on.bbox().center()[0], WithinAbs(p2.settings.printer.bed_x / 2, 1e-9));
  CHECK(on.bbox().min[2] == 0.0);
  // накопленные повороты по 90° раскладываются в углы X, Y, Z и обратно
  using kika::geometry::Transform;
  for (int axis1 = 0; axis1 < 3; ++axis1)
    for (int axis2 = 0; axis2 < 3; ++axis2)
      for (int k = 1; k < 4; ++k) {
        const Transform r = Transform::rotation(axis2, 90.0 * k) * Transform::rotation(axis1, 90);
        const auto e = kika::slicer::euler_xyz(r);
        const Transform back = kika::slicer::rotation_of(e);
        for (std::size_t i = 0; i < 9; ++i) REQUIRE_THAT(back.r[i], WithinAbs(r.r[i], 1e-12));
      }
}

TEST_CASE("Слайсер: L-кронштейн из 3MF в разных положениях — контуры слоёв замкнуты", "[slicer]") {
  // Сетка из CAD-ядра: на гранях веера узких треугольников от одной вершины. На высоте среза
  // соседние рёбра веера дают одну и ту же точку — такие отрезки нулевой длины раньше
  // выбрасывались, и контур рвался (в положении «на боку» — 7 разрывов).
  const auto im = kika::geometry::load_model(std::filesystem::path(KIKA_SOURCE_DIR) / "examples/bracket_L.3mf");
  REQUIRE(kika::geometry::check(im.mesh).closed());
  const double volume = im.mesh.volume();
  CHECK_THAT(volume, WithinRel(14000.88, 1e-4));
  const std::array<std::array<double, 3>, 6> turns{{{0, 0, 0}, {0, 90, 0}, {90, 0, 0}, {0, -90, 0}, {90, 0, 45}, {37, 21, 13}}};
  for (const auto& rot : turns) {
    kika::slicer::PrintJob p;
    p.rotate = rot;
    p.settings.infill_density = 1.0;
    p.settings.filament.flow = 1.0;
    const auto r = kika::slicer::slice(kika::slicer::placed(im.mesh, p), p.settings, {}, "кронштейн");
    INFO("поворот " << rot[0] << ", " << rot[1] << ", " << rot[2]);
    for (const auto& w : r.warnings)
      if (w.find("не замкнута") != std::string::npos) FAIL_CHECK(w);
    // при 100 % заполнении пластика — примерно объём детали (± щели между нитями и слой у краёв)
    const double extruded = r.filament_mm * std::numbers::pi * 1.75 * 1.75 / 4;
    CHECK_THAT(extruded, WithinRel(volume, 0.06));
    // как стоит в файле — печатается без поддержек; на боку (поворот вокруг Y) стенка висит в воздухе
    auto has_overhang = [&] {
      for (const auto& w : r.warnings)
        if (w.find("Нависание") != std::string::npos) return true;
      return false;
    };
    if (rot == std::array<double, 3>{0, 0, 0}) CHECK(r.warnings.empty());
    if (rot == std::array<double, 3>{0, 90, 0}) CHECK(has_overhang());
  }
}

TEST_CASE("Слайсер: рисунки заполнения — плотность по объёму, границы, ключи как в Orca", "[slicer]") {
  // брусок 40 × 40 × 12 без верха и низа, одна стенка: всё внутри — разреженное заполнение
  auto m = box(40, 40, 12);
  m.transform(kika::geometry::place_on_bed(m, 130, 130));
  const double sp = 0.42 - 0.2 * (1 - std::numbers::pi / 4), inset = 0.21 + sp / 2;
  const double sparse_volume = (40 - 2 * inset) * (40 - 2 * inset) * 12;
  for (const auto pat : kika::slicer::patterns()) {
    const std::string key(kika::slicer::pattern_key(pat));
    INFO(key);
    REQUIRE(kika::slicer::pattern_from_key(key) == pat);
    CHECK(!kika::slicer::pattern_name(pat).empty());
    for (const double dens : {0.1, 0.25}) {
      kika::slicer::Settings s;
      s.wall_loops = 1;
      s.top_layers = 0;
      s.bottom_layers = 0;
      s.skirt = false;
      s.infill_overlap = 0;
      s.infill_density = dens;
      s.pattern = pat;
      const auto r = kika::slicer::slice(m, s);
      const auto tp = kika::gcode::parse(r.gcode);
      CHECK(tp.info.infill_pattern == key);
      double v = 0;
      for (const auto& sg : tp.segments) {
        if (sg.role != kika::gcode::Role::Sparse) continue;
        v += sg.volume;
        // заполнение не выходит за стенку
        for (const double x : {sg.x0, sg.x1}) REQUIRE((x > 110 + inset - 0.01 && x < 150 - inset + 0.01));
        for (const double y : {sg.y0, sg.y1}) REQUIRE((y > 110 + inset - 0.01 && y < 150 - inset + 0.01));
      }
      // доля объёма — та, что задана (у треугольников на 10 % — до 6 %: короткие обрезки у края)
      CHECK_THAT(v / sparse_volume, WithinRel(dens, 0.06));
      // для расчёта у рисунка есть своя вертикальная жёсткость (не значение «по умолчанию»)
      CHECK(kika::material::infill_kz(key) != kika::material::infill_kz("неизвестный"));
    }
  }
  // старые названия из заданий и G-code других слайсеров
  CHECK(kika::slicer::pattern_from_key("zig-zag") == kika::slicer::Pattern::Rectilinear);
  CHECK(kika::slicer::pattern_from_key("3D Honeycomb") == kika::slicer::Pattern::Honeycomb3D);
  CHECK(kika::slicer::pattern_from_key("trihexagon") == kika::slicer::Pattern::TriHexagon);
  CHECK(!kika::slicer::pattern_from_key("соты"));
}

namespace {

// Доля точек поддержки, под которыми на слое ниже нет ни поддержки, ни детали ближе tol.
double floating_fraction(const kika::gcode::Toolpaths& tp, double tol) {
  std::map<long long, std::vector<std::array<double, 2>>> pts;
  auto zk = [](double z) { return std::llround(z * 1000); };
  auto sample = [](const kika::gcode::Segment& s, double step, auto&& f) {
    const double l = std::hypot(s.x1 - s.x0, s.y1 - s.y0);
    const int n = std::max(1, static_cast<int>(std::ceil(l / step)));
    for (int k = 0; k <= n; ++k) f(s.x0 + (s.x1 - s.x0) * k / n, s.y0 + (s.y1 - s.y0) * k / n);
  };
  for (const auto* v : {&tp.segments, &tp.supports})
    for (const auto& s : *v) sample(s, 0.5, [&](double x, double y) { pts[zk(s.z)].push_back({x, y}); });
  long long total = 0, bad = 0;
  for (const auto& s : tp.supports) {
    const auto it = pts.find(zk(s.z));
    if (it == pts.begin()) continue;
    const auto& below = std::prev(it)->second;
    sample(s, 1.0, [&](double x, double y) {
      ++total;
      bool ok = false;
      for (const auto& p : below)
        if ((p[0] - x) * (p[0] - x) + (p[1] - y) * (p[1] - y) <= tol * tol) {
          ok = true;
          break;
        }
      bad += ok ? 0 : 1;
    });
  }
  return total ? static_cast<double>(bad) / static_cast<double>(total) : 0.0;
}

bool has_warning(const kika::slicer::Result& r, const char* part) {
  for (const auto& w : r.warnings)
    if (w.find(part) != std::string::npos) return true;
  return false;
}

}  // namespace

TEST_CASE("Слайсер: поддержки обычные и древовидные под нависанием", "[slicer][support]") {
  // уголок на боку: бывшая стенка 40 × 30 мм висит над столом на высоте 55 мм
  const auto im = kika::geometry::load_model(std::filesystem::path(KIKA_SOURCE_DIR) / "examples/bracket_L.3mf");
  kika::slicer::PrintJob p;
  p.rotate = {0, 90, 0};
  const auto mesh = kika::slicer::placed(im.mesh, p);
  kika::gcode::ParseOptions keep;
  keep.keep_supports = true;

  const auto none = kika::slicer::slice(mesh, p.settings);
  CHECK(has_warning(none, "Нависание без опоры"));
  CHECK(none.support_g == 0);
  const double part_volume = kika::gcode::parse(none.gcode).total_volume();

  double grams[2] = {0, 0};
  for (const auto kind : {kika::slicer::Support::Normal, kika::slicer::Support::Tree}) {
    INFO(kika::slicer::support_key(kind));
    p.settings.support = kind;
    const auto r = kika::slicer::slice(mesh, p.settings);
    CHECK(r.warnings.empty());
    CHECK(r.support_g > 1);
    CHECK(r.print_time_s > none.print_time_s);
    grams[kind == kika::slicer::Support::Tree] = r.support_g;
    // в расчёт поддержки не попадают: деталь та же
    const auto tp = kika::gcode::parse(r.gcode);
    CHECK(tp.supports.empty());
    CHECK_THAT(tp.total_volume(), WithinRel(part_volume, 1e-9));
    // для показа их можно сохранить; ни одна линия поддержки не висит в воздухе
    const auto tk = kika::gcode::parse(r.gcode, keep);
    REQUIRE(tk.supports.size() > 100);
    CHECK(floating_fraction(tk, 2.0) < 0.002);
    // поддержки не заходят в деталь: по высоте — до низа нависания, по X — не левее стенки + зазор
    for (const auto& s : tk.supports) REQUIRE(s.z < 55.0 - 0.2 + 1e-6);
  }
  // деревья берут меньше пластика, чем сплошная колонна
  CHECK(grams[1] < 0.7 * grams[0]);
}

TEST_CASE("Слайсер: поддержки «только от стола» не встают на деталь", "[slicer][support]") {
  // брусок 30 × 10 × 20 с туннелем 10 × 6 мм по Y: свод туннеля висит над его полом
  TriangleMesh m = box(30, 10, 5);
  m.append(box(30, 10, 9, 0, 0, 11));
  m.append(box(10, 10, 6, 0, 0, 5));
  m.append(box(10, 10, 6, 20, 0, 5));
  m.transform(kika::geometry::place_on_bed(m, 130, 130));
  kika::gcode::ParseOptions keep;
  keep.keep_supports = true;
  kika::slicer::Settings s;
  s.support = kika::slicer::Support::Normal;
  const auto r = kika::slicer::slice(m, s);
  CHECK(r.warnings.empty());
  const auto tp = kika::gcode::parse(r.gcode, keep);
  REQUIRE(!tp.supports.empty());
  // поддержка — только в туннеле: стоит на его полу (z > 5 + зазор) и ниже свода
  for (const auto& sg : tp.supports) {
    REQUIRE(sg.z > 5.0 + 0.2);
    REQUIRE(sg.z < 11.0 - 0.2 + 1e-6);
    REQUIRE((sg.x0 > 125 - 1e-6 && sg.x0 < 135 + 1e-6));
  }
  s.support_on_bed_only = true;
  const auto rb = kika::slicer::slice(m, s);
  CHECK(kika::gcode::parse(rb.gcode, keep).supports.empty());
  CHECK(has_warning(rb, "только от стола"));
}

TEST_CASE("Слайсер: поддержки в поле print задания", "[slicer][support]") {
  const auto j = kika::json::parse(R"({"support": "tree", "support_angle": 50, "support_on_bed_only": true})");
  const auto p = kika::slicer::print_from_json(&j, "PLA");
  CHECK(p.settings.support == kika::slicer::Support::Tree);
  CHECK(p.settings.support_angle == 50);
  CHECK(p.settings.support_on_bed_only);
  const auto back = kika::slicer::print_to_json(p);
  CHECK(back.find("support")->as_string() == "tree");
  const auto bad = kika::json::parse(R"({"support": "везде"})");
  CHECK_THROWS_AS(kika::slicer::print_from_json(&bad, "PLA"), kika::analysis::JobError);
  CHECK(kika::slicer::print_from_json(nullptr, "PLA").settings.support == kika::slicer::Support::None);
}
