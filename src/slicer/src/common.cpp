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

std::vector<HatchPiece> hatch_rows(const Paths64& area, double spacing, double angle, double phase, double min_len) {
  std::vector<HatchPiece> pieces;
  if (area.empty() || spacing <= 0) return pieces;
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
  if (lines.empty()) return pieces;
  Clipper2Lib::Clipper64 c;
  c.AddOpenSubject(lines);
  c.AddClip(area);
  Paths64 closed, open;
  c.Execute(ClipType::Intersection, FillRule::NonZero, closed, open);
  for (const auto& path : open) {
    if (path.size() < 2) continue;
    Polyline pts = to_pts(path);
    const double len = std::hypot(pts.back()[0] - pts.front()[0], pts.back()[1] - pts.front()[1]);
    if (len < min_len) continue;
    double s0 = pts.front()[0] * ux + pts.front()[1] * uy, s1 = pts.back()[0] * ux + pts.back()[1] * uy;
    if (s1 < s0) {
      std::reverse(pts.begin(), pts.end());
      std::swap(s0, s1);
    }
    const double t = pts.front()[0] * nx + pts.front()[1] * ny;
    pieces.push_back({std::llround((t - phase) / spacing), s0, s1, std::move(pts)});
  }
  std::sort(pieces.begin(), pieces.end(),
            [](const HatchPiece& x, const HatchPiece& y) { return x.k != y.k ? x.k < y.k : x.s0 < y.s0; });
  return pieces;
}

std::vector<Polyline> hatch(const Paths64& area, double spacing, double angle, double phase, double min_len) {
  // по порядку рядов; через ряд — в обратную сторону (и куски ряда — с другого конца)
  auto pieces = hatch_rows(area, spacing, angle, phase, min_len);
  std::vector<Polyline> out;
  out.reserve(pieces.size());
  for (std::size_t i = 0; i < pieces.size();) {
    std::size_t j = i;
    while (j < pieces.size() && pieces[j].k == pieces[i].k) ++j;
    if (pieces[i].k % 2 != 0) {
      for (std::size_t m = j; m-- > i;) {
        std::reverse(pieces[m].pts.begin(), pieces[m].pts.end());
        out.push_back(std::move(pieces[m].pts));
      }
    } else {
      for (std::size_t m = i; m < j; ++m) out.push_back(std::move(pieces[m].pts));
    }
    i = j;
  }
  return out;
}

std::vector<std::vector<Polyline>> monotonic_runs(std::vector<HatchPiece> pieces) {
  // участок растёт, пока у последнего куска в следующем ряду ровно один перекрывающийся сосед
  // и этот сосед перекрывает только его
  struct Run {
    std::vector<std::size_t> idx;
  };
  std::vector<Run> runs;
  std::vector<std::size_t> open_runs;  // участки, кончающиеся в предыдущем ряду
  auto overlap = [](const HatchPiece& a, const HatchPiece& b) { return a.s0 < b.s1 && b.s0 < a.s1; };
  std::size_t i = 0;
  long long prev_k = 0;
  bool have_prev = false;
  while (i < pieces.size()) {
    std::size_t j = i;
    while (j < pieces.size() && pieces[j].k == pieces[i].k) ++j;
    const bool adjacent = have_prev && pieces[i].k == prev_k + 1;
    std::vector<std::size_t> next_open;
    for (std::size_t m = i; m < j; ++m) {
      std::size_t found = runs.size();
      int count = 0;
      if (adjacent)
        for (std::size_t r : open_runs)
          if (overlap(pieces[runs[r].idx.back()], pieces[m])) {
            found = r;
            ++count;
          }
      bool joined = false;
      if (count == 1) {
        // у последнего куска участка в этом ряду должен быть один сосед
        int back = 0;
        for (std::size_t o = i; o < j; ++o) back += overlap(pieces[runs[found].idx.back()], pieces[o]) ? 1 : 0;
        if (back == 1) {
          runs[found].idx.push_back(m);
          next_open.push_back(found);
          joined = true;
        }
      }
      if (!joined) {
        runs.push_back({{m}});
        next_open.push_back(runs.size() - 1);
      }
    }
    open_runs = std::move(next_open);
    prev_k = pieces[i].k;
    have_prev = true;
    i = j;
  }
  std::vector<std::vector<Polyline>> out;
  out.reserve(runs.size());
  for (auto& r : runs) {
    std::vector<Polyline> lines;
    lines.reserve(r.idx.size());
    for (std::size_t n = 0; n < r.idx.size(); ++n) {
      Polyline pts = std::move(pieces[r.idx[n]].pts);
      if (n % 2 == 1) std::reverse(pts.begin(), pts.end());
      lines.push_back(std::move(pts));
    }
    out.push_back(std::move(lines));
  }
  return out;
}

bool contains(const Paths64& area, const Pt& p) {
  const Point64 pt(q(p[0]), q(p[1]));
  int n = 0;
  for (const auto& path : area)
    if (Clipper2Lib::PointInPolygon(pt, path) != Clipper2Lib::PointInPolygonResult::IsOutside) ++n;
  return n % 2 == 1;
}

namespace {

// Сдвинуть конец ломаной (последнюю точку, from_end) или начало внутрь на d, не дальше половины отрезка.
void pull_in(Polyline& p, bool from_end, double d) {
  if (p.size() < 2 || d <= 0) return;
  Pt& e = from_end ? p.back() : p.front();
  const Pt& n = from_end ? p[p.size() - 2] : p[1];
  const double len = std::sqrt(dist2(e, n));
  if (len < 1e-9) return;
  const double t = std::min(d, 0.5 * len) / len;
  e = {e[0] + (n[0] - e[0]) * t, e[1] + (n[1] - e[1]) * t};
}

}  // namespace

std::vector<Polyline> join_zigzag(std::vector<Polyline> lines, const Paths64& area, double max_gap, double trim) {
  std::vector<Polyline> out;
  for (auto& l : lines) {
    if (!out.empty() && l.size() >= 2) {
      Polyline& prev = out.back();
      const Pt a = prev.back(), b = l.front();
      if (dist2(a, b) <= max_gap * max_gap && contains(area, {(a[0] + b[0]) / 2, (a[1] + b[1]) / 2})) {
        pull_in(prev, true, trim);
        pull_in(l, false, trim);
        prev.insert(prev.end(), l.begin(), l.end());
        continue;
      }
    }
    out.push_back(std::move(l));
  }
  return out;
}

std::vector<Polyline> concentric_loops(const Paths64& area, double spacing, double min_area_mm2) {
  std::vector<Polyline> out;
  if (area.empty() || spacing <= 0) return out;
  for (int k = 0;; ++k) {
    const Paths64 loops = offset(area, -(spacing / 2 + k * spacing));
    if (loops.empty()) break;
    for (const auto& l : loops)
      if (l.size() >= 3 && std::abs(Clipper2Lib::Area(l)) > min_area_mm2 * kScale * kScale) out.push_back(to_pts(l));
  }
  return out;
}

Paths64 compensate_xy(const Paths64& region, double contour, double hole) {
  if (region.empty() || (contour == 0 && hole == 0)) return region;
  Paths64 out;
  for (const auto& isl : islands(region)) {
    Paths64 outer{isl[0]};
    if (contour != 0) outer = offset(outer, contour);
    Paths64 holes;
    for (std::size_t k = 1; k < isl.size(); ++k) {
      Path64 h = isl[k];
      if (!Clipper2Lib::IsPositive(h)) std::reverse(h.begin(), h.end());
      holes.push_back(std::move(h));
    }
    if (hole != 0 && !holes.empty()) holes = offset(holes, hole);
    const Paths64 part = holes.empty() ? outer : Clipper2Lib::Difference(outer, holes, FillRule::NonZero);
    out.insert(out.end(), part.begin(), part.end());
  }
  return Clipper2Lib::Union(out, FillRule::NonZero);
}

}  // namespace kika::slicer::detail
