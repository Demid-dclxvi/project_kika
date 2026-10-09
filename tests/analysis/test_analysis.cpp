// Расчёт целиком: нагрузки, запас прочности, сверка с прототипом и с аналитикой.

#include <cmath>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::analysis::Vec3;

namespace {

namespace fs = std::filesystem;

kika::analysis::Model model_of(const fs::path& gcode, std::optional<double> voxel, int max_elems = 120000) {
  kika::analysis::ModelOptions o;
  o.voxel = voxel;
  o.max_elems = max_elems;
  return kika::analysis::build_model(kika::gcode::load(gcode), o);
}

fs::path example(const char* name) { return fs::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / name; }
fs::path data(const char* name) { return fs::path(KIKA_SOURCE_DIR) / "tests" / "data" / name; }

Vec3 total_force(const std::vector<double>& f) {
  Vec3 s{0, 0, 0};
  for (std::size_t i = 0; i < f.size(); ++i) s[i % 3] += f[i];
  return s;
}

}  // namespace

TEST_CASE("Нагрузки распределяются без потери силы и момента", "[analysis][loads]") {
  const auto model = model_of(example("bracket_side.gcode"), std::nullopt, 3000);
  const auto& mesh = model.mesh;
  const auto n3 = static_cast<std::size_t>(3 * mesh.n_nodes);
  const Vec3 origin = model.origin;
  using kika::analysis::Selection;

  Selection xmax;
  xmax.kind = Selection::Kind::Side;
  xmax.axis = 0;
  xmax.sign = 1;
  const auto faces = kika::analysis::select_faces(mesh, {xmax}, origin, "тест");
  REQUIRE(!faces.empty());
  for (auto f : faces) REQUIRE(mesh.face_dir[static_cast<std::size_t>(f)] == 1);

  SECTION("сила") {
    std::vector<double> f(n3, 0.0);
    kika::analysis::distribute_force(mesh, faces, {1.0, -2.0, 3.0}, f);
    const auto s = total_force(f);
    CHECK_THAT(s[0], WithinRel(1.0, 1e-12));
    CHECK_THAT(s[1], WithinRel(-2.0, 1e-12));
    CHECK_THAT(s[2], WithinRel(3.0, 1e-12));
  }
  SECTION("момент относительно центра граней: сумма сил ноль, момент — заданный") {
    std::vector<double> f(n3, 0.0);
    const Vec3 m{1000.0, -200.0, 50.0};
    kika::analysis::distribute_moment(mesh, faces, m, f);
    const auto nw = kika::analysis::face_node_weights(mesh, faces);
    Vec3 c{0, 0, 0};
    double ws = 0;
    for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
      for (std::size_t a = 0; a < 3; ++a) c[a] += mesh.xyz[static_cast<std::size_t>(nw.nodes[i])][a] * nw.w[i];
      ws += nw.w[i];
    }
    for (auto& v : c) v /= ws;
    const auto s = total_force(f);
    for (double v : s) CHECK_THAT(v, WithinAbs(0.0, 1e-9));
    Vec3 mm{0, 0, 0};
    for (std::size_t n = 0; n < mesh.xyz.size(); ++n) {
      const Vec3 r{mesh.xyz[n][0] - c[0], mesh.xyz[n][1] - c[1], mesh.xyz[n][2] - c[2]};
      mm[0] += r[1] * f[3 * n + 2] - r[2] * f[3 * n + 1];
      mm[1] += r[2] * f[3 * n] - r[0] * f[3 * n + 2];
      mm[2] += r[0] * f[3 * n + 1] - r[1] * f[3 * n];
    }
    for (std::size_t a = 0; a < 3; ++a) CHECK_THAT(mm[a], WithinAbs(m[a], 1e-9 * 1000));
  }
  SECTION("давление") {
    std::vector<double> f(n3, 0.0);
    kika::analysis::distribute_pressure(mesh, faces, 0.1, f);
    double area = 0;
    for (auto fc : faces) area += mesh.face_area[static_cast<std::size_t>(fc)];
    const auto s = total_force(f);
    CHECK_THAT(s[0], WithinRel(-0.1 * area, 1e-12));
    CHECK_THAT(s[1], WithinAbs(0.0, 1e-12));
  }
  SECTION("болт: давит только на грани против силы") {
    std::vector<double> f(n3, 0.0);
    kika::analysis::distribute_bearing(mesh, faces, {-40.0, 0.0, 0.0}, f);
    const auto s = total_force(f);
    CHECK_THAT(s[0], WithinRel(-40.0, 1e-12));
  }
  SECTION("пустая область — ошибка с названием") {
    Selection far;
    far.kind = Selection::Kind::Sphere;
    far.center = {1000, 1000, 1000};
    far.radius = 1.0;
    CHECK_THROWS_AS(kika::analysis::select_faces(mesh, {far}, origin, "Нагрузка 7"), kika::analysis::SelectionError);
  }
}

TEST_CASE("Все виды нагрузок совпадают с прототипом", "[analysis][reference][slow]") {
  // задание из prototype/tests/test_all_loads.py
  const auto job = kika::analysis::parse_job(kika::json::parse(R"({
    "material": "PETG", "target_sf": 2.0,
    "fixtures": [
      {"where": [{"hole": {"axis": "x", "center": [12.0, 15.0], "radius": 2.5}},
                 {"hole": {"axis": "x", "center": [28.0, 15.0], "radius": 2.5}}]},
      {"where": {"side": "xmin"}, "components": "x"}],
    "cases": [
      {"name": "Сила 30 Н", "loads": [{"type": "force",
        "where": {"box": {"min": [55, 40, 0], "max": [70, 50, 30]}, "normal": "+y"}, "vector": [0, -30, 0]}]},
      {"name": "Сила по величине и направлению", "loads": [{"type": "force",
        "where": {"box": {"min": [55, 40, 0], "max": [70, 50, 30]}, "normal": "+y"}, "value": 30, "direction": "-y"}]},
      {"name": "Груз 1 кг на вынесенной точке", "loads": [{"type": "mass",
        "where": {"hole": {"axis": "y", "center": [62.0, 15.0], "radius": 4.0}}, "kg": 1, "direction": "-y",
        "point": [62, 20, 15]}]},
      {"name": "Давление 0,05 МПа", "loads": [{"type": "pressure", "where": {"side": "ymax"}, "value": 0.05}]},
      {"name": "Кручение 2 Н·м", "loads": [{"type": "moment", "where": {"side": "xmax"}, "axis": "+x", "value": 2000}]},
      {"name": "Болт 40 Н", "loads": [{"type": "bearing",
        "where": {"hole": {"axis": "y", "center": [62.0, 15.0], "radius": 4.0}}, "vector": [0, -40, 0]}]},
      {"name": "Удар 0,3 кг с 100 мм", "loads": [{"type": "impact",
        "where": {"box": {"min": [55, 40, 0], "max": [70, 50, 30]}, "normal": "+y"}, "kg": 0.3, "height": 100,
        "direction": "-y"}]},
      {"name": "Прогиб конца 1 мм", "loads": [{"type": "displacement", "where": {"side": "xmax"},
        "vector": [null, -1.0, null]}]},
      {"name": "Перегрузка 20 g", "loads": [{"type": "gravity", "g": [0, -20, 0]}]},
      {"name": "Вибрация 10 Н, 10^6 циклов", "duration": "cyclic", "cycles": 1000000.0, "loads": [{"type": "force",
        "where": {"box": {"min": [55, 40, 0], "max": [70, 50, 30]}, "normal": "+y"}, "vector": [0, -10, 0]}]},
      {"name": "Нагрев 60 °C, длительно, зажат", "duration": "long", "temperature": 60, "thermal_expansion": true,
       "fixtures": [
         {"where": [{"hole": {"axis": "x", "center": [12.0, 15.0], "radius": 2.5}},
                    {"hole": {"axis": "x", "center": [28.0, 15.0], "radius": 2.5}}]},
         {"where": {"side": "xmin"}}, {"where": {"side": "xmax"}}],
       "loads": [{"type": "gravity", "g": [0, -1, 0]}]}
    ]})"));
  const auto model = model_of(example("bracket_side.gcode"), std::nullopt, 25000);
  REQUIRE(model.vm.size() == 22330);
  REQUIRE(model.mesh.n_nodes == 26928);
  const auto res = kika::analysis::run_analysis(model, job);
  REQUIRE(res.cases.size() == 11);

  struct Ref {
    double sf, max_disp;
    const char* mode;
    double reaction_y;
    const char* limit;
    std::size_t warnings;
  };
  // значения прототипа (python prototype/tests/test_all_loads.py, max_elems = 25000)
  const Ref refs[] = {
      {4.2586005761209655, 1.5628462915912527, "Смятие вдоль нити", 30.0, "≈ 128 Н", 0},
      {4.2586005761209655, 1.5628462915912527, "Смятие вдоль нити", 30.0, "≈ 128 Н", 0},
      {13.106225991525845, 0.5081031362154814, "Смятие вдоль нити", 9.81, "≈ 13,1 кг", 0},
      {2.4281693217711795, 2.3065750270561285, "Смятие вдоль нити", 102.908, "≈ 0,121 МПа", 0},
      {2.0301630614539916, 1.343704496602389, "Расслоение между слоями (отрыв по Z)", 0.0, "≈ 4 060 Н·мм", 0},
      {3.202305518894672, 2.0832008100443775, "Смятие вдоль нити", 40.0, "≈ 128 Н", 0},
      {1.0746808543402788, 6.193036835893834, "Смятие вдоль нити", 118.88, "падение с ≈ 116 мм", 2},
      {6.974448271574463, 1.0042268112883364, "Смятие вдоль нити", 16.15, "≈ 6,97 мм", 0},
      {123.12684716396588, 0.045659501882559864, "Смятие вдоль нити", 2.909, "≈ 2 463 g", 0},
      {3.209136301312013, 0.5209487638639674, "Смятие вдоль нити", 10.0, "≈ 32,1 Н", 0},
      {3.3133113120229503, 0.05650842792769608, "Смятие вдоль нити", 0.145, nullptr, 0},
  };
  for (std::size_t k = 0; k < 11; ++k) {
    const auto& s = res.cases[k].summary;
    INFO(s.name);
    // итерационный решатель сходится до невязки 1e-7, отсюда допуск
    CHECK_THAT(s.sf, WithinRel(refs[k].sf, 1e-5));
    CHECK_THAT(s.max_disp, WithinRel(refs[k].max_disp, 1e-5));
    CHECK(s.mode == refs[k].mode);
    CHECK_THAT(s.reaction[1], WithinAbs(refs[k].reaction_y, 1.5e-3));
    CHECK_THAT(s.reaction[1] + s.applied[1], WithinAbs(0.0, 1.5e-3));
    if (refs[k].limit) {
      REQUIRE(s.limit);
      CHECK(s.limit->short_text == refs[k].limit);
    } else {
      CHECK_FALSE(s.limit);
    }
    CHECK(s.warnings.size() == refs[k].warnings);
    CHECK(s.verdict == (k == 6 ? "risk" : "ok"));
  }
  CHECK(res.cases[6].summary.impact_factor == 40.394);
  CHECK(res.cases[9].summary.cycles == 1e6);
  // проверки согласованности из прототипа
  CHECK_THAT(res.cases[0].summary.sf, WithinRel(res.cases[1].summary.sf, 1e-12));
  const double weight = 20 * 9.81e-3 * res.material.density * model.vm.stats.model_volume / 1000;
  CHECK_THAT(res.cases[8].summary.applied[1], WithinAbs(-weight, 0.05));
}

TEST_CASE("Консольная балка: прогиб и запас прочности по аналитике", "[analysis][cantilever]") {
  // как prototype/tests/test_cantilever.py: балка 100×10×10 мм, PLA, сила 50 Н на конце
  const auto pla = *kika::material::find_material("PLA");
  auto analytic = [](double e, double g) {
    const double f = 50, l = 100, b = 10, h = 10, inertia = b * h * h * h / 12;
    const double d = f * l * l * l / (3 * e * inertia) + f * l / (5.0 / 6.0 * g * b * h);
    const double sig = f * l * (h / 2) / inertia;
    return std::pair{d, sig};
  };
  struct Ref {
    bool upright;
    double voxel;
    double disp, sf_mid, sf;  // значения прототипа
    const char* mode;
  };
  const Ref refs[] = {
      {false, 1.0, 6.662035936279543, 3.7669222354888916, 1.804180452178538, "Разрыв вдоль нити"},
      {false, 0.7, 6.661742593778683, 3.7099156379699707, 1.4263083513117534, "Отрыв соседних нитей (в слое)"},
      {true, 1.0, 9.418584453355088, 2.0108678340911865, 1.0290904867886428, "Расслоение между слоями (отрыв по Z)"},
      {true, 0.7, 9.434484155455248, 2.0028655529022217, 1.0032775001775283, "Расслоение между слоями (отрыв по Z)"},
  };
  for (const auto& r : refs) {
    INFO((r.upright ? "стоя, воксель " : "плашмя, воксель ") << r.voxel);
    const auto model = model_of(data(r.upright ? "cantilever_upright.gcode" : "cantilever_flat.gcode"), r.voxel);
    const char* job_text =
        r.upright ? R"({"material": "PLA", "cases": [{"name": "изгиб", "fixtures": [{"where": "zmin"}],
                       "loads": [{"type": "force", "where": "zmax", "vector": [50, 0, 0]}]}]})"
                  : R"({"material": "PLA", "cases": [{"name": "изгиб", "fixtures": [{"where": "xmin"}],
                       "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -50]}]}]})";
    const auto res = kika::analysis::run_analysis(model, kika::analysis::parse_job(kika::json::parse(job_text)));
    const auto& c = res.cases.at(0);
    const auto [d_an, s_an] = r.upright ? analytic(pla.E3, pla.G13) : analytic(pla.E1, pla.G13);
    const double sf_an = (r.upright ? pla.Zt : pla.Xt) / s_an;
    // запас посередине пролёта: там момент F·L/2 и нет влияния заделки
    const std::size_t ax = r.upright ? 2 : 0;
    double sf_mid = 1e300;
    for (std::size_t e = 0; e < model.vm.size(); ++e)
      if (std::abs(model.vm.center(e)[ax] - model.origin[ax] - 50.0) < model.vm.s) sf_mid = std::min(sf_mid, c.sf[e]);

    // аналитика: прогиб на 2–3 % больше (податливость заделки), запас посередине — в пределах 1 %
    CHECK(c.summary.max_disp > d_an);
    CHECK(c.summary.max_disp < d_an * 1.035);
    CHECK_THAT(sf_mid, WithinRel(2 * sf_an, 0.01));
    // прототип (его поля хранятся во float32 — отсюда допуск)
    CHECK_THAT(c.summary.max_disp, WithinRel(r.disp, 1e-6));
    CHECK_THAT(sf_mid, WithinRel(r.sf_mid, 1e-6));
    CHECK_THAT(c.summary.sf, WithinRel(r.sf, 1e-6));
    CHECK(c.summary.mode == r.mode);
    CHECK_THAT(std::abs(r.upright ? c.summary.reaction[0] : c.summary.reaction[2]), WithinAbs(50.0, 1e-3));
  }
}

TEST_CASE("Текст итогов и JSON — как у прототипа", "[analysis]") {
  const auto model = model_of(data("cantilever_flat.gcode"), 2.0);
  const auto job = kika::analysis::parse_job(kika::json::parse(R"({"material": "PLA", "target_sf": 1.5,
      "cases": [{"name": "изгиб", "fixtures": [{"where": "xmin"}],
                 "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -5]}]}]})"));
  const auto res = kika::analysis::run_analysis(model, job);
  const std::string text = kika::analysis::text_summary(res);
  CHECK(text.rfind("Материал: PLA", 0) == 0);
  CHECK(text.find("требуемый запас: 1.5") != std::string::npos);
  CHECK(text.find("▶ изгиб  [кратковременная]") != std::string::npos);
  CHECK(text.find("Итог: ВЫДЕРЖИТ") != std::string::npos);
  CHECK(text.find("Предельная сила ≈ ") != std::string::npos);
  const auto j = kika::analysis::to_json(res.cases[0].summary);
  const auto& o = j.as_object();
  REQUIRE(o.size() == 29);
  CHECK(o.front().first == "name");
  CHECK(o.back().first == "fail_volume_frac");
  CHECK(j.find("verdict")->as_string() == "ok");
  CHECK(j.find("loads")->at(0).find("type")->as_string() == "force");
  CHECK(j.find("loads")->at(0).find("unit")->as_string() == "Н");
  const auto ms = kika::analysis::model_summary(model);
  CHECK(ms.find("slicer")->as_string() == "OrcaSlicer");
  CHECK(ms.find("elements")->as_int() == static_cast<long long>(model.vm.size()));
}

TEST_CASE("Округление как в Python", "[analysis]") {
  using kika::analysis::py_round;
  CHECK(py_round(2.675, 2) == 2.67);  // 2.675 в двоичном виде чуть меньше
  CHECK(py_round(0.125, 2) == 0.12);  // ровно половина — к чётному
  CHECK(py_round(0.375, 2) == 0.38);
  CHECK(py_round(-1.5, 0) == -2.0);
  CHECK(py_round(12.5, 0) == 12.0);
  CHECK(py_round(1e20, 2) == 1e20);
}
