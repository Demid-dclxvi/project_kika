// Треугольная сетка: габарит, объём, слияние вершин, проверка замкнутости, преобразования.

#include "kika/geometry/mesh.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <unordered_map>

namespace kika::geometry {

Transform Transform::operator*(const Transform& o) const {
  Transform out;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) {
      double s = 0;
      for (int k = 0; k < 3; ++k) s += r[static_cast<std::size_t>(3 * i + k)] * o.r[static_cast<std::size_t>(3 * k + j)];
      out.r[static_cast<std::size_t>(3 * i + j)] = s;
    }
  out.t = apply(o.t);
  return out;
}

double Transform::det() const {
  return r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) + r[2] * (r[3] * r[7] - r[4] * r[6]);
}

Transform Transform::translation(const Vec3& d) {
  Transform t;
  t.t = d;
  return t;
}

Transform Transform::scale(double s) {
  Transform t;
  t.r = {s, 0, 0, 0, s, 0, 0, 0, s};
  return t;
}

Transform Transform::rotation(int axis, double degrees) {
  // кратные 90° — точно, без хвостов вроде 6e-17
  const double q = degrees / 90.0;
  double c, s;
  if (std::abs(q - std::round(q)) < 1e-12) {
    const int k = ((static_cast<int>(std::lround(q)) % 4) + 4) % 4;
    c = k == 0 ? 1 : (k == 2 ? -1 : 0);
    s = k == 1 ? 1 : (k == 3 ? -1 : 0);
  } else {
    const double a = degrees * std::numbers::pi / 180.0;
    c = std::cos(a);
    s = std::sin(a);
  }
  Transform t;
  if (axis == 0)
    t.r = {1, 0, 0, 0, c, -s, 0, s, c};
  else if (axis == 1)
    t.r = {c, 0, s, 0, 1, 0, -s, 0, c};
  else
    t.r = {c, -s, 0, s, c, 0, 0, 0, 1};
  return t;
}

Box TriangleMesh::bbox() const {
  Box b;
  if (vertices.empty()) return b;
  b.min = b.max = vertices[0];
  for (const auto& v : vertices)
    for (std::size_t a = 0; a < 3; ++a) {
      b.min[a] = std::min(b.min[a], v[a]);
      b.max[a] = std::max(b.max[a], v[a]);
    }
  return b;
}

double TriangleMesh::volume() const {
  // относительно центра габарита — меньше потерь точности вдали от начала координат
  const Vec3 c = bbox().center();
  double v = 0;
  for (const auto& t : triangles) {
    const Vec3& a = vertices[t[0]];
    const Vec3& b = vertices[t[1]];
    const Vec3& d = vertices[t[2]];
    const double ax = a[0] - c[0], ay = a[1] - c[1], az = a[2] - c[2];
    const double bx = b[0] - c[0], by = b[1] - c[1], bz = b[2] - c[2];
    const double dx = d[0] - c[0], dy = d[1] - c[1], dz = d[2] - c[2];
    v += ax * (by * dz - bz * dy) - ay * (bx * dz - bz * dx) + az * (bx * dy - by * dx);
  }
  return v / 6.0;
}

double TriangleMesh::area() const {
  double s = 0;
  for (const auto& t : triangles) {
    const Vec3& a = vertices[t[0]];
    const Vec3& b = vertices[t[1]];
    const Vec3& c = vertices[t[2]];
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    s += 0.5 * std::sqrt(std::pow(u[1] * w[2] - u[2] * w[1], 2) + std::pow(u[2] * w[0] - u[0] * w[2], 2) +
                         std::pow(u[0] * w[1] - u[1] * w[0], 2));
  }
  return s;
}

void TriangleMesh::append(const TriangleMesh& other, const Transform& tr) {
  const auto base = static_cast<std::uint32_t>(vertices.size());
  vertices.reserve(vertices.size() + other.vertices.size());
  for (const auto& v : other.vertices) vertices.push_back(tr.apply(v));
  const bool mirror = tr.det() < 0;  // зеркальное преобразование выворачивает обход
  for (const auto& t : other.triangles) {
    if (mirror)
      triangles.push_back({t[0] + base, t[2] + base, t[1] + base});
    else
      triangles.push_back({t[0] + base, t[1] + base, t[2] + base});
  }
}

void TriangleMesh::transform(const Transform& tr) {
  for (auto& v : vertices) v = tr.apply(v);
  if (tr.det() < 0) flip();
}

void TriangleMesh::flip() {
  for (auto& t : triangles) std::swap(t[1], t[2]);
}

namespace {

struct CellKey {
  std::int64_t x, y, z;
  bool operator==(const CellKey&) const = default;
};

struct CellHash {
  std::size_t operator()(const CellKey& k) const {
    std::uint64_t h = static_cast<std::uint64_t>(k.x) * 0x9E3779B97F4A7C15ull;
    h ^= static_cast<std::uint64_t>(k.y) * 0xC2B2AE3D27D4EB4Full + (h << 6) + (h >> 2);
    h ^= static_cast<std::uint64_t>(k.z) * 0x165667B19E3779F9ull + (h << 6) + (h >> 2);
    return static_cast<std::size_t>(h);
  }
};

}  // namespace

void merge_vertices(TriangleMesh& mesh, double tol) {
  // вершины в одной клетке размером tol — одна вершина; соседние клетки тоже проверяются
  const double inv = 1.0 / std::max(tol, 1e-12);
  std::unordered_map<CellKey, std::uint32_t, CellHash> cells;
  cells.reserve(mesh.vertices.size());
  std::vector<std::uint32_t> remap(mesh.vertices.size());
  std::vector<Vec3> out;
  out.reserve(mesh.vertices.size() / 4 + 16);
  for (std::size_t i = 0; i < mesh.vertices.size(); ++i) {
    const Vec3& v = mesh.vertices[i];
    const CellKey k{static_cast<std::int64_t>(std::floor(v[0] * inv)), static_cast<std::int64_t>(std::floor(v[1] * inv)),
                    static_cast<std::int64_t>(std::floor(v[2] * inv))};
    std::uint32_t found = UINT32_MAX;
    for (std::int64_t dx = -1; dx <= 1 && found == UINT32_MAX; ++dx)
      for (std::int64_t dy = -1; dy <= 1 && found == UINT32_MAX; ++dy)
        for (std::int64_t dz = -1; dz <= 1 && found == UINT32_MAX; ++dz) {
          const auto it = cells.find({k.x + dx, k.y + dy, k.z + dz});
          if (it == cells.end()) continue;
          const Vec3& w = out[it->second];
          if (std::abs(w[0] - v[0]) <= tol && std::abs(w[1] - v[1]) <= tol && std::abs(w[2] - v[2]) <= tol)
            found = it->second;
        }
    if (found == UINT32_MAX) {
      found = static_cast<std::uint32_t>(out.size());
      out.push_back(v);
      cells.emplace(k, found);
    }
    remap[i] = found;
  }
  std::vector<std::array<std::uint32_t, 3>> tris;
  tris.reserve(mesh.triangles.size());
  for (const auto& t : mesh.triangles) {
    const std::array<std::uint32_t, 3> u{remap[t[0]], remap[t[1]], remap[t[2]]};
    if (u[0] == u[1] || u[1] == u[2] || u[0] == u[2]) continue;
    tris.push_back(u);
  }
  mesh.vertices = std::move(out);
  mesh.triangles = std::move(tris);
}

MeshCheck check(const TriangleMesh& mesh) {
  MeshCheck c;
  // ребро (меньшая вершина, большая) → сколько раз и в какую сторону обходится
  struct EdgeUse {
    int forward = 0;  // от меньшей к большей
    int backward = 0;
  };
  std::unordered_map<std::uint64_t, EdgeUse> edges;
  edges.reserve(mesh.triangles.size() * 2);
  for (const auto& t : mesh.triangles) {
    const Vec3& a = mesh.vertices[t[0]];
    const Vec3& b = mesh.vertices[t[1]];
    const Vec3& d = mesh.vertices[t[2]];
    const Vec3 u{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, w{d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    const double n2 = std::pow(u[1] * w[2] - u[2] * w[1], 2) + std::pow(u[2] * w[0] - u[0] * w[2], 2) +
                      std::pow(u[0] * w[1] - u[1] * w[0], 2);
    if (n2 < 1e-24) ++c.degenerate;
    for (int k = 0; k < 3; ++k) {
      const std::uint32_t p = t[static_cast<std::size_t>(k)], q = t[static_cast<std::size_t>((k + 1) % 3)];
      const std::uint64_t key = (static_cast<std::uint64_t>(std::min(p, q)) << 32) | std::max(p, q);
      auto& e = edges[key];
      if (p < q)
        ++e.forward;
      else
        ++e.backward;
    }
  }
  for (const auto& [key, e] : edges) {
    const int n = e.forward + e.backward;
    if (n == 1)
      ++c.open_edges;
    else if (n > 2)
      ++c.nonmanifold_edges;
    else if (e.forward != 1)
      ++c.flipped_edges;
  }
  // связные куски: объединение вершин по треугольникам
  std::vector<std::uint32_t> parent(mesh.vertices.size());
  std::iota(parent.begin(), parent.end(), 0u);
  auto find = [&](std::uint32_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  };
  std::vector<char> used(mesh.vertices.size(), 0);
  for (const auto& t : mesh.triangles) {
    for (auto v : t) used[v] = 1;
    const auto a = find(t[0]);
    parent[find(t[1])] = a;
    parent[find(t[2])] = a;
  }
  for (std::uint32_t v = 0; v < parent.size(); ++v)
    if (used[v] && find(v) == v) ++c.shells;
  return c;
}

Transform place_on_bed(const TriangleMesh& mesh, double x, double y) {
  const Box b = mesh.bbox();
  return Transform::translation({x - 0.5 * (b.min[0] + b.max[0]), y - 0.5 * (b.min[1] + b.max[1]), -b.min[2]});
}

}  // namespace kika::geometry
