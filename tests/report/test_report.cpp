// HTML-отчёт: упаковка данных и сборка страницы. Сверка с report_html прототипа —
// в tools/compare_with_prototype.py; проверка в браузере — вручную (см. docs/ARCHITECTURE.md).

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "kika/analysis/analysis.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/report/report.hpp"
#include "kika/util/json.hpp"

using kika::json::Value;

TEST_CASE("base64 и gzip: туда и обратно", "[report]") {
  using namespace kika::report;
  CHECK(base64_encode("") == "");
  CHECK(base64_encode("f") == "Zg==");
  CHECK(base64_encode("fo") == "Zm8=");
  CHECK(base64_encode("foo") == "Zm9v");
  CHECK(base64_encode("Привет") == "0J/RgNC40LLQtdGC");
  std::string bin;
  for (int i = 0; i < 1000; ++i) bin += static_cast<char>((i * 37) & 0xFF);
  CHECK(base64_decode(base64_encode(bin)) == bin);
  const std::string text(100000, 'a');
  const std::string z = gzip_compress(text);
  CHECK(z.size() < 1000);
  CHECK(static_cast<unsigned char>(z[0]) == 0x1F);  // заголовок gzip
  CHECK(static_cast<unsigned char>(z[1]) == 0x8B);
  CHECK(gzip_decompress(z) == text);
  CHECK_THROWS(gzip_decompress(z.substr(0, z.size() / 2)));
}

TEST_CASE("Отчёт: страница и упакованные данные", "[report]") {
  kika::analysis::ModelOptions mo;
  mo.voxel = 2.0;
  const auto model = kika::analysis::build_model(
      kika::gcode::load(std::filesystem::path(KIKA_SOURCE_DIR) / "tests" / "data" / "cantilever_flat.gcode"), mo);
  const Value job_json = kika::json::parse(R"({"title": "Балка <тест>", "subtitle": "Проверка отчёта",
      "material": "PLA", "cases": [{"name": "изгиб", "fixtures": [{"where": "xmin"}],
      "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -5]}]}]})");
  const auto res = kika::analysis::run_analysis(model, kika::analysis::parse_job(job_json));
  kika::report::ReportInfo info;
  info.gcode_name = "cantilever_flat.gcode";
  info.date = "01.02.2026 10:00";
  const std::string html = kika::report::report_html(model, res, job_json, info);

  CHECK(html.rfind("<!doctype html>\n<html lang=\"ru\">", 0) == 0);
  CHECK(html.find("<title>Балка &lt;тест&gt;</title>") != std::string::npos);
  CHECK(html.find("<p class=\"sub\">Проверка отчёта</p>") != std::string::npos);
  CHECK(html.find("01.02.2026 10:00 · cantilever_flat.gcode") != std::string::npos);
  CHECK(html.find("<h3>изгиб</h3>") != std::string::npos);
  CHECK(html.find("Three.js Authors") != std::string::npos);  // three.js встроен
  CHECK(html.find("FDMViewer") != std::string::npos);          // просмотрщик встроен
  for (const char* marker : {"__TITLE__", "__HEADER__", "__CASES__", "__DETAILS__", "__PAYLOAD__",
                             "/*__THREE__*/", "/*__VIEWER_JS__*/", "/*__REPORT_JS__*/", "/*__VIEWER_CSS__*/"})
    CHECK(html.find(marker) == std::string::npos);
  // числа с узким неразрывным пробелом между разрядами, как в прототипе
  const std::string n = std::to_string(model.vm.size());
  if (n.size() > 3) CHECK(html.find(n.substr(0, n.size() - 3) + " " + n.substr(n.size() - 3)) != std::string::npos);

  // данные для 3D-просмотра
  const std::string key = "window.FDM_PAYLOAD = \"";
  const auto p0 = html.find(key);
  REQUIRE(p0 != std::string::npos);
  const auto p1 = html.find('"', p0 + key.size());
  const Value payload = kika::json::parse(
      kika::report::gzip_decompress(kika::report::base64_decode(html.substr(p0 + key.size(), p1 - p0 - key.size()))));
  const Value& m = *payload.find("model");
  CHECK(m.find("n_elems")->as_int() == static_cast<long long>(model.vm.size()));
  CHECK(kika::report::base64_decode(m.find("elems")->as_string()).size() == 6 * model.vm.size());
  CHECK(kika::report::base64_decode(m.find("node_flat")->as_string()).size() ==
        4 * static_cast<std::size_t>(model.mesh.n_nodes));
  CHECK(m.find("grid")->find("z_edges")->size() == model.vm.z_edges.size());
  const Value& r = *payload.find("results");
  REQUIRE(r.find("results")->size() == 1);
  const Value& c = r.find("results")->at(0);
  CHECK(kika::report::base64_decode(c.find("sf")->as_string()).size() == 4 * model.vm.size());
  CHECK(kika::report::base64_decode(c.find("u")->as_string()).size() == 12 * static_cast<std::size_t>(model.mesh.n_nodes));
  CHECK(c.find("sel")->contains("fix0"));
  CHECK(c.find("sel")->contains("load0"));
  CHECK(c.find("summary")->find("name")->as_string() == "изгиб");
  CHECK(r.find("material")->find("key")->as_string() == "PLA");
  CHECK(*payload.find("job") == job_json);
  CHECK(payload.find("meta")->find("title")->as_string() == "Балка <тест>");
}
