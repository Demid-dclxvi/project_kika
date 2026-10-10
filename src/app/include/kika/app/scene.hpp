#pragma once
// Данные 3D-сцены для окна: видимые грани (с разрезом), цвета полей и легенда, выбор граней
// мышью (плоская грань, отверстие, кисть), площадь и центр выбора, луч из камеры.
// Без Qt: окно только рисует то, что посчитано здесь, а здесь всё проверяется тестами.
// Перенос логики fdmfea/web/viewer.js и app.js.

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kika/analysis/analysis.hpp"

namespace kika::app {

using Vec3 = std::array<double, 3>;
using Rgb = std::array<float, 3>;

constexpr Rgb rgb(std::uint32_t hex) {
  return {static_cast<float>((hex >> 16) & 0xFF) / 255.0f, static_cast<float>((hex >> 8) & 0xFF) / 255.0f,
          static_cast<float>(hex & 0xFF) / 255.0f};
}

// Цвета сцены (светлая тема прототипа; палитры проверены на различимость при нарушениях цветового зрения).
namespace palette {
inline constexpr Rgb kBackground = rgb(0xE9EBE6);
inline constexpr Rgb kGrid = rgb(0xC3C6BD);
inline constexpr Rgb kGrid2 = rgb(0xD9DBD3);
inline constexpr Rgb kPart = rgb(0xD5D8CC);
inline constexpr Rgb kNeutral = rgb(0xC9C8C1);
inline constexpr Rgb kSelection = rgb(0xF2A900);
inline constexpr Rgb kFixture = rgb(0x3B3F45);
inline constexpr Rgb kLoad = rgb(0xE34948);
inline constexpr Rgb kAxisX = rgb(0xD03B3B);
inline constexpr Rgb kAxisY = rgb(0x0CA30C);
inline constexpr Rgb kAxisZ = rgb(0x2A78D6);
}  // namespace palette

// Что показывать цветом.
enum class Field { Model, Structure, Density, SafetyFactor, Stress, Displacement, Mode };

struct LegendEntry {
  Rgb color{};
  std::string label;
};

struct Legend {
  enum class Kind { None, Steps, Categories, Ramp };
  Kind kind = Kind::None;
  std::string title;
  std::vector<LegendEntry> entries;  // ступени или категории
  std::vector<Rgb> ramp;             // плавная шкала от 0 до max
  double max = 0.0;
  std::string unit;
};

// Видимая грань: элемент, направление (0: −x … 5: +z), грань разреза.
struct VisibleFace {
  std::int32_t elem = 0;
  std::int8_t dir = 0;
  bool cut = false;
};

// Подсветка набора граней (ключи по возрастанию).
struct Overlay {
  std::vector<std::int64_t> keys;
  Rgb color{};
  float alpha = 0.85f;
};

// Попадание луча в грань.
struct Hit {
  std::size_t face = 0;  // номер видимой грани
  std::int32_t elem = 0;
  int dir = 0;
  bool cut = false;
  std::int64_t key = 0;
  Vec3 point{};  // система детали, мм
  double t = 0.0;
};

// Точка на грани — чтобы перенести выбор на сетку другой детальности.
struct FacePoint {
  Vec3 p{};
  int dir = 0;
};

// Число для подписей: десятичная запятая, без d — знаков тем меньше, чем больше число.
std::string fmt(double v, int d = -1);

class Scene {
 public:
  explicit Scene(const analysis::Model& model);  // модель должна жить дольше сцены

  const analysis::Model& model() const { return *model_; }
  std::size_t n_elems() const { return ix_.size(); }
  Vec3 size() const { return model_->size; }

  std::int64_t key(std::size_t e, int d) const { return flat_[e] * 6 + d; }
  // Элемент и направление по ключу грани; пусто — такого элемента нет.
  std::optional<std::pair<std::int32_t, int>> face_of(std::int64_t key) const;
  bool boundary(std::size_t e, int d) const { return nbr_[e * 6 + static_cast<std::size_t>(d)] < 0; }

  // Разрез: видны элементы с центром не дальше pos по оси axis (система детали). Пусто — без разреза.
  void set_section(std::optional<std::pair<int, double>> section);
  const std::vector<VisibleFace>& faces() const { return faces_; }

  // Результаты расчёта (nullptr — нет) и что показывать.
  void set_results(const analysis::AnalysisResult* results);
  const analysis::AnalysisResult* results() const { return results_; }
  void set_case(std::size_t c);
  std::size_t case_index() const { return case_; }
  const analysis::CaseResult* current_case() const;
  void set_target(double target_sf) { target_ = target_sf; }
  void set_field(Field f) { field_ = f; }
  Field field() const { return field_; }

  // Цвет каждой видимой грани с подсветкой (последняя подходящая подсветка сверху).
  std::vector<Rgb> face_colors(const std::vector<Overlay>& overlays) const;
  Legend legend() const;

  // Узел и углы грани в системе детали, с перемещениями × deform.
  Vec3 node_position(std::size_t node, double deform) const;
  void face_corners(std::size_t f, double deform, std::array<Vec3, 4>& out) const;
  double auto_deform_scale() const;

  // Выбор граней (ключи по возрастанию).
  std::vector<std::int64_t> flood_plane(std::int32_t e, int d) const;
  std::vector<std::int64_t> flood_axis(std::int32_t e, int d, int axis) const;
  std::vector<std::int64_t> hole_region(std::int32_t e, int d) const;
  std::vector<std::int64_t> brush(const Vec3& point, double radius) const;
  std::vector<std::int64_t> side(int dir) const;
  double area(const std::vector<std::int64_t>& keys) const;
  std::optional<Vec3> center(const std::vector<std::int64_t>& keys, double deform) const;
  Vec3 normal_sum(const std::vector<std::int64_t>& keys) const;

  // Перенос выбора между сетками разной детальности.
  std::vector<FacePoint> to_points(const std::vector<std::int64_t>& keys) const;
  std::vector<std::int64_t> from_points(const std::vector<FacePoint>& points) const;

  // Ближайшая видимая грань на луче (система детали).
  std::optional<Hit> pick(const Vec3& origin, const Vec3& dir, double deform) const;

  // Подсказка под курсором: координаты и значения в элементе (несколько строк).
  std::vector<std::string> describe(std::int32_t e) const;

  // Центр элемента в системе детали.
  Vec3 elem_center(std::size_t e) const { return center_[e]; }

 private:
  std::vector<std::int64_t> flood(std::int32_t e0, int d0, bool plane, int axis) const;
  std::pair<std::int32_t, int> step_face(std::int32_t e, int d, int t) const;
  double stress_max() const;
  Rgb elem_color(std::size_t e) const;

  const analysis::Model* model_;
  std::vector<std::int32_t> ix_, iy_, iz_;
  std::vector<std::int64_t> flat_;
  std::vector<std::int32_t> nbr_;  // n × 6
  std::vector<Vec3> center_;
  std::vector<Vec3> npos_;  // узлы в системе детали
  std::vector<std::uint8_t> role_;
  std::vector<double> rho_;

  std::vector<char> visible_;
  std::vector<VisibleFace> faces_;

  const analysis::AnalysisResult* results_ = nullptr;
  std::size_t case_ = 0;
  double target_ = 2.0;
  Field field_ = Field::Model;
  mutable std::vector<double> vm_max_;  // 99,5-й процентиль напряжений по случаям
};

}  // namespace kika::app
