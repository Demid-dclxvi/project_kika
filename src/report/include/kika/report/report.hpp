#pragma once
// HTML-отчёт: сводка по расчётным случаям и интерактивный 3D-просмотр результатов.
// Перенос fdmfea/export.py. Отчёт — один файл: данные, three.js и просмотрщик внутри,
// открывается в любом современном браузере без интернета (шрифты подгружаются, если он есть).

#include <string>
#include <string_view>

#include "kika/analysis/analysis.hpp"
#include "kika/util/json.hpp"

namespace kika::report {

struct ReportInfo {
  std::string title;       // пусто — поле title задания, иначе «Расчёт детали»
  std::string gcode_name;  // имя файла G-code для шапки
  std::string date;        // «дд.мм.гггг чч:мм»; пусто — текущее время
};

// job_json — задание, как оно записано в файле (попадает в отчёт целиком).
std::string report_html(const analysis::Model& model, const analysis::AnalysisResult& result,
                        const json::Value& job_json, ReportInfo info = {});

// Данные для просмотрщика (модель, поля по случаям, задание) — то, что в отчёте лежит
// в сжатом виде. Отдельно — для проверок.
json::Value report_payload(const analysis::Model& model, const analysis::AnalysisResult& result,
                           const json::Value& job_json, const ReportInfo& info);

// Сжатие gzip и base64 — как данные упакованы в отчёте.
std::string gzip_compress(std::string_view data, int level = 6);
std::string gzip_decompress(std::string_view data);
std::string base64_encode(std::string_view data);
std::string base64_decode(std::string_view text);

}  // namespace kika::report
