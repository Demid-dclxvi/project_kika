// Нити детали для 3D-вида: разбиение отрезков по ячейкам, сетка треугольников, цвета, разрез, деформация.

#include <algorithm>
#include <cmath>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/app/roads.hpp"
#include "kika/app/scene.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::app::Field;
using kika::app::RoadMesh;
using kika::app::Roads;
using kika::app::Scene;

namespace {

namespace fs = std::filesystem;

kika::analysis::Model beam(double voxel) {
  kika::analysis::ModelOptions o;
  o.voxel = voxel;
  return kika::analysis::build_model(
      kika::gcode::load(fs::path(KIKA_SOURCE_DIR) / "tests" / "data" / "cantilever_flat.gcode"), o);
}

}  // namespace

TEST_CASE("Нити: отрезки делятся по ячейкам сетки без пропусков", "[app][roads]") {
  const auto m = beam(2.0);
  Scene s(m);
  Roads r(m, s);
  // в балке нет отброшенных кусков — нарисованы все отрезки
  CHECK(r.segment_count() == m.tp.segments.size());
  CHECK(r.piece_count() > r.segment_count());
  const auto& vm = m.vm;
  for (std::size_t k = 0; k < r.segment_count(); ++k) {
    const auto* b = r.pieces_begin(k);
    const auto* e = r.pieces_end(k);
    REQUIRE(b != e);
    REQUIRE(b->t0 == 0.0f);
    REQUIRE((e - 1)->t1 == 1.0f);
    for (const auto* p = b; p != e; ++p) {
      REQUIRE(p->elem >= 0);
      REQUIRE(p->t1 > p->t0);
      if (p != b) REQUIRE(p->t0 == (p - 1)->t1);
      if (p != b) REQUIRE(p->elem != (p - 1)->elem);
      // середина куска лежит в ячейке своего элемента
      const auto& sg = m.tp.segments[r.segment_index(k)];
      const double tm = 0.5 * (p->t0 + p->t1);
      const double x = sg.x0 + tm * (sg.x1 - sg.x0), y = sg.y0 + tm * (sg.y1 - sg.y0);
      const auto el = static_cast<std::size_t>(p->elem);
      REQUIRE(x >= vm.x0 + vm.ix[el] * vm.sx - 1e-9);
      REQUIRE(x <= vm.x0 + (vm.ix[el] + 1) * vm.sx + 1e-9);
      REQUIRE(y >= vm.y0 + vm.iy[el] * vm.sy - 1e-9);
      REQUIRE(y <= vm.y0 + (vm.iy[el] + 1) * vm.sy + 1e-9);
    }
  }
}

TEST_CASE("Нити: сетка треугольников, цвета полей и подсветка", "[app][roads]") {
  const auto m = beam(2.0);
  Scene s(m);
  Roads r(m, s);
  RoadMesh mesh;
  // одноцветная модель: каждый отрезок — одна лента из двух колец по 4 вершины
  s.set_field(Field::Model);
  r.build(mesh, {}, 0.0);
  CHECK(mesh.vertex_count() == 8 * r.segment_count());
  CHECK(mesh.triangle_count() == 8 * r.segment_count());
  CHECK(mesh.nrm.size() == 4 * mesh.vertex_count());
  CHECK(mesh.col.size() == 4 * mesh.vertex_count());
  for (auto i : mesh.idx) REQUIRE(i < mesh.vertex_count());
  // ленты лежат в габарите детали (с запасом на полширины валика на концах)
  const auto sz = s.size();
  for (std::size_t v = 0; v < mesh.vertex_count(); ++v)
    for (std::size_t a = 0; a < 3; ++a) {
      REQUIRE(mesh.pos[3 * v + a] >= -0.6f);
      REQUIRE(mesh.pos[3 * v + a] <= static_cast<float>(sz[a]) + 0.6f);
    }

  // структура печати: цвет по роли отрезка
  s.set_field(Field::Structure);
  r.build(mesh, {}, 0.0);
  std::size_t walls = 0;
  const auto blue = kika::app::role_color(kika::gcode::Role::OuterWall);
  for (std::size_t v = 0; v < mesh.vertex_count(); ++v)
    if (mesh.col[4 * v] == static_cast<std::uint8_t>(std::lround(blue[0] * 255))) ++walls;
  CHECK(walls > 0);
  CHECK(walls < mesh.vertex_count());

  // подсветка торца: перекрашены только нити у торца
  s.set_field(Field::Model);
  const auto end = s.side(1);
  r.build(mesh, {{end, kika::app::palette::kSelection, 1.0f}}, 0.0);
  std::size_t lit = 0;
  const auto sel = kika::app::palette::kSelection;
  for (std::size_t v = 0; v < mesh.vertex_count(); ++v)
    if (mesh.col[4 * v] == static_cast<std::uint8_t>(std::lround(sel[0] * 255)) &&
        mesh.col[4 * v + 1] == static_cast<std::uint8_t>(std::lround(sel[1] * 255))) {
      ++lit;
      REQUIRE(mesh.pos[3 * v] > static_cast<float>(sz[0]) - 2.0f - 1.0f);
    }
  CHECK(lit > 0);
}

TEST_CASE("Нити: разрез по Z показывает нижние слои, деформация сдвигает нити", "[app][roads]") {
  const auto m = beam(2.0);
  Scene s(m);
  Roads r(m, s);
  RoadMesh full, cut;
  s.set_field(Field::Model);
  r.build(full, {}, 0.0);
  const double zc = 0.5 * s.size()[2];
  s.set_section(std::pair<int, double>{2, zc});
  r.build(cut, {}, 0.0);
  CHECK(cut.vertex_count() < full.vertex_count());
  CHECK(cut.vertex_count() > 0);
  for (std::size_t v = 0; v < cut.vertex_count(); ++v) REQUIRE(cut.pos[3 * v + 2] <= static_cast<float>(zc) + 0.3f);
  s.set_section(std::nullopt);

  const auto job = kika::analysis::parse_job(kika::json::parse(R"({"material": "PLA", "cases": [{"name": "изгиб",
      "fixtures": [{"where": "xmin"}], "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -50]}]}]})"));
  const auto res = kika::analysis::run_analysis(m, job);
  s.set_results(&res);
  s.set_field(Field::SafetyFactor);
  RoadMesh plain, bent;
  r.build(plain, {}, 0.0);
  // с деформацией нить разбита по каждой ячейке
  const double k = s.auto_deform_scale();
  r.build(bent, {}, k);
  CHECK(bent.vertex_count() > plain.vertex_count());
  // самая опущенная точка опустилась на наибольший прогиб × масштаб (с точностью до ячейки)
  float zmin_plain = 1e9f, zmin_bent = 1e9f;
  for (std::size_t v = 0; v < plain.vertex_count(); ++v) zmin_plain = std::min(zmin_plain, plain.pos[3 * v + 2]);
  for (std::size_t v = 0; v < bent.vertex_count(); ++v) zmin_bent = std::min(zmin_bent, bent.pos[3 * v + 2]);
  const double drop = zmin_plain - zmin_bent;
  CHECK_THAT(drop, WithinRel(k * res.cases[0].summary.max_disp, 0.15));
}
