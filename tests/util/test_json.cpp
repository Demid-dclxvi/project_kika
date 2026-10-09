// JSON ядра: разбор заданий и запись итогов.

#include <cmath>
#include <limits>
#include <string>

#include <catch2/catch_test_macros.hpp>

#include "kika/util/json.hpp"

using kika::json::dump;
using kika::json::parse;
using kika::json::ParseError;
using kika::json::Value;

TEST_CASE("JSON: разбор значений всех типов", "[json]") {
  const Value v = parse(R"({"a": 1, "b": -2.5e3, "c": "текст", "d": [true, false, null], "e": {}, "f": []})");
  REQUIRE(v.is_object());
  CHECK(v.find("a")->is_int());
  CHECK(v.find("a")->as_int() == 1);
  CHECK(v.find("b")->is_number());
  CHECK_FALSE(v.find("b")->is_int());
  CHECK(v.find("b")->as_double() == -2500.0);
  CHECK(v.find("c")->as_string() == "текст");
  CHECK(v.find("d")->size() == 3);
  CHECK(v.find("d")->at(0).as_bool());
  CHECK(v.find("d")->at(2).is_null());
  CHECK(v.find("e")->is_object());
  CHECK(v.find("f")->is_array());
  CHECK(v.find("нет") == nullptr);
  // порядок ключей сохраняется
  const auto& o = v.as_object();
  REQUIRE(o.size() == 6);
  CHECK(o[0].first == "a");
  CHECK(o[5].first == "f");
}

TEST_CASE("JSON: строки и escape-последовательности", "[json]") {
  CHECK(parse(R"("a\"b\\c\/d\n\t")").as_string() == "a\"b\\c/d\n\t");
  CHECK(parse(R"("Ж")").as_string() == "Ж");
  CHECK(parse(R"("😀")").as_string() == "\xF0\x9F\x98\x80");  // суррогатная пара
  CHECK(parse("\xEF\xBB\xBF{\"x\": 1}").find("x")->as_int() == 1);    // метка порядка байтов
  CHECK(dump(Value("кавычка \" и \\ и\nперевод")) == R"("кавычка \" и \\ и\nперевод")");
}

TEST_CASE("JSON: ошибки с номером строки и столбца", "[json]") {
  auto where = [](const char* text) {
    try {
      parse(text);
    } catch (const ParseError& e) {
      return std::to_string(e.line()) + ":" + std::to_string(e.column());
    }
    return std::string("нет ошибки");
  };
  CHECK(where("{\n  \"a\": 1,\n}") == "3:1");     // лишняя запятая
  CHECK(where("{\"a\" 1}") == "1:6");             // нет двоеточия
  CHECK(where("[1, 2") == "1:6");                 // не закрыт список
  CHECK(where("{'a': 1}") == "1:2");              // одинарные кавычки
  CHECK(where("{\"a\": 01}") == "1:8");           // ведущий ноль
  CHECK(where("{\"a\": 1} x") == "1:10");         // мусор после конца
  CHECK(where("{\"имя\": тру}") == "1:9");        // столбцы считаются в символах, не байтах
  CHECK_THROWS_AS(parse(""), ParseError);
}

TEST_CASE("JSON: запись как json.dumps в Python", "[json]") {
  Value v = Value::object();
  v["int"] = 3;
  v["float"] = 2.0;
  v["small"] = 1e-5;
  v["pi"] = 0.1 + 0.2;
  v["big"] = 1e16;
  v["neg0"] = -0.0;
  v["nan"] = std::numeric_limits<double>::quiet_NaN();
  v["list"] = Value::array_of(std::vector<double>{1.5, 2.0});
  v["empty"] = Value::array();
  CHECK(dump(v) ==
        R"({"int": 3, "float": 2.0, "small": 1e-05, "pi": 0.30000000000000004, "big": 1e+16, "neg0": -0.0, )"
        R"("nan": null, "list": [1.5, 2.0], "empty": []})");
  CHECK(dump(parse(R"({"a": [1, {"b": null}]})"), 2) == "{\n  \"a\": [\n    1,\n    {\n      \"b\": null\n    }\n  ]\n}");
  // запись и чтение обратно дают то же значение
  const Value back = parse(dump(v));
  CHECK(back.find("pi")->as_double() == 0.1 + 0.2);
  CHECK(back.find("float")->is_number());
  CHECK_FALSE(back.find("float")->is_int());
}

TEST_CASE("JSON: истинность как в Python и повтор ключа", "[json]") {
  CHECK_FALSE(Value().truthy());
  CHECK_FALSE(Value(0).truthy());
  CHECK_FALSE(Value("").truthy());
  CHECK_FALSE(Value::array().truthy());
  CHECK(Value(0.5).truthy());
  CHECK(parse(R"({"a": 1, "a": 2})").find("a")->as_int() == 2);
  CHECK_THROWS_AS(Value("x").as_double(), std::invalid_argument);
}
