// Общая геометрия слайсера на Clipper2.

#include "common.hpp"

#include <algorithm>
#include <numbers>

namespace kika::slicer::detail {

Paths64 offset(const Paths64& p, double delta_mm) {
  if (p.empty()) return {};
  return Clipper2Lib::InflatePaths(p, delta_mm * kScale, JoinType::Miter, EndType::Polygon, 3.0);
}

Paths64 opening(const Paths64& p, double r_mm) {
  if (p.empty()) return {};
  return offset(offset(p, -r_mm), r_mm);
}

double area_mm2(const Paths64& p) { return Clipper2Lib::Area(p) / (kScale * kScale); }

Polyline to_pts(const Path64& p) {
  Polyline v;
  v.reserve(p.size());
  for (const auto& x : p) v.push_back({mm(x.x), mm(x.y)});
  return v;
}

Path64 to_path(const Polyline& p) {
  Path64 v;
  v.reserve(p.size());
  for (const auto& x : p) v.push_back(Point64(q(x[0]), q(x[1])));
  return v;
}

std::vector<Paths64> islands(const Paths64& region) {
  std::vector<Paths64> out;
  if (region.empty()) return out;
  Clipper2Lib::PolyTree64 tree;
  Clipper2Lib::Clipper64 c;
  c.AddSubject(region);
  c.Execute(ClipType::Union, FillRule::NonZero, tree);
  std::vector<const Clipper2Lib::PolyPath64*> stack;
  for (const auto& child : tree) stack.push_back(child.get());
  while (!stack.empty()) {
    const auto* node = stack.back();
    stack.pop_back();
    Paths64 isl{node->Polygon()};
    for (const auto& hole : *node) {
      isl.push_back(hole->Polygon());
      for (const auto& inner : *hole) stack.push_back(inner.get());  // остров внутри отверстия
    }
    out.push_back(std::move(isl));
  }
  return out;
}

Frame frame_of(const Paths64& area, double angle_deg) {
  Frame f;
  const double a = angle_deg * std::numbers::pi / 180.0;
  f.cs = std::cos(a);
  f.sn = std::sin(a);
  f.umin = f.vmin = 1e300;
  f.umax = f.vmax = -1e300;
  for (const auto& path : area)
    for (const auto& p : path) {
      const double x = mm(p.x), y = mm(p.y);
      const double u = f.cs * x + f.sn * y, v = -f.sn * x + f.cs * y;
      f.umin = std::min(f.umin, u);
      f.umax = std::max(f.umax, u);
      f.vmin = std::min(f.vmin, v);
      f.vmax = std::max(f.vmax, v);
    }
  return f;
}

namespace {

double length(const Polyline& p) {
  double l = 0;
  for (std::size_t i = 1; i < p.size(); ++i) l += std::hypot(p[i][0] - p[i - 1][0], p[i][1] - p[i - 1][1]);
  return l;
}

}  // namespace

std::vector<Polyline> clip_open(const Paths64& area, const std::vector<Polyline>& lines, double min_len) {
  std::vector<Polyline> out;
  if (area.empty() || lines.empty()) return out;
  Paths64 subj;
  subj.reserve(lines.size());
  for (const auto& l : lines)
    if (l.size() >= 2) subj.push_back(to_path(l));
  Clipper2Lib::Clipper64 c;
  c.AddOpenSubject(subj);
  c.AddClip(area);
  Paths64 closed, open;
  c.Execute(ClipType::Intersection, FillRule::NonZero, closed, open);
  for (const auto& path : open) {
    if (path.size() < 2) continue;
    Polyline pts = to_pts(path);
    if (length(pts) >= min_len) out.push_back(std::move(pts));
  }
  return out;
}

std::vector<Polyline> hatch(const Paths64& area, double spacing, double angle, double phase, double min_len) {
  std::vector<Polyline> out;
  if (area.empty() || spacing <= 0) return out;
  const double a = angle * std::numbers::pi / 180.0;
  const double ux = std::cos(a), uy = std::sin(a), nx = -uy, ny = ux;
  double tmin = 1e300, tmax = -1e300, smin = 1e300, smax = -1e300;
  for (const auto& path : area)
    for (const auto& p : path) {
      const double x = mm(p.x), y = mm(p.y);
      tmin = std::min(tmin, x * nx + y * ny);
      tmax = std::max(tmax, x * nx + y * ny);
      smin = std::min(smin, x * ux + y * uy);
      smax = std::max(smax, x * ux + y * uy);
    }
  Paths64 lines;
  for (double k = std::ceil((tmin - phase) / spacing); phase + k * spacing <= tmax; k += 1) {
    const double t = phase + k * spacing;
    lines.push_back({Point64(q(nx * t + ux * (smin - 1)), q(ny * t + uy * (smin - 1))),
                     Point64(q(nx * t + ux * (smax + 1)), q(ny * t + uy * (smax + 1)))});
  }
  if (lines.empty()) return out;
  Clipper2Lib::Clipper64 c;
  c.AddOpenSubject(lines);
  c.AddClip(area);
  Paths64 closed, open;
  c.Execute(ClipType::Intersection, FillRule::NonZero, closed, open);
  // по порядку: номер линии, затем положение вдоль неё; через одну — в обратную сторону
  struct Piece {
    long long k;
    double s0;
    Polyline pts;
  };
  std::vector<Piece> pieces;
  for (const auto& path : open) {
    if (path.size() < 2) continue;
    Polyline pts = to_pts(path);
    const double len = std::hypot(pts.back()[0] - pts.front()[0], pts.back()[1] - pts.front()[1]);
    if (len < min_len) continue;
    const double s0 = pts.front()[0] * ux + pts.front()[1] * uy, s1 = pts.back()[0] * ux + pts.back()[1] * uy;
    if (s1 < s0) std::reverse(pts.begin(), pts.end());
    const double t = pts.front()[0] * nx + pts.front()[1] * ny;
    pieces.push_back({std::llround((t - phase) / spacing), std::min(s0, s1), std::move(pts)});
  }
  std::sort(pieces.begin(), pieces.end(), [](const Piece& x, const Piece& y) { return x.k != y.k ? x.k < y.k : x.s0 < y.s0; });
  for (auto& p : pieces) {
    if (p.k % 2 != 0) std::reverse(p.pts.begin(), p.pts.end());
    out.push_back(std::move(p.pts));
  }
  return out;
}

}  // namespace kika::slicer::detail
