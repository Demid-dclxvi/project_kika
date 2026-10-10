// Логика окна без Qt: сцена, выбор граней, задание из окна.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/app/job_model.hpp"
#include "kika/app/scene.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::app::Scene;

namespace {

namespace fs = std::filesystem;

kika::analysis::Model beam(double voxel) {
  kika::analysis::ModelOptions o;
  o.voxel = voxel;
  return kika::analysis::build_model(kika::gcode::load(fs::path(KIKA_SOURCE_DIR) / "tests" / "data" / "cantilever_flat.gcode"), o);
}

kika::analysis::Model bracket(int max_elems) {
  kika::analysis::ModelOptions o;
  o.max_elems = max_elems;
  return kika::analysis::build_model(
      kika::gcode::load(fs::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / "bracket_side.gcode"), o);
}

std::vector<std::int64_t> select(const kika::analysis::Model& m, const char* where) {
  const auto region = kika::analysis::region_from_json(kika::json::parse(where), "тест");
  const auto idx = kika::analysis::select_faces(m.mesh, region, m.origin, "тест");
  std::vector<std::int64_t> keys;
  for (auto i : idx) keys.push_back(m.mesh.face_key[static_cast<std::size_t>(i)]);
  std::sort(keys.begin(), keys.end());
  return keys;
}

double jaccard(const std::vector<std::int64_t>& a, const std::vector<std::int64_t>& b) {
  std::vector<std::int64_t> i, u;
  std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(i));
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(u));
  return u.empty() ? 1.0 : static_cast<double>(i.size()) / static_cast<double>(u.size());
}

}  // namespace

TEST_CASE("Сцена: видимые грани и разрез", "[app]") {
  const auto m = beam(2.0);
  Scene s(m);
  CHECK(s.faces().size() == m.mesh.n_faces());
  for (const auto& f : s.faces()) REQUIRE_FALSE(f.cut);
  const auto key = s.key(5, 3);
  const auto back = s.face_of(key);
  REQUIRE(back);
  CHECK(back->first == 5);
  CHECK(back->second == 3);
  CHECK_FALSE(s.face_of(-1));

  s.set_section(std::pair{0, 50.0});
  std::size_t cut = 0;
  for (const auto& f : s.faces()) {
    REQUIRE(s.elem_center(static_cast<std::size_t>(f.elem))[0] <= 50.0);
    cut += f.cut;
  }
  CHECK(cut > 0);
  // грани разреза смотрят в +x
  for (const auto& f : s.faces())
    if (f.cut) REQUIRE(f.dir == 1);
  s.set_section(std::nullopt);
  CHECK(s.faces().size() == m.mesh.n_faces());
}

TEST_CASE("Сцена: выбор плоской грани, стороны, кисти; площадь", "[app]") {
  const auto m = beam(2.0);
  Scene s(m);
  const auto side = s.side(1);  // торец +x
  REQUIRE(!side.empty());
  const auto f0 = s.face_of(side.front());
  REQUIRE(f0);
  const auto plane = s.flood_plane(f0->first, f0->second);
  CHECK(plane == side);
  CHECK(plane == select(m, R"({"side": "xmax"})"));
  // торец балки 10 × 10 мм (сечение по наружной кромке валиков)
  CHECK_THAT(s.area(plane), WithinRel(m.size[1] * m.size[2], 0.25));

  // кисть: все центры выбранных граней в радиусе
  const kika::app::Vec3 p{50, 5, 10};
  const auto br = s.brush(p, 4.0);
  REQUIRE(!br.empty());
  const auto pts = s.to_points(br);
  for (const auto& q : pts) {
    const double d = std::hypot(q.p[0] - p[0], q.p[1] - p[1], q.p[2] - p[2]);
    REQUIRE(d <= 4.0 + 1e-9);
  }
}

TEST_CASE("Сцена: отверстие выбирается целиком", "[app]") {
  const auto m = bracket(12000);
  Scene s(m);
  // грань внутри отверстия под саморез (ось X, центр [12, 15], радиус 2,5)
  const auto hole = select(m, R"({"hole": {"axis": "x", "center": [12, 15], "radius": 2.5}})");
  REQUIRE(hole.size() > 8);
  // берём грань стенки отверстия, смотрящую по Y или Z
  std::optional<std::pair<std::int32_t, int>> start;
  for (auto k : hole) {
    const auto f = s.face_of(k);
    if (f && (f->second >> 1) != 0) {
      start = f;
      break;
    }
  }
  REQUIRE(start);
  const auto region = s.hole_region(start->first, start->second);
  for (auto k : region) REQUIRE((k % 6) / 2 != 0);  // нет граней, смотрящих вдоль оси отверстия
  CHECK(jaccard(region, hole) > 0.6);
}

TEST_CASE("Сцена: луч из камеры попадает в верхнюю грань", "[app]") {
  const auto m = beam(2.0);
  Scene s(m);
  const auto sz = s.size();
  const auto hit = s.pick({50, sz[1] / 2, 100}, {0, 0, -1}, 0.0);
  REQUIRE(hit);
  CHECK(hit->dir == 5);
  CHECK_THAT(hit->point[2], WithinAbs(sz[2], 1e-6));
  CHECK_FALSE(s.pick({50, sz[1] / 2, 100}, {0, 0, 1}, 0.0));  // вверх — мимо детали
}

TEST_CASE("Сцена: цвета полей и легенда", "[app]") {
  const auto m = beam(2.0);
  Scene s(m);
  s.set_field(kika::app::Field::Structure);
  CHECK(s.legend().kind == kika::app::Legend::Kind::Categories);
  CHECK(s.legend().entries.size() == 3);

  const auto job = kika::analysis::parse_job(kika::json::parse(R"({"material": "PLA", "cases": [{"name": "изгиб",
      "fixtures": [{"where": "xmin"}], "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -50]}]}]})"));
  const auto res = kika::analysis::run_analysis(m, job);
  s.set_results(&res);
  s.set_field(kika::app::Field::SafetyFactor);
  const auto lg = s.legend();
  REQUIRE(lg.kind == kika::app::Legend::Kind::Steps);
  REQUIRE(lg.entries.size() == 6);
  CHECK(lg.entries[0].label == "< 1");
  CHECK(lg.entries[2].label == "1,5–2,0");
  CHECK(lg.entries[5].label == "> 8,0");
  s.set_field(kika::app::Field::Stress);
  CHECK(s.legend().kind == kika::app::Legend::Kind::Ramp);
  CHECK(s.legend().max > 0);
  CHECK_THAT(s.auto_deform_scale() * res.cases[0].summary.max_disp, WithinRel(0.08 * 100, 0.05));

  // подсветка выбора меняет цвет только выбранных граней
  s.set_field(kika::app::Field::Model);
  const auto sel = s.side(1);
  const auto plain = s.face_colors({});
  const auto lit = s.face_colors({{sel, kika::app::palette::kSelection, 0.9f}});
  std::size_t changed = 0;
  for (std::size_t f = 0; f < plain.size(); ++f)
    if (plain[f] != lit[f]) {
      ++changed;
      REQUIRE(std::binary_search(sel.begin(), sel.end(), s.key(static_cast<std::size_t>(s.faces()[f].elem), s.faces()[f].dir)));
    }
  CHECK(changed == sel.size());
  CHECK(s.describe(0).size() == 2);
}

TEST_CASE("Выбор переносится на сетку другой детальности", "[app]") {
  const auto m1 = beam(2.0);
  const auto m2 = beam(1.25);
  Scene s1(m1), s2(m2);
  const auto end = s1.side(1);
  CHECK(s1.from_points(s1.to_points(end)) == end);
  const auto moved = s2.from_points(s1.to_points(end));
  REQUIRE(!moved.empty());
  for (auto k : moved) REQUIRE(k % 6 == 1);
}

TEST_CASE("Поворот детали на столе: закрепления и нагрузки поворачиваются вместе с ней", "[app]") {
  // балка плашмя (длина по X) и та же балка стоя (длина по Z): поворот вокруг Y на −90°, X → Z
  kika::analysis::ModelOptions o;
  o.voxel = 2.0;
  const auto flat = beam(2.0);
  const auto up = kika::analysis::build_model(
      kika::gcode::load(fs::path(KIKA_SOURCE_DIR) / "tests" / "data" / "cantilever_upright.gcode"), o);
  Scene s1(flat), s2(up);
  auto job = kika::app::new_job();
  kika::app::UiFixture fx;
  fx.id = job.new_id();
  fx.faces = s1.side(0);  // торец −X
  fx.components = "xz";
  job.fixtures.push_back(fx);
  auto ld = kika::app::default_load("force", job.new_id());
  ld.faces = s1.side(1);  // торец +X
  ld.dir = "-z";
  job.cases[0].loads.push_back(ld);
  auto mv = kika::app::default_load("displacement", job.new_id());
  mv.faces = s1.side(1);
  mv.disp = {0.5, std::nullopt, -1.0};
  job.cases[0].loads.push_back(mv);
  auto mo = kika::app::default_load("moment", job.new_id());
  mo.faces = s1.side(1);
  mo.axis = "custom";
  mo.vec = {0, 1, 1};
  job.cases[0].loads.push_back(mo);

  const std::array<double, 9> ry{0, 0, -1, 0, 1, 0, 1, 0, 0};  // x' = −z, z' = x
  kika::app::rotate_job(job, s1, s2, ry);
  CHECK(kika::app::can_run(job));
  CHECK(jaccard(job.fixtures[0].faces, s2.side(4)) > 0.95);  // торец −Z: стоит на нём
  CHECK(job.fixtures[0].components == "xz");                 // x → z, z → x
  const auto& loads = job.cases[0].loads;
  CHECK(jaccard(loads[0].faces, s2.side(5)) > 0.95);  // верхний торец
  CHECK(loads[0].dir == "+x");                         // было −Z: x' = −z = +1
  // перемещение 0,5 по X → по Z; −1 по Z → +1 по X (z переходит в −x); Y свободно
  REQUIRE(loads[1].disp[0].has_value());
  REQUIRE(loads[1].disp[2].has_value());
  CHECK(*loads[1].disp[0] == 1.0);
  CHECK(!loads[1].disp[1].has_value());
  CHECK(*loads[1].disp[2] == 0.5);
  CHECK(loads[2].axis == "custom");
  CHECK_THAT(loads[2].vec[0], WithinAbs(-1, 1e-12));
  CHECK_THAT(loads[2].vec[1], WithinAbs(1, 1e-12));
  CHECK_THAT(loads[2].vec[2], WithinAbs(0, 1e-12));
}

TEST_CASE("Задание из окна → JSON → тот же расчёт, что по геометрическому заданию", "[app]") {
  const auto m = beam(2.0);
  Scene s(m);
  auto job = kika::app::new_job();
  REQUIRE(job.cases.size() == 1);
  CHECK_FALSE(kika::app::can_run(job));
  kika::app::UiFixture fx;
  fx.id = job.new_id();
  fx.faces = s.side(0);
  job.fixtures.push_back(fx);
  auto ld = kika::app::default_load("force", job.new_id());
  ld.faces = s.side(1);
  ld.value = 50;
  ld.dir = "-z";
  job.cases[0].loads.push_back(ld);
  CHECK(kika::app::can_run(job));
  const auto arrow = kika::app::load_arrow(job.cases[0].loads[0], s);
  REQUIRE(arrow);
  CHECK((*arrow)[2] == -1.0);

  const auto j = kika::app::to_job_json(job, "Балка", "cantilever_flat.gcode", 1234);
  CHECK(j.find("max_elems")->as_int() == 1234);
  CHECK(j.find("cases")->at(0).find("loads")->at(0).find("direction")->as_string() == "-z");
  const auto r1 = kika::analysis::run_analysis(m, kika::analysis::parse_job(j));
  const auto r2 = kika::analysis::run_analysis(
      m, kika::analysis::parse_job(kika::json::parse(R"({"material": "PLA", "cases": [{"name": "Основная нагрузка",
         "fixtures": [{"where": "xmin"}], "loads": [{"type": "force", "where": "xmax", "value": 50, "direction": "-z"}]}]})")));
  CHECK_THAT(r1.cases[0].summary.sf, WithinRel(r2.cases[0].summary.sf, 1e-9));
  CHECK_THAT(r1.cases[0].summary.max_disp, WithinRel(r2.cases[0].summary.max_disp, 1e-9));
}

TEST_CASE("Задание прототипа открывается в окне", "[app]") {
  const auto m = bracket(12000);
  std::ifstream f(fs::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / "bracket_side.job.json", std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  std::vector<std::string> warnings;
  const auto job = kika::app::from_job_json(kika::json::parse(ss.str()), m, &warnings);
  CHECK(warnings.empty());
  CHECK(job.material == "PLA");
  REQUIRE(job.fixtures.size() == 2);
  CHECK(job.fixtures[0].name == "Саморезы");
  CHECK(job.fixtures[0].faces ==
        select(m, R"([{"hole": {"axis": "x", "center": [12, 15], "radius": 2.5}},
                      {"hole": {"axis": "x", "center": [28, 15], "radius": 2.5}}])"));
  CHECK(job.fixtures[1].components == "x");
  REQUIRE(job.cases.size() == 3);
  CHECK(job.cases[0].duration == "long");
  CHECK(job.cases[0].temperature == 30.0);
  REQUIRE(job.cases[0].loads.size() == 1);
  const auto& b = job.cases[0].loads[0];
  CHECK(b.type == "bearing");
  CHECK(b.dir == "-y");
  CHECK_THAT(b.value, WithinRel(19.62, 1e-12));
  CHECK(job.cases[2].loads[0].faces == select(m, R"({"side": "xmax"})"));

  // и обратно: задание из окна считается так же, как исходное
  const auto res_ui = kika::analysis::run_analysis(m, kika::analysis::parse_job(kika::app::to_job_json(job, "т", "", 0)));
  const auto res_file = kika::analysis::run_analysis(m, kika::analysis::parse_job(kika::json::parse(ss.str())));
  REQUIRE(res_ui.cases.size() == 3);
  for (std::size_t c = 0; c < 3; ++c)
    CHECK_THAT(res_ui.cases[c].summary.sf, WithinRel(res_file.cases[c].summary.sf, 1e-9));
}

TEST_CASE("Числа для подписей", "[app]") {
  CHECK(kika::app::fmt(1234.5) == "1234");
  CHECK(kika::app::fmt(12.34) == "12,3");
  CHECK(kika::app::fmt(1.234) == "1,23");
  CHECK(kika::app::fmt(0.1234) == "0,123");
  CHECK(kika::app::fmt(-0.0001, 1) == "0,0");
  CHECK(kika::app::fmt(2.0, 1) == "2,0");
}
