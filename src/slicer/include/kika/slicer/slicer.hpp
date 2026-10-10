#pragma once
// Свой слайсер: треугольная модель → слои → стенки, сплошное и разреженное заполнение → G-code.
// G-code — в стиле OrcaSlicer (;LAYER_CHANGE, ;TYPE:, настройки в конце файла), поэтому его
// понимают и наш разбор G-code, и программы просмотра G-code. Контуры — Clipper2.

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/geometry/mesh.hpp"

namespace kika::slicer {

// Принтер: стол, сопло, стартовый и финальный код.
struct Printer {
  std::string key = "sparkx_i7";
  std::string name = "Creality SPARKX i7";
  double bed_x = 260, bed_y = 260, max_z = 255;  // мм
  double nozzle = 0.4;
  double filament_diameter = 1.75;
  double max_bed_temp = 100;
  double max_nozzle_temp = 300;
  // Подстановки: {nozzle_temp}, {first_layer_nozzle_temp}, {bed_temp}, {first_layer_bed_temp},
  // {bed_x}, {bed_y}, {max_z}, {filament_type}
  std::string start_gcode;
  std::string end_gcode;
};

// Пластик: температуры, обдув, плотность для массы.
struct Filament {
  std::string type = "PETG";  // как в базе материалов: PLA, PETG, ABS…
  double nozzle_temp = 240, first_layer_nozzle_temp = 240;
  double bed_temp = 70, first_layer_bed_temp = 70;
  double fan = 0.4;           // 0…1, со второго слоя
  double density = 1.27;      // г/см³
  double max_speed = 200;     // мм/с — ограничение для этого пластика
};

const std::vector<Printer>& printers();
const Printer* find_printer(std::string_view key);
// Принтер по умолчанию — первый в списке (Creality SPARKX i7).
const Printer& default_printer();
// Пластик по названию (как в базе материалов); неизвестный — с настройками PLA и тем же названием.
Filament filament_preset(std::string_view type);

// Рисунок разреженного заполнения.
enum class Pattern { Grid, Rectilinear, Triangles, Lines };
std::string_view pattern_key(Pattern p);  // как в Orca: grid, rectilinear, triangles, line
std::optional<Pattern> pattern_from_key(std::string_view key);

struct Settings {
  Printer printer = default_printer();
  Filament filament = filament_preset("PETG");
  double layer_height = 0.2;
  double first_layer_height = 0.2;
  double line_width = 0.42;
  int wall_loops = 2;
  int top_layers = 5;
  int bottom_layers = 3;
  double infill_density = 0.15;  // 0…1
  Pattern pattern = Pattern::Grid;
  double infill_angle = 45;      // градусы
  bool alternate_solid = true;   // сплошные слои: угол через слой ±90° (иначе все под infill_angle)
  double infill_overlap = 0.15;  // заход разреженного заполнения на стенки, доля ширины линии
  bool skirt = true;
  double skirt_distance = 3;     // мм
  // скорости, мм/с
  double speed_outer_wall = 60, speed_inner_wall = 100, speed_solid = 100, speed_top = 60, speed_sparse = 120;
  double speed_first_layer = 30, speed_travel = 200;
  double retract_length = 0.8, retract_speed = 40;  // мм, мм/с
  double retract_min_travel = 2.0;                  // мм
};

// Ход нарезки: доля 0…1 и текст для человека. Бросить исключение — прервать.
using Progress = std::function<void(double fraction, std::string_view text)>;

struct Result {
  std::string gcode;
  int layers = 0;
  double filament_mm = 0;   // длина прутка, мм
  double filament_g = 0;
  double print_time_s = 0;  // оценка без ускорений
  std::vector<std::string> warnings;
};

// Нарезка сетки. Сетка должна стоять на столе (низ на Z = 0) в координатах принтера;
// поставить её можно geometry::place_on_bed.
Result slice(const geometry::TriangleMesh& mesh, const Settings& s, const Progress& progress = {},
             std::string_view model_name = {});

// Контуры сечения сетки плоскостью Z = z (для проверок и окна): замкнутые многоугольники,
// внешние — против часовой стрелки, отверстия — по часовой.
std::vector<std::vector<std::array<double, 2>>> section(const geometry::TriangleMesh& mesh, double z);

}  // namespace kika::slicer
