// Перенос fdmfea/loads.py.

#include "kika/analysis/loads.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

#include "util/text.hpp"

namespace kika::analysis {

namespace {

bool in_box(const Vec3& p, const Box& b) {
  for (std::size_t a = 0; a < 3; ++a) {
    const double lo = std::min(b.lo[a], b.hi[a]);
    const double hi = std::max(b.lo[a], b.hi[a]);
    if (!(p[a] >= lo - 1e-9 && p[a] <= hi + 1e-9)) return false;
  }
  return true;
}

Vec3 rel_center(const fem::Mesh& mesh, std::size_t f, const Vec3& origin) {
  const auto& c = mesh.face_center[f];
  return {c[0] - origin[0], c[1] - origin[1], c[2] - origin[2]};
}

std::vector<std::int64_t> select_one(const fem::Mesh& mesh, const Selection& sel, const Vec3& origin,
                                     const std::string& name) {
  const std::size_t nf = mesh.n_faces();
  const double s = mesh.s;
  const double tol = 0.51 * s;
  std::vector<char> m(nf, 0);
  switch (sel.kind) {
    case Selection::Kind::Faces: {
      std::vector<std::int64_t> keys = sel.faces;
      std::sort(keys.begin(), keys.end());
      for (std::size_t f = 0; f < nf; ++f) m[f] = std::binary_search(keys.begin(), keys.end(), mesh.face_key[f]);
      break;
    }
    case Selection::Kind::Side: {
      const auto ax = static_cast<std::size_t>(sel.axis);
      const double sg = sel.sign;
      const double depth = sel.depth + tol;
      double ext = -HUGE_VAL;
      bool any = false;
      for (std::size_t f = 0; f < nf; ++f)
        if (mesh.face_normal(f)[ax] * sg > 0.5) {
          any = true;
          ext = std::max(ext, rel_center(mesh, f, origin)[ax] * sg);
        }
      if (any)
        for (std::size_t f = 0; f < nf; ++f)
          m[f] = mesh.face_normal(f)[ax] * sg > 0.5 && rel_center(mesh, f, origin)[ax] * sg >= ext - depth;
      if (sel.range_box)
        for (std::size_t f = 0; f < nf; ++f) m[f] = m[f] && in_box(rel_center(mesh, f, origin), *sel.range_box);
      break;
    }
    case Selection::Kind::Box:
      for (std::size_t f = 0; f < nf; ++f) {
        bool ok = in_box(rel_center(mesh, f, origin), sel.box);
        if (ok && sel.normal) {
          const auto n = mesh.face_normal(f);
          const auto& v = *sel.normal;
          ok = n[0] * v[0] + n[1] * v[1] + n[2] * v[2] > 0.5;
        }
        m[f] = ok;
      }
      break;
    case Selection::Kind::Sphere:
      for (std::size_t f = 0; f < nf; ++f) {
        const auto c = rel_center(mesh, f, origin);
        const double d = std::hypot(c[0] - sel.center[0], c[1] - sel.center[1], c[2] - sel.center[2]);
        m[f] = d <= sel.radius;
      }
      break;
    case Selection::Kind::Cylinder: {
      const auto ax = static_cast<std::size_t>(sel.axis);
      const std::size_t o0 = ax == 0 ? 1 : 0;
      const std::size_t o1 = ax == 2 ? 1 : 2;
      for (std::size_t f = 0; f < nf; ++f) {
        const auto c = rel_center(mesh, f, origin);
        const double dist = std::hypot(c[o0] - sel.center2[0], c[o1] - sel.center2[1]);
        const bool side_face = std::abs(mesh.face_normal(f)[ax]) < 0.5;
        bool ok;
        if (sel.outer)
          ok = side_face && dist >= sel.radius - 1.2 * s && dist <= sel.radius + 1.2 * s;
        else
          ok = side_face && dist <= sel.radius + 1.0 * s;
        if (ok && sel.range) ok = c[ax] >= (*sel.range)[0] && c[ax] <= (*sel.range)[1];
        m[f] = ok;
      }
      break;
    }
  }
  std::vector<std::int64_t> idx;
  for (std::size_t f = 0; f < nf; ++f)
    if (m[f]) idx.push_back(static_cast<std::int64_t>(f));
  if (idx.empty())
    throw SelectionError(name +
                         ": область не содержит ни одной поверхности детали. Проверьте координаты "
                         "(система детали, мм).");
  return idx;
}

// Псевдообратная симметричной матрицы 3×3 (как numpy.linalg.pinv: отбрасываются
// собственные числа меньше 1e-15 от наибольшего по модулю).
std::array<std::array<double, 3>, 3> pinv_sym3(std::array<std::array<double, 3>, 3> a) {
  std::array<std::array<double, 3>, 3> v{};
  for (std::size_t i = 0; i < 3; ++i) v[i][i] = 1.0;
  // метод вращений Якоби
  for (int sweep = 0; sweep < 64; ++sweep) {
    const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
    const double diag = a[0][0] * a[0][0] + a[1][1] * a[1][1] + a[2][2] * a[2][2];
    if (off <= 1e-34 * diag || off == 0.0) break;
    for (std::size_t p = 0; p < 2; ++p)
      for (std::size_t q = p + 1; q < 3; ++q) {
        if (a[p][q] == 0.0) continue;
        const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
        const double t = (theta >= 0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
        const double c = 1.0 / std::sqrt(t * t + 1.0);
        const double s = t * c;
        for (std::size_t k = 0; k < 3; ++k) {
          const double akp = a[k][p], akq = a[k][q];
          a[k][p] = c * akp - s * akq;
          a[k][q] = s * akp + c * akq;
        }
        for (std::size_t k = 0; k < 3; ++k) {
          const double apk = a[p][k], aqk = a[q][k];
          a[p][k] = c * apk - s * aqk;
          a[q][k] = s * apk + c * aqk;
        }
        for (std::size_t k = 0; k < 3; ++k) {
          const double vkp = v[k][p], vkq = v[k][q];
          v[k][p] = c * vkp - s * vkq;
          v[k][q] = s * vkp + c * vkq;
        }
      }
  }
  const std::array<double, 3> lam{a[0][0], a[1][1], a[2][2]};
  const double lmax = std::max({std::abs(lam[0]), std::abs(lam[1]), std::abs(lam[2])});
  std::array<std::array<double, 3>, 3> r{};
  for (std::size_t k = 0; k < 3; ++k) {
    if (!(std::abs(lam[k]) > 1e-15 * lmax)) continue;
    const double inv = 1.0 / lam[k];
    for (std::size_t i = 0; i < 3; ++i)
      for (std::size_t j = 0; j < 3; ++j) r[i][j] += v[i][k] * inv * v[j][k];
  }
  return r;
}

NodeWeights weights_from(const fem::Mesh& mesh, std::span<const std::int64_t> faces,
                         const std::vector<double>& face_w) {
  std::vector<std::pair<std::int64_t, double>> nw;
  nw.reserve(faces.size() * 4);
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const auto f = static_cast<std::size_t>(faces[i]);
    for (std::size_t q = 0; q < 4; ++q) nw.emplace_back(mesh.face_nodes[f][q], face_w[i] / 4.0);
  }
  // устойчивая сортировка — суммы копятся в том же порядке, что и в numpy.bincount
  std::stable_sort(nw.begin(), nw.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  NodeWeights out;
  for (const auto& [node, w] : nw) {
    if (out.nodes.empty() || out.nodes.back() != node) {
      out.nodes.push_back(node);
      out.w.push_back(0.0);
    }
    out.w.back() += w;
  }
  return out;
}

void add_node_force(std::vector<double>& f_out, std::int64_t node, const Vec3& f) {
  for (std::size_t c = 0; c < 3; ++c) f_out[static_cast<std::size_t>(3 * node) + c] += f[c];
}

}  // namespace

std::vector<std::int64_t> select_faces(const fem::Mesh& mesh, const Region& region, const Vec3& origin,
                                       const std::string& name) {
  if (region.empty()) throw SelectionError(name + ": не задана область");
  std::vector<std::int64_t> all;
  for (const auto& sel : region) {
    auto idx = select_one(mesh, sel, origin, name);
    all.insert(all.end(), idx.begin(), idx.end());
  }
  std::sort(all.begin(), all.end());
  all.erase(std::unique(all.begin(), all.end()), all.end());
  return all;
}

NodeWeights face_node_weights(const fem::Mesh& mesh, std::span<const std::int64_t> faces) {
  std::vector<double> fw(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) fw[i] = mesh.face_area[static_cast<std::size_t>(faces[i])];
  return weights_from(mesh, faces, fw);
}

Vec3 resolve_direction(const Direction& d, const fem::Mesh* mesh, std::span<const std::int64_t> faces) {
  if (d.vec) {
    const auto& v = *d.vec;
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-12) throw SelectionError("нулевое направление");
    return {v[0] / n, v[1] / n, v[2] / n};
  }
  const std::string t = util::to_lower_utf8(util::trim(d.name));
  const bool in = t == "normal_in" || t == "inward" || t == "внутрь";
  const bool out = t == "normal_out" || t == "outward" || t == "наружу";
  if (in || out) {
    if (!mesh) throw SelectionError("направление «" + d.name + "» здесь не подходит: нужна поверхность");
    Vec3 n{0, 0, 0};
    for (auto f : faces) {
      const auto nf = mesh->face_normal(static_cast<std::size_t>(f));
      const double a = mesh->face_area[static_cast<std::size_t>(f)];
      for (std::size_t c = 0; c < 3; ++c) n[c] += nf[c] * a;
    }
    const double len = std::max(std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), 1e-12);
    const double sg = in ? -1.0 : 1.0;
    return {sg * n[0] / len, sg * n[1] / len, sg * n[2] / len};
  }
  const double sg = (!t.empty() && t[0] == '-') ? -1.0 : 1.0;
  const auto p = t.find_first_not_of("+-");
  const std::string ax = p == std::string::npos ? std::string() : t.substr(p);
  Vec3 v{0, 0, 0};
  if (ax == "x") {
    v[0] = sg;
  } else if (ax == "y") {
    v[1] = sg;
  } else if (ax == "z") {
    v[2] = sg;
  } else {
    throw SelectionError("неизвестное направление «" + d.name +
                         "»: допустимо +x, -x, +y, -y, +z, -z, normal_in, normal_out или вектор [x, y, z]");
  }
  return v;
}

void distribute_force(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& force,
                      std::vector<double>& f_out) {
  const auto nw = face_node_weights(mesh, faces);
  const double total = std::accumulate(nw.w.begin(), nw.w.end(), 0.0);
  for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
    const double k = nw.w[i] / total;
    add_node_force(f_out, nw.nodes[i], {k * force[0], k * force[1], k * force[2]});
  }
}

void distribute_moment(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& moment,
                       std::vector<double>& f_out, const std::optional<Vec3>& center) {
  const auto nw = face_node_weights(mesh, faces);
  Vec3 c{0, 0, 0};
  if (center) {
    c = *center;
  } else {
    double wsum = 0;
    for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
      const auto& x = mesh.xyz[static_cast<std::size_t>(nw.nodes[i])];
      for (std::size_t a = 0; a < 3; ++a) c[a] += x[a] * nw.w[i];
      wsum += nw.w[i];
    }
    for (auto& v : c) v /= wsum;
  }
  std::array<std::array<double, 3>, 3> j{};
  std::vector<Vec3> d(nw.nodes.size());
  for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
    const auto& x = mesh.xyz[static_cast<std::size_t>(nw.nodes[i])];
    d[i] = {x[0] - c[0], x[1] - c[1], x[2] - c[2]};
    const double dd = d[i][0] * d[i][0] + d[i][1] * d[i][1] + d[i][2] * d[i][2];
    for (std::size_t a = 0; a < 3; ++a)
      for (std::size_t b = 0; b < 3; ++b) j[a][b] += nw.w[i] * ((a == b ? dd : 0.0) - d[i][a] * d[i][b]);
  }
  const auto jp = pinv_sym3(j);
  Vec3 th{0, 0, 0};
  for (std::size_t a = 0; a < 3; ++a)
    for (std::size_t b = 0; b < 3; ++b) th[a] += jp[a][b] * moment[b];
  for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
    const auto& r = d[i];
    const Vec3 cr{th[1] * r[2] - th[2] * r[1], th[2] * r[0] - th[0] * r[2], th[0] * r[1] - th[1] * r[0]};
    add_node_force(f_out, nw.nodes[i], {nw.w[i] * cr[0], nw.w[i] * cr[1], nw.w[i] * cr[2]});
  }
}

void distribute_pressure(const fem::Mesh& mesh, std::span<const std::int64_t> faces, double pressure,
                         std::vector<double>& f_out) {
  std::vector<Vec3> fv(faces.size());
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const auto f = static_cast<std::size_t>(faces[i]);
    const auto n = mesh.face_normal(f);
    for (std::size_t c = 0; c < 3; ++c) fv[i][c] = -pressure * mesh.face_area[f] * n[c];
  }
  for (std::size_t q = 0; q < 4; ++q)
    for (std::size_t i = 0; i < faces.size(); ++i)
      add_node_force(f_out, mesh.face_nodes[static_cast<std::size_t>(faces[i])][q],
                     {fv[i][0] / 4, fv[i][1] / 4, fv[i][2] / 4});
}

void distribute_bearing(const fem::Mesh& mesh, std::span<const std::int64_t> faces, const Vec3& force,
                        std::vector<double>& f_out) {
  const double fn = std::sqrt(force[0] * force[0] + force[1] * force[1] + force[2] * force[2]);
  if (fn < 1e-12) return;
  const Vec3 u{force[0] / fn, force[1] / fn, force[2] / fn};
  std::vector<double> wf(faces.size());
  double total = 0;
  for (std::size_t i = 0; i < faces.size(); ++i) {
    const auto f = static_cast<std::size_t>(faces[i]);
    const auto n = mesh.face_normal(f);
    wf[i] = std::max(0.0, -(n[0] * u[0] + n[1] * u[1] + n[2] * u[2])) * mesh.face_area[f];
    total += wf[i];
  }
  if (total <= 1e-12)
    for (std::size_t i = 0; i < faces.size(); ++i) wf[i] = mesh.face_area[static_cast<std::size_t>(faces[i])];
  const auto nw = weights_from(mesh, faces, wf);
  const double wsum = std::accumulate(nw.w.begin(), nw.w.end(), 0.0);
  for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
    const double k = nw.w[i] / wsum;
    add_node_force(f_out, nw.nodes[i], {k * force[0], k * force[1], k * force[2]});
  }
}

}  // namespace kika::analysis
