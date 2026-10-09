#pragma once
// Разбор G-code: команды принтера → отрезки экструзии детали.
//
// Поддерживаются OrcaSlicer, Bambu Studio, PrusaSlicer, SuperSlicer, Cura, Simplify3D
// (по комментариям ;TYPE: / ; FEATURE: / ;LAYER_CHANGE и т. п.). Учитываются абсолютная
// и относительная экструзия (M82/M83, G90/G91, G92), дуги G2/G3, дюймы (G20),
// отсечение стартового и финального кода, поддержек, юбки, каймы, черновой башни.
//
// Перенос fdmfea/gcode.py из прототипа. Отличия от прототипа — в docs/ARCHITECTURE.md.

#include <filesystem>
#include <stdexcept>
#include <string_view>

#include "kika/gcode/toolpaths.hpp"

namespace kika::gcode {

// В G-code нет ни одного отрезка экструзии детали, или файл не читается.
class ParseError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ParseOptions {
  // Длина хорды при разбиении дуг G2/G3 на отрезки, мм.
  double arc_segment_length = 0.5;
};

Toolpaths parse(std::string_view text, const ParseOptions& options = {});

Toolpaths load(const std::filesystem::path& path, const ParseOptions& options = {});

}  // namespace kika::gcode
