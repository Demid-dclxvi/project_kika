// Нити детали для 3D-вида: ленты по отрезкам экструзии, цвет — из ячейки расчётной сетки.

#include "kika/app/roads.hpp"

#include <algorithm>
#include <cmath>

#include "kika/fem/element.hpp"

namespace kika::app {

namespace {

std::uint8_t byte_of(float c) { return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

std::int8_t snorm(double v) { return static_cast<std::int8_t>(std::lround(std::clamp(v, -1.0, 1.0) * 127.0)); }

}  // namespace

Roads::Roads(const analysis::Model& model, const Scene& scene) : model_(&model), scene_(&scene) {
  const auto& vm = model.vm;
  const auto& segs = model.tp.segments;
  const double line_width = model.tp.info.line_width.value_or(0.45);
  first_.push_back(0);
  std::vector<double> ts;
  std::vector<Piece> run;
  for (std::size_t i = 0; i < segs.size(); ++i) {
    const auto& s = segs[i];
    const double dx = s.x1 - s.x0, dy = s.y1 - s.y0;
    const double len = std::hypot(dx, dy);
    if (len < 1e-6 || s.h <= 0) continue;
    // ячейка по Z — по середине валика
    const double zc = s.z - 0.5 * s.h;
    const int iz = static_cast<int>(std::upper_bound(vm.z_edges.begin(), vm.z_edges.end(), zc) - vm.z_edges.begin()) - 1;
    // точки пересечения границ ячеек в плоскости слоя (доли длины отрезка)
    ts.assign({0.0, 1.0});
    auto crossings = [&](double a0, double a1, double origin, double step) {
      if (std::abs(a1 - a0) < 1e-12) return;
      const double lo = std::min(a0, a1), hi = std::max(a0, a1);
      for (double k = std::floor((lo - origin) / step) + 1; origin + k * step < hi; k += 1) {
        const double t = (origin + k * step - a0) / (a1 - a0);
        if (t > 0 && t < 1) ts.push_back(t);
      }
    };
    crossings(s.x0, s.x1, vm.x0, vm.sx);
    crossings(s.y0, s.y1, vm.y0, vm.sy);
    std::sort(ts.begin(), ts.end());
    run.clear();
    for (std::size_t j = 0; j + 1 < ts.size(); ++j) {
      if (ts[j + 1] - ts[j] < 1e-9) continue;
      const double tm = 0.5 * (ts[j] + ts[j + 1]);
      const int ix = static_cast<int>(std::floor((s.x0 + tm * dx - vm.x0) / vm.sx));
      const int iy = static_cast<int>(std::floor((s.y0 + tm * dy - vm.y0) / vm.sy));
      const std::int32_t e = scene.elem_at(ix, iy, iz);
      if (!run.empty() && run.back().elem == e)
        run.back().t1 = static_cast<float>(ts[j + 1]);
      else
        run.push_back({static_cast<float>(ts[j]), static_cast<float>(ts[j + 1]), e});
    }
    // пустые ячейки (тонкое место, край сетки) берут элемент соседнего куска;
    // отрезок целиком вне сетки — часть отброшенного куска, его не рисуем
    std::int32_t prev = -1;
    for (auto& p : run) {
      if (p.elem < 0) p.elem = prev;
      prev = p.elem;
    }
    std::int32_t next = -1;
    for (auto it = run.rbegin(); it != run.rend(); ++it) {
      if (it->elem < 0) it->elem = next;
      next = it->elem;
    }
    if (run.empty() || run.front().elem < 0) continue;
    // соседние куски одного элемента — одним куском
    std::size_t w = 0;
    for (std::size_t j = 0; j < run.size(); ++j) {
      if (w > 0 && run[w - 1].elem == run[j].elem)
        run[w - 1].t1 = run[j].t1;
      else
        run[w++] = run[j];
    }
    run.resize(w);
    run.front().t0 = 0.0f;
    run.back().t1 = 1.0f;
    seg_.push_back(static_cast<std::uint32_t>(i));
    pieces_.insert(pieces_.end(), run.begin(), run.end());
    first_.push_back(static_cast<std::uint32_t>(pieces_.size()));
    double width = s.volume > 0 ? s.volume / (len * s.h) : line_width;
    width_.push_back(static_cast<float>(std::clamp(width, 0.3 * s.h, 4.0 * s.h)));
  }
}

Vec3 Roads::displaced(const Vec3& p, std::int32_t elem, double deform) const {
  const auto* r = scene_->current_case();
  if (deform == 0.0 || !r || elem < 0) return p;
  const auto& vm = model_->vm;
  const auto e = static_cast<std::size_t>(elem);
  const Vec3& o = model_->origin;
  const auto iz = static_cast<std::size_t>(vm.iz[e]);
  // локальные координаты в ячейке 0…1
  const double u = std::clamp((p[0] + o[0] - (vm.x0 + vm.ix[e] * vm.sx)) / vm.sx, 0.0, 1.0);
  const double v = std::clamp((p[1] + o[1] - (vm.y0 + vm.iy[e] * vm.sy)) / vm.sy, 0.0, 1.0);
  const double w = std::clamp((p[2] + o[2] - vm.z_edges[iz]) / (vm.z_edges[iz + 1] - vm.z_edges[iz]), 0.0, 1.0);
  Vec3 out = p;
  const auto& en = model_->mesh.elem_nodes[e];
  for (std::size_t q = 0; q < 8; ++q) {
    const double k = (fem::kLocalNodes[q][0] > 0 ? u : 1 - u) * (fem::kLocalNodes[q][1] > 0 ? v : 1 - v) *
                     (fem::kLocalNodes[q][2] > 0 ? w : 1 - w);
    const auto n = static_cast<std::size_t>(en[q]) * 3;
    for (std::size_t a = 0; a < 3; ++a) out[a] += deform * k * r->u[n + a];
  }
  return out;
}

void Roads::build(RoadMesh& out, const std::vector<Overlay>& overlays, double deform) const {
  out.pos.clear();
  out.nrm.clear();
  out.col.clear();
  out.idx.clear();
  const auto& segs = model_->tp.segments;
  const Vec3& o = model_->origin;
  const std::size_t n = scene_->n_elems();
  const Field field = scene_->field();
  const bool by_elem = field != Field::Model && field != Field::Structure;
  std::vector<Rgb> ec;
  if (by_elem) {
    ec.resize(n);
    for (std::size_t e = 0; e < n; ++e) ec[e] = scene_->elem_color(e);
  }
  // подсветка: элементы, у которых подсвечена хотя бы одна грань (последняя подсветка сверху)
  std::vector<std::int16_t> ov;
  if (!overlays.empty()) {
    ov.assign(n, -1);
    for (std::size_t k = 0; k < overlays.size(); ++k)
      for (auto key : overlays[k].keys)
        if (const auto f = scene_->face_of(key)) ov[static_cast<std::size_t>(f->first)] = static_cast<std::int16_t>(k);
  }
  const auto& sec = scene_->section();
  const bool split = deform != 0.0;
  out.pos.reserve(pieces_.size() * 24);

  struct Ring {
    std::uint32_t base;
    Rgb color;
    float t;
  };
  for (std::size_t k = 0; k < seg_.size(); ++k) {
    const auto& s = segs[seg_[k]];
    const Vec3 a{s.x0 - o[0], s.y0 - o[1], s.z - 0.5 * s.h - o[2]};
    const Vec3 b{s.x1 - o[0], s.y1 - o[1], s.z - 0.5 * s.h - o[2]};
    const double dx = b[0] - a[0], dy = b[1] - a[1];
    const double len = std::hypot(dx, dy);
    const double ux = dx / len, uy = dy / len;
    const double hw = 0.5 * width_[k], hh = 0.5 * s.h;
    const Rgb seg_color = field == Field::Model ? palette::kPart : role_color(s.role);

    auto add_ring = [&](double t, std::int32_t elem, const Rgb& c, double ext) {
      const Vec3 p{a[0] + t * dx + ext * ux, a[1] + t * dy + ext * uy, a[2]};
      const Vec3 cp = displaced(p, elem, deform);
      const auto base = static_cast<std::uint32_t>(out.pos.size() / 3);
      const double off[4][3] = {{0, 0, hh}, {-uy * hw, ux * hw, 0}, {0, 0, -hh}, {uy * hw, -ux * hw, 0}};
      const double nrm[4][3] = {{0, 0, 1}, {-uy, ux, 0}, {0, 0, -1}, {uy, -ux, 0}};
      const std::uint8_t rgb[3] = {byte_of(c[0]), byte_of(c[1]), byte_of(c[2])};
      for (std::size_t q = 0; q < 4; ++q) {
        for (std::size_t i = 0; i < 3; ++i) {
          out.pos.push_back(static_cast<float>(cp[i] + off[q][i]));
          out.nrm.push_back(snorm(nrm[q][i]));
          out.col.push_back(rgb[i]);
        }
        out.nrm.push_back(0);
        out.col.push_back(255);
      }
      return base;
    };
    auto add_quads = [&](std::uint32_t ra, std::uint32_t rb) {
      for (std::uint32_t q = 0; q < 4; ++q) {
        const std::uint32_t q1 = (q + 1) % 4;
        out.idx.insert(out.idx.end(), {ra + q, ra + q1, rb + q1, ra + q, rb + q1, rb + q});
      }
    };

    // куски подряд с одним цветом — одной лентой; с деформацией — каждый кусок своей,
    // соседние ленты одного цвета делят общее кольцо вершин
    bool have = false;
    Ring last{};
    float run_t0 = 0, run_t1 = 0;
    std::int32_t run_e0 = -1, run_e1 = -1;
    Rgb run_c{};
    auto flush = [&]() {
      if (!have) return;
      const double ext0 = run_t0 <= 0.0f ? -hw : 0.0, ext1 = run_t1 >= 1.0f ? hw : 0.0;
      std::uint32_t r0;
      if (last.base != UINT32_MAX && last.t == run_t0 && last.color == run_c && ext0 == 0.0)
        r0 = last.base;
      else
        r0 = add_ring(run_t0, run_e0, run_c, ext0);
      const std::uint32_t r1 = add_ring(run_t1, run_e1, run_c, ext1);
      add_quads(r0, r1);
      last = {r1, run_c, run_t1};
      have = false;
    };
    last.base = UINT32_MAX;
    for (const Piece* p = pieces_begin(k); p != pieces_end(k); ++p) {
      const auto e = static_cast<std::size_t>(p->elem);
      if (sec) {
        const double tm = 0.5 * (p->t0 + p->t1);
        const double coord = sec->first == 2 ? a[2] : (sec->first == 0 ? a[0] + tm * dx : a[1] + tm * dy);
        if (coord > sec->second) {
          flush();
          last.base = UINT32_MAX;
          continue;
        }
      }
      Rgb c = by_elem ? ec[e] : seg_color;
      if (!ov.empty() && ov[e] >= 0) {
        const auto& lay = overlays[static_cast<std::size_t>(ov[e])];
        c = blend(c, lay.color, lay.alpha);
      }
      if (have && !split && c == run_c && run_t1 == p->t0) {
        run_t1 = p->t1;
        run_e1 = p->elem;
        continue;
      }
      flush();
      have = true;
      run_t0 = p->t0;
      run_t1 = p->t1;
      run_e0 = run_e1 = p->elem;
      run_c = c;
    }
    flush();
  }
}

}  // namespace kika::app
