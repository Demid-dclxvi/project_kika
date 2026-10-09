#pragma once
// Выбор граней модели и приложение нагрузок и закреплений. Перенос fdmfea/loads.py.
//
// Координаты в задании по умолчанию — в системе детали: начало в минимальном углу
// габарита детали (мм), оси как у принтера (Z — вверх при печати).

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "kika/fem/mesh.hpp"

namespace kika::analysis {

using Vec3 = std::array<double, 3>;

inline constexpr double kGravity = 9810.0;  // мм/с²

// Ошибка в описании области или направления (по-русски, с названием нагрузки/закрепления).
class SelectionError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct Box {
  Vec3 lo{};
  Vec3 hi{};  // углы в любом порядке
};

// Одна область на поверхности детали.
struct Selection {
  enum class Kind { Faces, Side, Box, Sphere, Cylinder };
  Kind kind = Kind::Side;
  std::vector<std::int64_t> faces;  // Faces: устойчивые номера граней (Mesh::face_key)
  int axis = 2;                     // Side, Cylinder
  int sign = 1;                     // Side: −1 — сторона min, +1 — max
  double depth = 0.0;               // Side: глубина захвата от крайней грани, мм
  std::optional<Box> range_box;     // Side: только грани внутри рамки
  Box box;                          // Box
  std::optional<Vec3> normal;       // Box: только грани, смотрящие в эту сторону
  Vec3 center{};                    // Sphere
  std::array<double, 2> center2{};  // Cylinder: центр в плоскости, перпендикулярной оси
  double radius = 0.0;              // Sphere, Cylinder
  bool outer = false;               // Cylinder: наружная поверхность (бобышка), а не отверстие
  std::optional<std::array<double, 2>> range;  // Cylinder: от и до вдоль оси
};
// Область — объединение нескольких описаний.
using Region = std::vector<Selection>;

// Направление: вектор или строка "+x", "-z", "normal_in", "normal_out" (по средней нормали граней).
struct Direction {
  std::optional<Vec3> vec;
  std::string name = "-z";
};

// Номера граничных граней (по возрастанию) в области. name — для сообщений об ошибках.
std::vector<std::int64_t> select_faces(const fem::Mesh& mesh, const Region& region, const Vec3& origin,
                                       const std::string& name);

// Узлы граней и их «площади» (по четверти площади от каждой грани).
struct NodeWeights {
  std::vector<std::int64_t> nodes;  // по возрастанию
  std::vector<double> w;
};
NodeWeights face_node_weights(const fem::Mesh& mesh, std::span<const std::int64_t> faces);

// Единичный вектор направления. Для normal_in/normal_out нужны грани.
Vec3 resolve_direction(const Direction& d, const fem::Mesh* mesh = nullptr,
                       std::span<const std::int64_t> faces = {});

// Распределение нагрузок по узлам граней; f_out — вектор сил 3N, к нему прибавляется.
void distribute_force(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& force,
                      std::vector<double>& f_out);
void distribute_moment(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& moment,
                       std::vector<double>& f_out, const std::optional<Vec3>& center = std::nullopt);
void distribute_pressure(const fem::Mesh& mesh, std::span<const std::int64_t> faces, double pressure,
                         std::vector<double>& f_out);
// Нагрузка от болта или оси в отверстии: давит только на грани, обращённые против силы.
void distribute_bearing(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& force,
                        std::vector<double>& f_out);

}  // namespace kika::analysis
