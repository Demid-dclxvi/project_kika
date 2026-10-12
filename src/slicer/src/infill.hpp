#pragma once
// Рисунки разреженного заполнения. Внутренний заголовок слайсера.

#include <cstddef>
#include <vector>

#include "common.hpp"
#include "kika/slicer/slicer.hpp"

namespace kika::slicer::detail {

struct InfillParams {
  Pattern pattern = Pattern::Grid;
  double density = 0.15;  // доля объёма 0…1
  double spacing = 0.38;  // шаг между соседними валиками при 100 %, мм (w − h·(1 − π/4))
  double line_width = 0.42;
  double angle = 45;      // градусы
  std::size_t layer = 0;  // номер слоя
  double z = 0;           // высота середины слоя, мм
};

struct FillPath {
  Polyline pts;
  bool closed = false;
};

// Линии заполнения внутри области. Плотность — по объёму: пластика в области — density её объёма.
std::vector<FillPath> sparse_infill(const Paths64& area, const InfillParams& p);

}  // namespace kika::slicer::detail
