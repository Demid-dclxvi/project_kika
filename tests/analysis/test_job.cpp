// Задание на расчёт: разбор JSON в том же формате, что у прототипа.

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/analysis/job.hpp"
#include "kika/util/json.hpp"

using Catch::Matchers::WithinRel;
using kika::analysis::JobError;
using kika::analysis::LoadType;
using kika::analysis::parse_job;
using kika::analysis::SelectionError;
using kika::json::parse;

namespace {

kika::json::Value read_json(const char* name) {
  std::ifstream f(std::filesystem::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / name, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return parse(ss.str());
}

// Текст ошибки при разборе задания (пусто — ошибки нет).
std::string job_error(const char* text) {
  try {
    parse_job(parse(text));
  } catch (const std::exception& e) {
    return e.what();
  }
  return {};
}

bool contains(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }

}  // namespace

TEST_CASE("Задание кронштейна из прототипа", "[job]") {
  const auto job = parse_job(read_json("bracket_side.job.json"));
  REQUIRE(job.material);
  CHECK(job.material->key == "PLA");
  CHECK(job.target_sf == 2.0);
  CHECK(job.part_coords);
  CHECK(job.gcode == "bracket_side.gcode");
  REQUIRE(job.cases.size() == 3);
  const auto& c0 = job.cases[0];
  CHECK(c0.name == "Лампа 2 кг");
  CHECK(c0.duration == "long");
  CHECK(c0.temperature == 30.0);
  REQUIRE(c0.fixtures.size() == 2);
  CHECK(c0.fixtures[0].name == "Саморезы");
  CHECK(c0.fixtures[0].where.size() == 2);  // два отверстия под саморезы
  CHECK(c0.fixtures[0].where[0].kind == kika::analysis::Selection::Kind::Cylinder);
  CHECK(c0.fixtures[0].where[0].axis == 0);
  CHECK(c0.fixtures[0].where[0].center2[0] == 12.0);
  CHECK(c0.fixtures[1].components == std::array<bool, 3>{true, false, false});
  REQUIRE(c0.loads.size() == 1);
  CHECK(c0.loads[0].type == LoadType::Bearing);
  CHECK(c0.loads[0].vector == kika::analysis::Vec3{0, -19.62, 0});
  const auto& c1 = job.cases[1];
  CHECK(c1.duration == "short");
  CHECK(c1.loads[0].where[0].kind == kika::analysis::Selection::Kind::Box);
  CHECK(c1.loads[0].where[0].normal == kika::analysis::Vec3{0, 1, 0});
  CHECK(job.cases[2].loads[0].where[0].kind == kika::analysis::Selection::Kind::Side);
}

TEST_CASE("Все виды нагрузок разбираются", "[job]") {
  const auto job = parse_job(parse(R"({
    "material": "PETG",
    "fixtures": [{"where": "xmin"}],
    "cases": [
      {"loads": [{"type": "force", "where": "xmax", "value": 30, "direction": "-y", "point": [1, 2, 3]}]},
      {"loads": [{"type": "mass", "where": "xmax", "kg": 1.5}]},
      {"loads": [{"type": "pressure", "where": "ymax", "mpa": 0.05}]},
      {"loads": [{"type": "torque", "where": "xmax", "axis": "+x", "value": 2000, "center": [0, 5, 5]}]},
      {"loads": [{"type": "pin", "where": {"hole": {"axis": "y", "center": [62, 15], "radius": 4}},
                  "vector": [0, -40, 0]}]},
      {"loads": [{"type": "impact", "where": "xmax", "kg": 0.3, "height": 100}]},
      {"loads": [{"type": "displacement", "where": "xmax", "vector": [null, -1, null]}]},
      {"loads": [{"type": "gravity", "g": 20}]},
      {"loads": [{"type": "Self_Weight", "g": "-y"}]}
    ]})"));
  REQUIRE(job.cases.size() == 9);
  CHECK(job.cases[0].name == "Случай 1");
  const auto& f = job.cases[0].loads[0];
  CHECK(f.type == LoadType::Force);
  CHECK(f.value == 30.0);
  CHECK(f.direction.name == "-y");
  CHECK(f.point == kika::analysis::Vec3{1, 2, 3});
  CHECK(job.cases[1].loads[0].kg == 1.5);
  CHECK(job.cases[1].loads[0].direction.name == "-z");  // по умолчанию вниз
  CHECK(job.cases[2].loads[0].value == 0.05);
  CHECK(job.cases[3].loads[0].type == LoadType::Moment);
  CHECK(job.cases[3].loads[0].axis);
  CHECK(job.cases[4].loads[0].type == LoadType::Bearing);
  CHECK(job.cases[5].loads[0].height == 100.0);
  const auto& d = job.cases[6].loads[0].displacement;
  CHECK_FALSE(d[0]);
  CHECK(d[1] == -1.0);
  CHECK(job.cases[7].loads[0].g == kika::analysis::Vec3{0, 0, -20});
  CHECK(job.cases[8].loads[0].type == LoadType::Gravity);
  CHECK(job.cases[8].loads[0].g == kika::analysis::Vec3{0, -1, 0});
  CHECK(job.cases[8].loads[0].type_name == "self_weight");
}

TEST_CASE("Свой материал на основе базового", "[job]") {
  const auto m = kika::analysis::material_from_json(parse(R"({"base": "PETG", "E1": 2500, "name": "Мой PETG"})"));
  CHECK(m.E1 == 2500.0);
  CHECK(m.name == "Мой PETG");
  CHECK(m.E2 == kika::material::find_material("PETG")->E2);
  const std::string err = [] {
    try {
      kika::analysis::material_from_json(parse(R"({"E1": 2500, "E2": 2000})"));
    } catch (const JobError& e) {
      return std::string(e.what());
    }
    return std::string();
  }();
  CHECK(contains(err, "не хватает"));
  CHECK(contains(err, "E3"));
  CHECK_THROWS_AS(kika::analysis::material_from_json(parse(R"("Дерево")")), JobError);
}

TEST_CASE("Понятные ошибки в задании", "[job]") {
  CHECK(contains(job_error(R"({"cases": []})"), "нет ни одного расчётного случая"));
  CHECK(contains(job_error(R"({"cases": [{"name": "А", "loads": [{"where": "xmax"}]}]})"),
                 "«А»: не задано ни одного закрепления"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"name": "Б"}]})"),
                 "«Б»: не задано ни одной нагрузки"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"loads": [{"type": "wind", "where": "xmax"}]}]})"),
                 "Нагрузка 1 (wind): неизвестный тип нагрузки 'wind'"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "left"}], "cases": [{"loads": [{"where": "xmax"}]}]})"),
                 "Закрепление 1: неизвестная сторона 'left'"));
  CHECK(contains(job_error(R"({"fixtures": [{}], "cases": [{"loads": [{"where": "xmax"}]}]})"),
                 "Закрепление 1: не задана область"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"loads": [{"where": "xmax", "direction": "up"}]}]})"),
                 "неизвестное направление «up»"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"loads": [{"where": "xmax", "vector": [1, 2]}]}]})"),
                 "должен содержать 3 числа"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"loads": [{"type": "displacement", "where": "xmax", "vector": [1]}]}]})"),
                 "перемещение задаётся тремя числами"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": "xmin"}], "cases": [{"loads": [
                   {"type": "impact", "where": "xmax", "kg": 1}, {"where": "xmax"}]}]})"),
                 "Удар (impact) считается отдельным случаем"));
  CHECK(contains(job_error(R"({"fixtures": [{"where": {"cube": 1}}], "cases": [{"loads": [{"where": "xmax"}]}]})"),
                 "не понимаю описание области"));
  CHECK_THROWS_AS(parse_job(parse(R"({"fixtures": [{"where": "left"}], "cases": [{"loads": [{"where": "xmax"}]}]})")),
                  SelectionError);
}
