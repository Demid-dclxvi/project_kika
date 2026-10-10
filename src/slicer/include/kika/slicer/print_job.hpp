#pragma once
// Нарезка модели по заданию: поле "print" задания на расчёт ↔ настройки слайсера,
// поворот детали и постановка её в центр стола.
//
// "print": {
//   "printer": "sparkx_i7", "filament": "PETG",          // пластик по умолчанию — материал задания
//   "layer_height": 0.2, "first_layer_height": 0.2, "line_width": 0.42,
//   "walls": 2, "top_layers": 5, "bottom_layers": 3,
//   "infill": 15, "pattern": "grid", "infill_angle": 45,  // заполнение в процентах
//   "rotate": [90, 0, 0],                                 // градусы вокруг X, затем Y, затем Z
//   "skirt": true
// }

#include <array>
#include <string>

#include "kika/geometry/mesh.hpp"
#include "kika/slicer/slicer.hpp"
#include "kika/util/json.hpp"

namespace kika::slicer {

struct PrintJob {
  Settings settings;
  std::array<double, 3> rotate{0, 0, 0};
};

// Настройки из поля "print" (nullptr — всё по умолчанию). material — материал задания: его тип
// пластика берётся, если "filament" не задан. Ошибки — analysis::JobError по-русски.
PrintJob print_from_json(const json::Value* print, const std::string& material);
json::Value print_to_json(const PrintJob& p);

// Поворот детали: матрица по углам rotate (сначала X, затем Y, затем Z) и обратно.
geometry::Transform rotation_of(const std::array<double, 3>& rotate);
std::array<double, 3> euler_xyz(const geometry::Transform& r);

// Поворот и постановка в центр стола: сетку после этого можно резать.
geometry::Transform placement(const geometry::TriangleMesh& mesh, const PrintJob& p);
geometry::TriangleMesh placed(const geometry::TriangleMesh& mesh, const PrintJob& p);

}  // namespace kika::slicer
