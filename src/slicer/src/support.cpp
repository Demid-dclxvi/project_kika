// Поддержки — свои алгоритмы (код AGPL-слайсеров не используется).
//
// Нависание слоя — часть, под которой на слое ниже нет детали даже с допустимым наклоном.
// Обычные поддержки: область под нависанием спускается вниз до стола (или до детали), отступая
// от стенок детали на зазор XY и не подходя к детали ближе зазора Z. Верхние слои под деталью —
// плотные (контактные), остальное — редкие линии.
// Древовидные: под нависанием на сетке с шагом ~3 мм ставятся кончики ветвей; ветви растут
// вниз, на каждом слое сдвигаясь не больше чем на h·tg 40°, тянутся к соседям и сливаются,
// толстеют к низу, обходят деталь; ветвь, упёршаяся в деталь, на ней и стоит. Над кончиками —
// контактные слои по площади нависания.

#include "support.hpp"

#include <algorithm>
#include <map>
#include <numbers>

namespace kika::slicer::detail {

namespace {

constexpr double kPi = std::numbers::pi;

Paths64 unite(const Paths64& a, const Paths64& b) {
  if (a.empty()) return b;
  if (b.empty()) return a;
  return Clipper2Lib::Union(a, b, FillRule::NonZero);
}
Paths64 minus(const Paths64& a, const Paths64& b) {
  if (a.empty() || b.empty()) return a;
  return Clipper2Lib::Difference(a, b, FillRule::NonZero);
}
Paths64 inter(const Paths64& a, const Paths64& b) {
  if (a.empty() || b.empty()) return {};
  return Clipper2Lib::Intersect(a, b, FillRule::NonZero);
}
// Убрать лишние точки, накопленные при объединениях слой за слоем.
Paths64 tidy(const Paths64& p) {
  if (p.empty()) return p;
  return Clipper2Lib::SimplifyPaths(p, 0.005 * kScale, true);
}

// Ближайшая точка границы области.
Pt nearest_boundary(const Paths64& area, const Pt& p, double& dist) {
  Pt best = p;
  double bd = 1e300;
  for (const auto& path : area)
    for (std::size_t k = 0; k < path.size(); ++k) {
      const Pt a{mm(path[k].x), mm(path[k].y)};
      const Pt b{mm(path[(k + 1) % path.size()].x), mm(path[(k + 1) % path.size()].y)};
      const double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
      double t = l2 > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2 : 0;
      t = std::clamp(t, 0.0, 1.0);
      const Pt c{a[0] + t * dx, a[1] + t * dy};
      const double d = dist2(c, p);
      if (d < bd) {
        bd = d;
        best = c;
      }
    }
  dist = std::sqrt(bd);
  return best;
}

Path64 circle(const Pt& c, double r) {
  const int seg = std::clamp(static_cast<int>(2 * kPi * r / 0.4), 12, 64);
  Path64 p;
  p.reserve(static_cast<std::size_t>(seg));
  for (int k = 0; k < seg; ++k) {
    const double a = 2 * kPi * k / seg;
    p.push_back(Point64(q(c[0] + r * std::cos(a)), q(c[1] + r * std::sin(a))));
  }
  return p;
}

// Общие данные для обоих видов поддержек.
struct Context {
  Context(const std::vector<Paths64>& r, const Settings& st) : regions(r), s(st), n(r.size()) {}
  const std::vector<Paths64>& regions;
  const Settings& s;
  std::size_t n;
  int gap = 1;   // слоёв зазора по Z
  int iface = 2; // контактных слоёв
  std::vector<Paths64> block;   // деталь с зазором XY
  std::vector<Paths64> shadow;  // всё, что ниже слоя (только «от стола»)
  Paths64 empty;

  const Paths64& R(long long k) const {
    return (k >= 0 && static_cast<std::size_t>(k) < n) ? regions[static_cast<std::size_t>(k)] : empty;
  }
  // Куда поддержке на слое j нельзя: деталь с зазором XY, деталь выше и ниже в пределах зазора Z.
  Paths64 cut(std::size_t j) const {
    Paths64 c = block[j];
    for (int d = 1; d <= gap; ++d) {
      c = unite(c, R(static_cast<long long>(j) + d));
      c = unite(c, R(static_cast<long long>(j) - d));
    }
    if (!shadow.empty()) c = unite(c, shadow[j]);
    return c;
  }
};

void plan_normal(const Context& c, const std::vector<Paths64>& O, SupportPlan& plan) {
  Paths64 S;  // поддержка слоя выше
  for (std::size_t j = c.n - 1; j-- > 0;) {
    Paths64 need = S;
    const std::size_t i = j + 1 + static_cast<std::size_t>(c.gap);
    if (i < c.n) need = unite(need, O[i]);
    if (need.empty()) {
      S.clear();
      continue;
    }
    Paths64 cur = tidy(minus(need, c.cut(j)));
    Paths64 above;
    for (int k = 1; k <= c.iface; ++k) above = unite(above, c.R(static_cast<long long>(j) + c.gap + k));
    Paths64 I = inter(cur, above);
    plan.layers[j] = {minus(cur, I), std::move(I)};
    S = std::move(cur);
  }
}

void plan_tree(const Context& c, const std::vector<Paths64>& O, const std::vector<double>& heights, SupportPlan& plan) {
  const Settings& s = c.s;
  const double tip_r = std::max(0.8, s.line_width * 1.6), max_r = 4.0, tip_step = 3.0;
  const double branch_tan = std::tan(40.0 * kPi / 180.0), grow_tan = std::tan(5.0 * kPi / 180.0);
  struct Node {
    Pt p;
    double r;
    int id;
  };
  struct Circle {
    Pt p;
    double r;
    int id;
  };
  std::vector<std::vector<Circle>> circles(c.n);
  std::vector<std::vector<int>> parents;  // откуда собран узел (после слияния)
  std::vector<char> dead;
  auto new_id = [&](std::vector<int> from) {
    parents.push_back(std::move(from));
    dead.push_back(0);
    return static_cast<int>(parents.size() - 1);
  };
  // где ветви радиуса r на слое j нельзя: деталь (и зазор по Z под ней) с зазором XY + r
  std::map<std::pair<std::size_t, int>, Paths64> col_cache;
  auto collision = [&](std::size_t j, double r) -> const Paths64& {
    const int bucket = static_cast<int>(std::ceil(r / 0.4));
    auto [it, fresh] = col_cache.try_emplace({j, bucket});
    if (fresh) {
      Paths64 a = c.R(static_cast<long long>(j));
      for (int d = 1; d <= c.gap; ++d) a = unite(a, c.R(static_cast<long long>(j) - d));
      if (!c.shadow.empty()) a = unite(a, c.shadow[j]);
      it->second = offset(a, s.support_xy_gap + bucket * 0.4);
    }
    return it->second;
  };
  std::vector<Node> nodes;
  for (std::size_t j = c.n; j-- > 0;) {
    const double h = heights[j];
    const double move = h * branch_tan;
    // 1. ветви с верхнего слоя: толще, тянутся к соседям
    for (auto& nd : nodes) nd.r = std::min(max_r, nd.r + h * grow_tan);
    std::vector<Pt> target(nodes.size());
    for (std::size_t a = 0; a < nodes.size(); ++a) {
      target[a] = nodes[a].p;
      double best = 1e300;
      std::size_t bi = a;
      for (std::size_t b = 0; b < nodes.size(); ++b) {
        if (b == a) continue;
        const double d = dist2(nodes[a].p, nodes[b].p);
        if (d < best) {
          best = d;
          bi = b;
        }
      }
      const double d = std::sqrt(best);
      const double reach = nodes[a].r + (bi != a ? nodes[bi].r : 0) + 2 * tip_step;
      if (bi != a && d < reach && d > 1e-9) {
        const double step = std::min(move, d / 2);
        target[a] = {nodes[a].p[0] + (nodes[bi].p[0] - nodes[a].p[0]) / d * step,
                     nodes[a].p[1] + (nodes[bi].p[1] - nodes[a].p[1]) / d * step};
      }
    }
    // 2. обход детали; ветвь, которой некуда деться, стоит на детали (или, «от стола», гибнет)
    std::vector<Node> moved;
    for (std::size_t a = 0; a < nodes.size(); ++a) {
      Node nd = nodes[a];
      nd.p = target[a];
      const Paths64& col = collision(j, nd.r);
      if (contains(col, nd.p)) {
        double d = 0;
        const Pt e = nearest_boundary(col, nd.p, d);
        if (d <= 3 * move) {
          const double k = d > 1e-9 ? (d + 0.02) / d : 1;
          nd.p = {nd.p[0] + (e[0] - nd.p[0]) * k, nd.p[1] + (e[1] - nd.p[1]) * k};
        } else {
          if (s.support_on_bed_only) dead[static_cast<std::size_t>(nd.id)] = 1;
          continue;
        }
      }
      moved.push_back(nd);
    }
    nodes = std::move(moved);
    // 3. новые кончики под нависаниями (над ними — контактные слои)
    const std::size_t i = j + 1 + static_cast<std::size_t>(c.gap + c.iface);
    if (i < c.n && !O[i].empty()) {
      for (const auto& isl : islands(O[i])) {
        const Frame f = frame_of(isl, 0);
        std::vector<Pt> cand;
        for (double x = std::ceil(f.umin / tip_step) * tip_step; x <= f.umax; x += tip_step)
          for (double y = std::ceil(f.vmin / tip_step) * tip_step; y <= f.vmax; y += tip_step)
            if (contains(isl, {x, y})) cand.push_back({x, y});
        if (cand.empty()) {  // узкий кусок: точка внутри него
          const Paths64 core = offset(isl, -0.05);
          const Paths64& src = core.empty() ? isl : core;
          if (!src.empty() && !src[0].empty()) cand.push_back({mm(src[0][0].x), mm(src[0][0].y)});
        }
        for (const Pt& p : cand) {
          bool near = false;
          for (const auto& nd : nodes) near = near || dist2(nd.p, p) < (0.5 * tip_step) * (0.5 * tip_step);
          if (near || contains(collision(j, tip_r), p)) continue;
          nodes.push_back({p, tip_r, new_id({})});
        }
      }
    }
    // 4. слияние близких ветвей
    for (bool merged = true; merged;) {
      merged = false;
      for (std::size_t a = 0; a < nodes.size() && !merged; ++a)
        for (std::size_t b = a + 1; b < nodes.size() && !merged; ++b) {
          const double ra = nodes[a].r, rb = nodes[b].r;
          if (dist2(nodes[a].p, nodes[b].p) < std::pow(0.75 * std::max(ra, rb), 2)) {
            const double wa = ra * ra, wb = rb * rb;
            Node m{{(nodes[a].p[0] * wa + nodes[b].p[0] * wb) / (wa + wb), (nodes[a].p[1] * wa + nodes[b].p[1] * wb) / (wa + wb)},
                   std::min(max_r, std::max(ra, rb) + 0.1), new_id({nodes[a].id, nodes[b].id})};
            nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(b));
            nodes[a] = m;
            merged = true;
          }
        }
    }
    for (const auto& nd : nodes) circles[j].push_back({nd.p, nd.r, nd.id});
  }
  // погибшие ветви — вместе со всем, из чего они собраны
  std::vector<int> stack;
  for (std::size_t id = 0; id < dead.size(); ++id)
    if (dead[id]) stack.push_back(static_cast<int>(id));
  while (!stack.empty()) {
    const int id = stack.back();
    stack.pop_back();
    for (int p : parents[static_cast<std::size_t>(id)])
      if (!dead[static_cast<std::size_t>(p)]) {
        dead[static_cast<std::size_t>(p)] = 1;
        stack.push_back(p);
      }
  }
  // контактные слои: площадь нависания на iface слоях над кончиками
  std::vector<Paths64> iface(c.n);
  for (std::size_t i = 1; i < c.n; ++i) {
    if (O[i].empty()) continue;
    for (int k = 0; k < c.iface; ++k) {
      const long long j = static_cast<long long>(i) - 1 - c.gap - k;
      if (j >= 0) iface[static_cast<std::size_t>(j)] = unite(iface[static_cast<std::size_t>(j)], O[i]);
    }
  }
  for (std::size_t j = 0; j < c.n; ++j) {
    Paths64 area;
    for (const auto& ci : circles[j]) {
      if (dead[static_cast<std::size_t>(ci.id)]) continue;
      // у стола ветви шире — лучше держатся
      area.push_back(circle(ci.p, ci.r + (j < 3 ? 1.0 : 0.0)));
    }
    if (!area.empty()) area = Clipper2Lib::Union(area, FillRule::NonZero);
    const Paths64 cut = c.cut(j);
    Paths64 I = minus(iface[j], cut);
    plan.layers[j] = {tidy(minus(minus(area, cut), I)), tidy(I)};
  }
}

}  // namespace

std::vector<Paths64> overhangs(const std::vector<Paths64>& regions, const std::vector<double>& heights, double angle) {
  std::vector<Paths64> O(regions.size());
  const double tan_a = std::tan(std::clamp(angle, 5.0, 85.0) * kPi / 180.0);
  for (std::size_t i = 1; i < regions.size(); ++i) {
    if (regions[i].empty()) continue;
    const double allow = heights[i] / tan_a;
    Paths64 raw = opening(minus(regions[i], offset(regions[i - 1], allow)), 0.02);
    if (raw.empty()) continue;
    O[i] = inter(offset(raw, allow + 0.02), regions[i]);
  }
  return O;
}

SupportPlan plan_supports(const std::vector<Paths64>& regions, const std::vector<double>& mids,
                          const std::vector<double>& heights, const Settings& s) {
  SupportPlan plan;
  const std::size_t n = regions.size();
  plan.layers.resize(n);
  if (s.support == Support::None || n < 2) return plan;
  const auto O = overhangs(regions, heights, s.support_angle);
  Context c(regions, s);
  c.gap = s.support_z_gap <= 0 ? 0 : std::max(1, static_cast<int>(std::lround(s.support_z_gap / s.layer_height)));
  c.iface = std::max(0, s.support_interface_layers);
  c.block.resize(n);
  for (std::size_t j = 0; j < n; ++j) c.block[j] = offset(regions[j], s.support_xy_gap);
  if (s.support_on_bed_only) {
    c.shadow.resize(n);
    Paths64 acc;
    for (std::size_t j = 0; j < n; ++j) {
      c.shadow[j] = offset(acc, s.support_xy_gap);
      acc = tidy(unite(acc, regions[j]));
    }
  }
  if (s.support == Support::Normal)
    plan_normal(c, O, plan);
  else
    plan_tree(c, O, heights, plan);
  // что осталось без опоры: нависание, под которым через зазор нет поддержки
  for (std::size_t i = 1; i < n; ++i) {
    if (O[i].empty()) continue;
    const long long j = static_cast<long long>(i) - 1 - c.gap;
    if (j < 0) continue;
    const auto& L = plan.layers[static_cast<std::size_t>(j)];
    const Paths64 sup = unite(L.base, L.interface);
    const Paths64 left = opening(minus(O[i], offset(sup, s.support_xy_gap + s.line_width)), 0.3);
    const double a = area_mm2(left);
    if (a > plan.unsupported_mm2) {
      plan.unsupported_mm2 = a;
      plan.unsupported_z = mids[i];
    }
  }
  return plan;
}

SupportLines support_lines(const SupportLayer& L, const Settings& s, std::size_t layer, double spacing, double width) {
  SupportLines out;
  const double w = width;
  if (!L.interface.empty()) {
    const Paths64 a = opening(L.interface, 0.45 * w);
    const double step = std::max(s.support_interface_spacing, spacing);
    for (auto& l : join_zigzag(hatch(a, step, layer % 2 == 0 ? 0.0 : 90.0, 0, w), a, 2 * step))
      out.interface.push_back({std::move(l), false});
  }
  if (!L.base.empty()) {
    const Paths64 a = opening(L.base, 0.45 * w);
    if (s.support == Support::Tree) {
      // ветвь — обвод по краю; толстый ствол — ещё и редкие линии внутри
      for (const auto& isl : islands(a)) {
        for (const auto& p : offset(isl, -w / 2))
          if (p.size() >= 3) out.base.push_back({to_pts(p), true});
        const Paths64 core = offset(isl, -w);
        if (area_mm2(core) > 4 * s.support_spacing * s.support_spacing)
          for (auto& l : join_zigzag(hatch(core, s.support_spacing, 0, 0, w), core, 1.5 * s.support_spacing))
            out.base.push_back({std::move(l), false});
      }
    } else {
      for (auto& l : join_zigzag(hatch(a, s.support_spacing, 0, 0, w), a, 1.5 * s.support_spacing))
        out.base.push_back({std::move(l), false});
    }
  }
  return out;
}

}  // namespace kika::slicer::detail
