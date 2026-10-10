// Свой слайсер: сечения, G-code, его разбор и расчёт по нему.

#include <cmath>
#include <filesystem>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/geometry/mesh.hpp"
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
  s.filament.nozzle_temp = 320;
  const auto r = kika::slicer::slice(m, s);
  CHECK(r.warnings.size() == 2);
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
  CHECK(pj.settings.printer.key == "sparkx_i7");
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
  CHECK_THAT(on.bbox().center()[0], WithinAbs(130, 1e-9));
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
