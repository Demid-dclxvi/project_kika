#include "kika/gcode/toolpaths.hpp"

#include <algorithm>
#include <limits>

namespace kika::gcode {

double Toolpaths::total_volume() const {
  double sum = 0.0;
  for (const Segment& s : segments) sum += s.volume;
  return sum;
}

BBox Toolpaths::bbox() const {
  BBox b;
  if (segments.empty()) return b;
  constexpr double inf = std::numeric_limits<double>::infinity();
  b.min = {inf, inf, inf};
  b.max = {-inf, -inf, -inf};
  for (const Segment& s : segments) {
    b.min[0] = std::min({b.min[0], s.x0, s.x1});
    b.min[1] = std::min({b.min[1], s.y0, s.y1});
    b.min[2] = std::min(b.min[2], s.z - s.h);
    b.max[0] = std::max({b.max[0], s.x0, s.x1});
    b.max[1] = std::max({b.max[1], s.y0, s.y1});
    b.max[2] = std::max(b.max[2], s.z);
  }
  return b;
}

}  // namespace kika::gcode
