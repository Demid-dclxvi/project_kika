#pragma once
// Общее для частей слайсера: координаты Clipper2, смещения контуров, штриховка, острова слоя.
// Внутренний заголовок — снаружи библиотеки не виден.

#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include <clipper2/clipper.h>

namespace kika::slicer::detail {

using Clipper2Lib::ClipType;
using Clipper2Lib::EndType;
using Clipper2Lib::FillRule;
using Clipper2Lib::JoinType;
using Clipper2Lib::Path64;
using Clipper2Lib::Paths64;
using Clipper2Lib::Point64;

constexpr double kScale = 10000.0;  // единиц Clipper в миллиметре (0,1 мкм)

inline std::int64_t q(double v) { return std::llround(v * kScale); }
inline double mm(std::int64_t v) { return static_cast<double>(v) / kScale; }

using Pt = std::array<double, 2>;
using Polyline = std::vector<Pt>;

inline double dist2(const Pt& a, const Pt& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]); }

// Смещение контуров (мм): плюс — наружу, минус — внутрь.
Paths64 offset(const Paths64& p, double delta_mm);
// «Открытие»: убрать перешейки и полоски уже 2·r.
Paths64 opening(const Paths64& p, double r_mm);
double area_mm2(const Paths64& p);
Polyline to_pts(const Path64& p);
Path64 to_path(const Polyline& p);

// Острова слоя: внешний контур со своими отверстиями.
std::vector<Paths64> islands(const Paths64& region);

// Параллельные линии с шагом spacing под углом angle (градусы) внутри области; phase — сдвиг
// по нормали к линиям. Линии привязаны к началу координат: на соседних слоях — друг над другом.
std::vector<Polyline> hatch(const Paths64& area, double spacing, double angle, double phase, double min_len);

// Обрезать открытые ломаные областью; куски короче min_len отбрасываются.
std::vector<Polyline> clip_open(const Paths64& area, const std::vector<Polyline>& lines, double min_len);

// Габарит области в повёрнутой системе: u — вдоль угла angle, v — поперёк.
struct Frame {
  double cs = 1, sn = 0;  // поворот: мир = R·(u, v)
  double umin = 0, umax = 0, vmin = 0, vmax = 0;
  Pt to_world(double u, double v) const { return {cs * u - sn * v, sn * u + cs * v}; }
};
Frame frame_of(const Paths64& area, double angle_deg);

}  // namespace kika::slicer::detail
