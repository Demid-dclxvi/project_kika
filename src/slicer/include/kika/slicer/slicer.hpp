#pragma once
// Свой слайсер: треугольная модель → слои → стенки, сплошное и разреженное заполнение → G-code.
// G-code — в стиле OrcaSlicer (;LAYER_CHANGE, ;TYPE:, настройки в конце файла), поэтому его
// понимают и наш разбор G-code, и программы просмотра G-code; шапка и итоги в конце — как у
// PrusaSlicer, по ним Moonraker (Klipper) показывает время, пластик и миниатюру. Контуры — Clipper2.

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/geometry/mesh.hpp"

namespace kika::slicer {

// Прошивка принтера: от неё зависят служебные команды в G-code.
enum class Flavor {
  Marlin,   // Marlin, RepRapFirmware и похожие: только стандартные M- и G-команды
  Klipper,  // Klipper: разметка деталей (EXCLUDE_OBJECT), номера слоёв на экране, pressure advance
};
std::string_view flavor_key(Flavor f);  // marlin, klipper
std::optional<Flavor> flavor_from_key(std::string_view key);

// Принтер: стол, сопло, пределы движения, откат, стартовый и финальный код.
struct Printer {
  std::string key = "qidi_q2";
  std::string name = "QIDI Q2";
  Flavor flavor = Flavor::Klipper;
  double bed_x = 270, bed_y = 270, max_z = 256;  // мм
  double nozzle = 0.4;
  double filament_diameter = 1.75;
  double max_nozzle_temp = 370, max_bed_temp = 120;
  double max_chamber_temp = 65;  // 0 — камера без подогрева
  // пределы движения: скорости и ускорения из настроек печати к ним прижимаются
  double max_speed = 600;        // мм/с
  double max_accel = 20000;      // мм/с²
  double corner_speed = 8;       // мм/с: скорость в прямом углу (square_corner_velocity / jerk)
  double max_speed_z = 20;       // мм/с
  // откат (пластик может задать свою длину и скорость)
  double retract_length = 0.8, retract_speed = 30;  // мм, мм/с
  double retract_min_travel = 1.0;                  // мм: короче — без отката
  double z_hop = 0.4;                               // подъём сопла на перемещениях с откатом, мм
  bool aux_fan = true;          // боковой обдув модели (M106 P2)
  bool thumbnails = true;       // миниатюры в G-code (экран принтера, Moonraker)
  // Подстановки: {nozzle_temp}, {first_layer_nozzle_temp}, {bed_temp}, {first_layer_bed_temp},
  // {chamber_temp}, {bed_x}, {bed_y}, {max_z}, {filament_type}, {total_layers},
  // {max_layer_z}, {lift_z} (верх детали + 3 мм), {park_z} (не ниже половины высоты),
  // в коде слоя — {layer_num} (с 1) и {layer_z}.
  std::string start_gcode;
  std::string end_gcode;
  std::string layer_gcode;       // в начале каждого слоя
};

// Пластик: температуры, поток, обдув, откат, плотность и цена.
struct Filament {
  std::string name = "PETG";  // название профиля
  std::string type = "PETG";  // как в базе материалов: PLA, PETG, ABS…
  double nozzle_temp = 240, first_layer_nozzle_temp = 245;
  double bed_temp = 70, first_layer_bed_temp = 70;
  double chamber_temp = 0;       // 0 — не греть
  double flow = 1.0;             // поправка потока
  double max_volumetric = 12;    // мм³/с: предел расхода сопла для этого пластика
  double max_speed = 300;        // мм/с — ограничение для этого пластика
  double density = 1.27;         // г/см³
  double cost = 0;               // за кг, 0 — не считать
  // обдув модели, доли 0…1
  double fan_min = 0.3, fan_max = 0.6;
  int fan_off_layers = 1;        // первые слои без обдува
  double fan_full_time = 20;     // слой короче (с) — обдув сильнее, до fan_max
  double slowdown_time = 6;      // слой короче (с) — печать медленнее, но не ниже slowdown_speed
  double slowdown_speed = 20;    // мм/с
  double overhang_fan = 0.9;     // нависающие стенки и мосты
  double aux_fan = 0;            // боковой обдув, если есть у принтера
  // откат: отрицательное — как у принтера
  double retract_length = -1, retract_speed = -1;
  double pressure_advance = -1;  // Klipper: SET_PRESSURE_ADVANCE; отрицательное — не задавать
};

const std::vector<Printer>& printers();
const Printer* find_printer(std::string_view key);
// Принтер по умолчанию — первый в списке (QIDI Q2).
const Printer& default_printer();
// Пластик по названию (как в базе материалов); неизвестный — с настройками PLA и тем же названием.
Filament filament_preset(std::string_view type);
// Встроенные пластики (по одному на тип).
const std::vector<Filament>& filaments();

// Рисунок разреженного заполнения.
enum class Pattern {
  Grid,         // сетка: два направления на каждом слое
  Rectilinear,  // прямые: одно направление, через слой поперёк
  Triangles,    // треугольники: три направления через 60°
  Lines,        // линии одного направления
  Gyroid,       // гироид: волнистые линии, в объёме — гладкая поверхность без слабого направления
  Honeycomb,    // соты: шестиугольные призмы
  Honeycomb3D,  // 3D-соты: зигзаги, сходящиеся через слой в замкнутые ячейки
  Cubic,        // кубы, поставленные на вершину: три направления, сдвигаются с высотой
  TriHexagon,   // тригексагон: треугольники и шестиугольники
  Concentric,   // концентрическое: контуры, повторяющие форму слоя
};
// Ключ как в OrcaSlicer: grid, rectilinear, triangles, line, gyroid, honeycomb, 3dhoneycomb,
// cubic, tri-hexagon, concentric.
std::string_view pattern_key(Pattern p);
std::optional<Pattern> pattern_from_key(std::string_view key);
// Название по-русски — для окна и сообщений.
std::string_view pattern_name(Pattern p);
// Все рисунки по порядку.
const std::vector<Pattern>& patterns();

// Поддержки.
enum class Support {
  None,    // без поддержек
  Normal,  // обычные: колонны линий под всей площадью нависания
  Tree,    // древовидные: ветви от точек под нависанием сходятся в стволы до стола
};
std::string_view support_key(Support s);  // none, normal, tree
std::optional<Support> support_from_key(std::string_view key);
std::string_view support_name(Support s);

// Шов: где начинается замкнутая стенка.
enum class Seam {
  Aligned,  // друг над другом, по возможности в углу
  Nearest,  // ближе к соплу: меньше перемещений
  Rear,     // сзади детали
  Random,   // в случайном месте каждого слоя
};
std::string_view seam_key(Seam s);  // aligned, nearest, rear, random
std::optional<Seam> seam_from_key(std::string_view key);
std::string_view seam_name(Seam s);

struct Settings {
  Printer printer = default_printer();
  Filament filament = filament_preset("PETG");
  double layer_height = 0.2;
  double first_layer_height = 0.2;
  double line_width = 0.42;
  int wall_loops = 2;
  bool outer_wall_first = false;  // сначала наружная стенка (точнее размеры), иначе изнутри наружу
  Seam seam = Seam::Aligned;
  int top_layers = 5;
  int bottom_layers = 3;
  double infill_density = 0.15;  // 0…1
  Pattern pattern = Pattern::Grid;
  double infill_angle = 45;      // градусы
  bool alternate_solid = true;   // сплошные слои: угол через слой ±90° (иначе все под infill_angle)
  double infill_overlap = 0.15;  // заход разреженного заполнения на стенки, доля ширины линии
  bool skirt = true;
  int skirt_loops = 1;
  double skirt_distance = 3;     // мм
  double brim_width = 0;         // кайма вокруг первого слоя, мм (0 — нет)
  double brim_gap = 0.1;         // зазор между каймой и деталью, мм
  // скорости, мм/с (прижимаются к пределам принтера и пластика)
  double speed_outer_wall = 200, speed_inner_wall = 300, speed_solid = 250, speed_top = 200, speed_sparse = 270;
  double speed_first_layer = 50, speed_travel = 500;
  double speed_overhang = 30;    // нависающие стенки и мосты
  // ускорения, мм/с² (0 — как печать по умолчанию)
  double accel_default = 10000, accel_outer_wall = 5000, accel_inner_wall = 0, accel_top = 2000,
         accel_sparse = 0, accel_first_layer = 500, accel_travel = 10000;
  // поддержки
  Support support = Support::None;
  double support_angle = 40;           // нависание положе этого угла к горизонтали — с поддержкой, градусы
  bool support_on_bed_only = false;    // только от стола: на деталь поддержки не опираются
  double support_z_gap = 0.2;          // зазор между поддержкой и деталью по высоте, мм
  double support_xy_gap = 0.6;         // зазор до стенок детали, мм
  int support_interface_layers = 2;    // плотных слоёв под деталью
  double support_spacing = 2.5;        // шаг линий основы, мм
  double support_interface_spacing = 0.5;
  double speed_support = 150, speed_support_interface = 80;  // мм/с
  // служебное
  bool label_objects = true;     // Klipper: EXCLUDE_OBJECT — отмена детали, карта стола только под ней
  std::string generated_at;      // «2026-10-12 at 23:00:00» для шапки (Moonraker узнаёт по ней сведения о печати)
};

// Ход нарезки: доля 0…1 и текст для человека. Бросить исключение — прервать.
using Progress = std::function<void(double fraction, std::string_view text)>;

struct Result {
  std::string gcode;
  int layers = 0;
  double filament_mm = 0;   // длина прутка, мм
  double filament_g = 0;
  double print_time_s = 0;  // оценка с разгонами и торможениями, без нагрева и PRINT_START
  double support_g = 0;     // из них на поддержки, г
  double cost = 0;          // стоимость пластика (если у пластика задана цена)
  std::vector<std::string> warnings;
};

// Нарезка сетки. Сетка должна стоять на столе (низ на Z = 0) в координатах принтера;
// поставить её можно geometry::place_on_bed.
Result slice(const geometry::TriangleMesh& mesh, const Settings& s, const Progress& progress = {},
             std::string_view model_name = {});

// Текущее время для шапки G-code (Settings::generated_at): «2026-10-12 at 23:00:00».
std::string now_stamp();

// Контуры сечения сетки плоскостью Z = z (для проверок и окна): замкнутые многоугольники,
// внешние — против часовой стрелки, отверстия — по часовой.
std::vector<std::vector<std::array<double, 2>>> section(const geometry::TriangleMesh& mesh, double z);

}  // namespace kika::slicer
