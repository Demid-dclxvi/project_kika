// Принтеры и пластики для своего слайсера.

#include <algorithm>
#include <cctype>

#include "kika/slicer/slicer.hpp"

namespace kika::slicer {

namespace {

// Стартовый код: нагрев, парковка, полоса прочистки у левого края стола.
// Подходит для Marlin и Klipper. Перед печатью сверить с кодом своего принтера из его слайсера.
const char* kStart = R"(M140 S{first_layer_bed_temp}
M104 S{first_layer_nozzle_temp}
G28
M190 S{first_layer_bed_temp}
M109 S{first_layer_nozzle_temp}
G90
M83
G92 E0
G1 Z5 F3000
G1 X3 Y20 F6000
G1 Z0.3 F600
G1 X3 Y120 E10 F1200
G1 X3.6 Y120 F3000
G1 X3.6 Y20 E10 F1200
G1 Z2 F600
G92 E0)";

const char* kEnd = R"(M104 S0
M140 S0
M107
G91
G1 E-2 F2400
G1 Z10 F600
G90
G1 X5 Y{bed_y} F6000
M84)";

}  // namespace

const std::vector<Printer>& printers() {
  static const std::vector<Printer> list = [] {
    Printer i7;
    i7.start_gcode = kStart;
    i7.end_gcode = kEnd;
    Printer generic;
    generic.key = "generic_220";
    generic.name = "Принтер 220×220 (Marlin)";
    generic.bed_x = 220;
    generic.bed_y = 220;
    generic.max_z = 250;
    generic.max_bed_temp = 110;
    generic.max_nozzle_temp = 260;
    generic.start_gcode = kStart;
    generic.end_gcode = kEnd;
    return std::vector<Printer>{i7, generic};
  }();
  return list;
}

const Printer& default_printer() { return printers().front(); }

const Printer* find_printer(std::string_view key) {
  for (const auto& p : printers())
    if (p.key == key) return &p;
  return nullptr;
}

Filament filament_preset(std::string_view type) {
  std::string t(type);
  for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  // сопло, сопло 1-го слоя, стол, стол 1-го слоя, обдув, плотность, скорость
  struct P {
    const char* key;
    double nozzle, nozzle1, bed, bed1, fan, density, speed;
  };
  static const P table[] = {
      {"PLA", 210, 215, 60, 60, 1.0, 1.24, 250},     {"PLA-CF", 220, 225, 60, 60, 1.0, 1.29, 200},
      {"PETG", 240, 245, 70, 70, 0.4, 1.27, 200},    {"PETG-CF", 250, 255, 75, 75, 0.3, 1.30, 160},
      {"ABS", 250, 255, 100, 100, 0.0, 1.04, 200},   {"ASA", 255, 260, 100, 100, 0.2, 1.07, 200},
      {"PC", 270, 270, 100, 100, 0.0, 1.20, 120},    {"PA", 260, 260, 80, 80, 0.0, 1.14, 120},
      {"PA-CF", 280, 280, 90, 90, 0.0, 1.25, 120},   {"TPU", 225, 225, 45, 45, 1.0, 1.21, 40},
  };
  Filament f;
  f.type = std::string(type);
  const P* p = nullptr;
  for (const auto& e : table)
    if (t == e.key) p = &e;
  if (!p) p = &table[0];
  f.nozzle_temp = p->nozzle;
  f.first_layer_nozzle_temp = p->nozzle1;
  f.bed_temp = p->bed;
  f.first_layer_bed_temp = p->bed1;
  f.fan = p->fan;
  f.density = p->density;
  f.max_speed = p->speed;
  if (f.type.empty()) f.type = "PLA";
  return f;
}

std::string_view pattern_key(Pattern p) {
  switch (p) {
    case Pattern::Grid: return "grid";
    case Pattern::Rectilinear: return "rectilinear";
    case Pattern::Triangles: return "triangles";
    case Pattern::Lines: return "line";
  }
  return "grid";
}

std::optional<Pattern> pattern_from_key(std::string_view key) {
  std::string k(key);
  for (char& c : k) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (k == "grid") return Pattern::Grid;
  if (k == "rectilinear" || k == "zigzag") return Pattern::Rectilinear;
  if (k == "triangles") return Pattern::Triangles;
  if (k == "line" || k == "lines") return Pattern::Lines;
  return std::nullopt;
}

}  // namespace kika::slicer
