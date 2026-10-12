// Подбор положения и настроек печати: две цели из трёх.

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/advisor/advisor.hpp"
#include "kika/analysis/job.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/geometry/import.hpp"
#include "kika/util/json.hpp"

namespace {

namespace fs = std::filesystem;

std::string read(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Уголок из примеров: задание, текущая нарезка на грубой сетке, сцена.
struct Bracket {
  kika::geometry::ImportedModel im;
  kika::json::Value job_json;
  kika::slicer::PrintJob current;
  kika::analysis::Model model;
  std::unique_ptr<kika::app::Scene> scene;
  kika::app::UiJob job;

  explicit Bracket(int max_elems) {
    const fs::path dir = fs::path(KIKA_SOURCE_DIR) / "examples";
    im = kika::geometry::load_model(dir / "bracket_L.3mf");
    job_json = kika::json::parse(read(dir / "bracket_L.job.json"));
    current = kika::slicer::print_from_json(job_json.find("print"), "PETG");
    kika::analysis::ModelOptions mo;
    mo.max_elems = max_elems;
    model = kika::analysis::build_model(
        kika::gcode::parse(kika::slicer::slice(kika::slicer::placed(im.mesh, current), current.settings).gcode), mo);
    scene = std::make_unique<kika::app::Scene>(model);
    job = kika::app::from_job_json(job_json, model);
  }

  kika::advisor::Request request(const kika::advisor::Goals& g, int max_elems) const {
    kika::advisor::Request rq;
    rq.mesh = &im.mesh;
    rq.current = current;
    rq.model = &model;
    rq.scene = scene.get();
    rq.job = job;
    rq.goals = g;
    rq.max_elems = max_elems;
    return rq;
  }
};

}  // namespace

TEST_CASE("Подбор: цели — ровно две из трёх", "[advisor]") {
  using kika::advisor::goals_from_key;
  REQUIRE(goals_from_key("speed,strength"));
  CHECK(goals_from_key("speed,strength")->economy == false);
  CHECK(goals_from_key("экономия,прочность")->strength);
  CHECK(!goals_from_key("speed"));
  CHECK(!goals_from_key("speed,economy,strength"));
  CHECK(!goals_from_key("speed,быстро"));
  CHECK(kika::advisor::goals_key(*goals_from_key("strength,economy")) == "economy,strength");
  // шесть положений «какой гранью вниз», без повторов
  const auto o = kika::advisor::orientations({0, 0, 0});
  REQUIRE(o.size() == 6);
  CHECK(o[0] == std::array<double, 3>{0, 0, 0});
  for (std::size_t a = 0; a < o.size(); ++a)
    for (std::size_t b = a + 1; b < o.size(); ++b) CHECK(o[a] != o[b]);
}

TEST_CASE("Подбор: уголок — прочно и экономно кладёт его профилем на стол", "[advisor][slow]") {
  // как стоит в файле, толчок 55 Н отрывает слои стенки (запас < 2); положенный на бок
  // профилем в плоскость слоя уголок выдерживает — подбор должен это найти
  const Bracket b(8000);
  REQUIRE(kika::app::can_run(b.job));
  const auto res = kika::advisor::advise(b.request({false, true, true}, 8000));
  REQUIRE(res.strength_evaluated);
  REQUIRE(!res.ranked.empty());
  const auto& best = res.ranked.front();
  CHECK(best.feasible);
  CHECK(best.printable);
  REQUIRE(best.sf);
  CHECK(*best.sf >= 2.0);
  // профиль уголка — в плоскости XY: поворот на 90° вокруг X, остальное не трогаем
  CHECK(std::abs(best.print.rotate[0]) == 90);
  CHECK(best.print.rotate[1] == 0);
  CHECK(res.summary.find("Лучше всего") != std::string::npos);
  // в положениях есть и исходное (с меньшим запасом)
  bool upright = false;
  for (const auto& c : res.orientations)
    if (c.print.rotate == std::array<double, 3>{0, 0, 0}) {
      upright = true;
      REQUIRE(c.sf);
      CHECK(*c.sf < *best.sf);
    }
  CHECK(upright);
}

TEST_CASE("Подбор: быстро и экономно — самый дешёвый вариант, который выдерживает", "[advisor][slow]") {
  const Bracket b(8000);
  const auto res = kika::advisor::advise(b.request({true, true, false}, 8000));
  REQUIRE(!res.ranked.empty());
  const auto& best = res.ranked.front();
  CHECK(best.feasible);
  CHECK(best.print.settings.layer_height > 0.25);  // быстрый слой
  // среди посчитанных подходящих он дешевле всех по «время × пластик»
  for (const auto& c : res.ranked)
    if (c.feasible && c.printable && c.sf) CHECK(best.time_s * best.grams <= c.time_s * c.grams * 1.0001);
}

TEST_CASE("Подбор без нагрузок: прочность не оценивается, остальное работает", "[advisor]") {
  const Bracket b(6000);
  auto rq = b.request({true, false, true}, 6000);
  rq.job.fixtures.clear();
  const auto res = kika::advisor::advise(rq);
  CHECK(!res.strength_evaluated);
  REQUIRE(!res.ranked.empty());
  CHECK(!res.ranked.front().sf);
  CHECK(res.summary.find("закрепления и нагрузки") != std::string::npos);
}
