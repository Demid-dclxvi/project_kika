#pragma once
// Нарезка модели по заданию: поле "print" задания на расчёт ↔ настройки слайсера,
// поворот и масштаб детали, постановка её на стол.
//
// "print": {
//   "printer": "qidi_q2",                                // или {"base": "qidi_q2", "z_hop": 0.2, …}
//   "filament": "PETG",                                   // по умолчанию — материал задания;
//                                                         // или {"base": "PETG", "nozzle_temp": 245, …}
//   "layer_height": 0.2, "first_layer_height": 0.2, "line_width": 0.42,
//   "walls": 2, "top_layers": 5, "bottom_layers": 3,
//   "infill": 15, "pattern": "grid", "infill_angle": 45,  // заполнение в процентах
//   "rotate": [90, 0, 0],                                 // градусы вокруг X, затем Y, затем Z
//   "scale": 1.5 или [1, 1, 2],                           // масштаб по осям стола (после поворота)
//   "position": [135, 135],                               // центр детали на столе, мм (по умолчанию — центр стола)
//   "support": "none",                                    // none, normal (обычные), tree (древовидные)
//   …и любые другие настройки печати из таблицы profiles.hpp (скорости, ускорения, кайма, шов…)
// }

#include <array>
#include <optional>
#include <string>

#include "kika/geometry/mesh.hpp"
#include "kika/slicer/slicer.hpp"
#include "kika/util/json.hpp"

namespace kika::slicer {

struct PrintJob {
  Settings settings;
  std::array<double, 3> rotate{0, 0, 0};
  std::array<double, 3> scale{1, 1, 1};           // по осям стола, после поворота
  std::optional<std::array<double, 2>> position;  // центр габарита детали на столе, мм; пусто — центр стола
};

// Настройки из поля "print" (nullptr — всё по умолчанию). material — материал задания: его тип
// пластика берётся, если "filament" не задан. Ошибки — analysis::JobError по-русски.
PrintJob print_from_json(const json::Value* print, const std::string& material);
json::Value print_to_json(const PrintJob& p);

// Поворот детали: матрица по углам rotate (сначала X, затем Y, затем Z) и обратно.
geometry::Transform rotation_of(const std::array<double, 3>& rotate);
std::array<double, 3> euler_xyz(const geometry::Transform& r);
// Поворот детали из положения from в положение to (оба угла — от модели в файле):
// R(to)·R(from)ᵀ, без сдвига.
geometry::Transform turn_between(const std::array<double, 3>& from, const std::array<double, 3>& to);

// Линейная часть постановки: масштаб × поворот, без сдвига.
geometry::Transform linear_of(const PrintJob& p);
// Деталь из постановки from в постановку to (без сдвига): L(to)·L(from)⁻¹ — для переноса точек
// детали; направления (силы, оси) переносятся только поворотом turn_between(from.rotate, to.rotate).
geometry::Transform change_between(const PrintJob& from, const PrintJob& to);
// Центр детали на столе: position или центр стола.
std::array<double, 2> bed_position(const PrintJob& p);

// Поворот, масштаб и постановка на стол (низ на Z = 0, центр габарита — в bed_position):
// сетку после этого можно резать.
geometry::Transform placement(const geometry::TriangleMesh& mesh, const PrintJob& p);
geometry::TriangleMesh placed(const geometry::TriangleMesh& mesh, const PrintJob& p);

}  // namespace kika::slicer
