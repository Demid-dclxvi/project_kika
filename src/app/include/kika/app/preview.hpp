#pragma once
// Предпросмотр нарезки: G-code (свой, OrcaSlicer, PrusaSlicer, Bambu Studio, Cura) → движения
// с типом линии, шириной, высотой слоя, скоростью и обдувом; слои; сводка по типам линий (время,
// пластик); сетка нитей для OpenGL с границами слоёв и движений — окно рисует диапазон слоёв
// и «проигрывает» верхний слой по движениям. Без Qt: всё проверяется тестами.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/app/roads.hpp"
#include "kika/app/scene.hpp"

namespace kika::app {

// Тип линии (как в предпросмотре OrcaSlicer). Travel — перемещение без выдавливания.
enum class LineType : std::uint8_t {
  OuterWall,
  InnerWall,
  OverhangWall,
  Sparse,
  Solid,
  Top,
  Bottom,
  Bridge,
  GapFill,
  Ironing,
  Skirt,
  Brim,
  Support,
  SupportInterface,
  PrimeTower,
  Custom,  // стартовый и финальный код, прочистка, неизвестные подписи
  Travel,
};
constexpr std::size_t kLineTypeCount = static_cast<std::size_t>(LineType::Travel) + 1;

std::string_view line_type_name(LineType t);  // по-русски: «Наружная стенка»…
std::string_view line_type_key(LineType t);   // outer_wall, inner_wall… (для сценариев и JSON)
std::optional<LineType> line_type_from_key(std::string_view key);
Rgb line_type_color(LineType t);
// Тип по подписи ;TYPE: / ; FEATURE: из G-code любого из поддержанных слайсеров.
LineType line_type_of(std::string_view label);

struct PreviewMove {
  float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  float z = 0;                 // высота сопла (верх валика), мм
  float width = 0, height = 0; // валик, мм (у перемещений 0)
  float speed = 0;             // мм/с
  float fan = 0;               // обдув модели 0…1
  float volume = 0;            // пластик, мм³
  float time = 0;              // оценка, с
  LineType type = LineType::Custom;
  std::uint32_t layer = 0;
  float length() const;
  bool extrusion() const { return type != LineType::Travel; }
};

struct PreviewLayer {
  double z = 0, height = 0;
  std::uint32_t first = 0, last = 0;  // движения [first, last)
  double time = 0;                    // с
};

struct TypeStat {
  LineType type = LineType::Custom;
  std::size_t moves = 0;
  double length = 0;  // мм
  double time = 0;    // с
  double volume = 0;  // мм³
  double grams = 0;
};

struct Preview {
  std::vector<PreviewMove> moves;  // по порядку G-code
  std::vector<PreviewLayer> layers;
  std::vector<TypeStat> stats;     // только встретившиеся типы, по порядку LineType
  double time_s = 0;               // всего: из сводки слайсера в G-code или своя оценка
  bool time_from_file = false;
  double filament_mm = 0, filament_g = 0;
  std::optional<double> cost;      // если слайсер её посчитал
  double density = 1.24, filament_diameter = 1.75;
  std::string slicer;              // кто нарезал (из первой строки), пусто — неизвестно
  std::array<double, 3> min{0, 0, 0}, max{0, 0, 0};  // габарит выдавливания
  std::size_t retractions = 0;

  bool empty() const { return layers.empty(); }
  const TypeStat* stat(LineType t) const;
};

// Разбор G-code. Ошибок не бросает: непонятные строки пропускаются.
Preview parse_preview(std::string_view gcode);

// Время «1d 2h 3m 4s» / «1h 2m» / «45m 10s» → секунды (пусто — не время).
std::optional<double> parse_duration(std::string_view s);

// Чем раскрашивать нити.
enum class ColorBy { Type, Speed, Flow, Width, Height, Fan, LayerTime };
std::string_view color_by_name(ColorBy c);
std::string_view color_by_key(ColorBy c);  // type, speed, flow, width, height, fan, layer_time
std::optional<ColorBy> color_by_from_key(std::string_view key);

struct PreviewView {
  ColorBy color = ColorBy::Type;
  std::array<bool, kLineTypeCount> visible{};  // по LineType
  PreviewView();
};

// Легенда: категории (тип линии) или шкала значений с подписями.
struct PreviewLegend {
  std::string title, unit;
  struct Entry {
    Rgb color{};
    std::string label;
    std::optional<LineType> type;  // для типов: можно скрыть щелчком
    bool visible = true;
    std::string time, share, grams;  // для типов: «12 мин», «34 %», «5,6 г»
  };
  std::vector<Entry> entries;
  double lo = 0, hi = 0;  // для шкалы
};
PreviewLegend preview_legend(const Preview& p, const PreviewView& v);

// Сетка нитей. Порядок треугольников — порядок движений, поэтому слои [lo, hi] и верхний слой
// до движения k рисуются одним диапазоном индексов.
struct PreviewMesh {
  RoadMesh mesh;
  std::vector<std::uint32_t> layer_end;  // индексов после каждого слоя
  std::vector<std::uint32_t> move_end;   // индексов после каждого движения
  // перемещения — линии: x, y, z на вершину, по две вершины на перемещение
  std::vector<float> travel;
  std::vector<std::uint32_t> travel_layer_end, travel_move_end;  // вершин после слоя / движения
};
void build_preview_mesh(const Preview& p, const PreviewView& v, PreviewMesh& out);

// Диапазон индексов треугольников для слоёв [lo, hi], верхний — до движения upto (включительно;
// пусто — весь слой).
std::pair<std::uint32_t, std::uint32_t> preview_range(const Preview& p, const PreviewMesh& m, std::size_t lo, std::size_t hi,
                                                      std::optional<std::size_t> upto = std::nullopt);
std::pair<std::uint32_t, std::uint32_t> travel_range(const Preview& p, const PreviewMesh& m, std::size_t lo, std::size_t hi,
                                                     std::optional<std::size_t> upto = std::nullopt);

// Подписи: время «1 ч 05 мин», «12 мин 30 с», граммы «12,3 г».
std::string fmt_duration(double s);

}  // namespace kika::app
