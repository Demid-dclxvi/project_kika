#pragma once
// Траектории печати, извлечённые из G-code: отрезки экструзии и сведения о печати.
//
// Единицы: миллиметры, мм³. Оси — как у принтера: X, Y по столу, Z вверх по слоям.

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace kika::gcode {

// Роль отрезка в детали. Числовые значения совпадают с прототипом на Python.
enum class Role : std::uint8_t {
  Excluded = 0,   // поддержки, юбка, кайма, башня, стартовый и финальный код
  OuterWall = 1,  // внешняя стенка
  InnerWall = 2,  // внутренние стенки
  Solid = 3,      // сплошное заполнение, верх и низ, мосты, заполнение щелей
  Sparse = 4,     // разреженное заполнение
  Unknown = 5,    // тип не указан — считаем сплошным материалом
};

// Роль по подписи типа линии из комментария слайсера (;TYPE:, ; FEATURE:).
Role classify_role(std::string_view type_label);

// Название роли по-русски для отчётов.
std::string_view role_name(Role role);

// Короткий ключ роли латиницей для JSON и CSV.
std::string_view role_key(Role role);

// Один отрезок экструзии.
struct Segment {
  double x0 = 0, y0 = 0;  // начало, мм
  double x1 = 0, y1 = 0;  // конец, мм
  double z = 0;           // Z верха валика (высота сопла при печати), мм
  double h = 0;           // толщина слоя для этого отрезка, мм
  double volume = 0;      // объём выдавленного пластика, мм³
  Role role = Role::Unknown;
  std::int32_t layer = 0;  // порядковый номер слоя (по уникальным Z)
};

// Сведения о печати, найденные в G-code.
struct Info {
  std::string slicer = "unknown";
  double filament_diameter = 1.75;
  std::string filament_type;
  std::optional<double> filament_density;
  std::optional<double> layer_height;
  std::optional<double> first_layer_height;
  std::optional<double> nozzle_diameter;
  std::optional<double> line_width;
  std::optional<double> infill_density;  // доля 0..1
  std::string infill_pattern;
  std::optional<int> wall_loops;
  std::optional<int> top_layers;
  std::optional<int> bottom_layers;

  std::map<std::string, std::string> settings;  // распознанные настройки как в файле
  std::vector<std::string> warnings;

  std::size_t n_lines = 0;
  int n_layers = 0;
  double filament_used_mm = 0.0;  // длина прутка на всю печать, включая служебные движения
  double excluded_volume = 0.0;   // объём, отброшенный как не относящийся к детали, мм³
};

struct BBox {
  std::array<double, 3> min{};
  std::array<double, 3> max{};
};

// Отрезки экструзии детали (только конструкционные: без поддержек, юбки и т. п.).
struct Toolpaths {
  std::vector<Segment> segments;
  Info info;

  std::size_t size() const { return segments.size(); }
  double total_volume() const;
  // Габарит по осям валиков: по X, Y — концы отрезков, по Z — от низа нижнего слоя до верха верхнего.
  BBox bbox() const;
};

}  // namespace kika::gcode
