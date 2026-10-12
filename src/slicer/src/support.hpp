#pragma once
// Поддержки: где они нужны и как их печатать. Внутренний заголовок слайсера.

#include <cstddef>
#include <vector>

#include "common.hpp"
#include "infill.hpp"
#include "kika/slicer/slicer.hpp"

namespace kika::slicer::detail {

// Нависания слоёв: часть слоя i, под которой на слое i − 1 нет детали даже с наклоном angle
// (градусы к горизонтали). Полоса возвращается целиком — от края нижнего слоя.
std::vector<Paths64> overhangs(const std::vector<Paths64>& regions, const std::vector<double>& heights, double angle);

struct SupportLayer {
  Paths64 base;       // основа (обычные) или ветви (деревья)
  Paths64 interface;  // плотные слои прямо под деталью
};

struct SupportPlan {
  std::vector<SupportLayer> layers;  // по слоям детали
  double unsupported_mm2 = 0;        // самое большое нависание, оставшееся без опоры
  double unsupported_z = 0;
};

SupportPlan plan_supports(const std::vector<Paths64>& regions, const std::vector<double>& mids,
                          const std::vector<double>& heights, const Settings& s);

// Линии поддержек одного слоя.
struct SupportLines {
  std::vector<FillPath> base, interface;
};
SupportLines support_lines(const SupportLayer& L, const Settings& s, std::size_t layer, double spacing);

}  // namespace kika::slicer::detail
