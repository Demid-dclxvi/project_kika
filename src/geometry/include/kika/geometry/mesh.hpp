#pragma once
// Треугольная сетка модели (STL, 3MF, STEP после разбиения на треугольники) и простые операции над ней:
// габарит, объём, проверка замкнутости, слияние совпадающих вершин, преобразования.
// Единицы — мм.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kika::geometry {

using Vec3 = std::array<double, 3>;

// Аффинное преобразование: p' = R·p + t, матрица R хранится по строкам.
struct Transform {
  std::array<double, 9> r{1, 0, 0, 0, 1, 0, 0, 0, 1};  // по строкам: r[0..2] — строка X
  Vec3 t{0, 0, 0};

  Vec3 apply(const Vec3& p) const {
    return {r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + t[0], r[3] * p[0] + r[4] * p[1] + r[5] * p[2] + t[1],
            r[6] * p[0] + r[7] * p[1] + r[8] * p[2] + t[2]};
  }
  // this ∘ other: сначала other, затем this.
  Transform operator*(const Transform& o) const;
  double det() const;

  static Transform translation(const Vec3& d);
  static Transform scale(double s);
  // Поворот на угол (градусы) вокруг оси 0 — X, 1 — Y, 2 — Z.
  static Transform rotation(int axis, double degrees);
};

struct Box {
  Vec3 min{0, 0, 0};
  Vec3 max{0, 0, 0};
  Vec3 size() const { return {max[0] - min[0], max[1] - min[1], max[2] - min[2]}; }
  Vec3 center() const { return {0.5 * (min[0] + max[0]), 0.5 * (min[1] + max[1]), 0.5 * (min[2] + max[2])}; }
};

struct TriangleMesh {
  std::vector<Vec3> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;

  bool empty() const { return triangles.empty(); }
  Box bbox() const;
  // Объём со знаком (положительный при нормалях наружу), мм³.
  double volume() const;
  double area() const;
  // Добавить другую сетку (вершины не сливаются).
  void append(const TriangleMesh& other, const Transform& tr = {});
  void transform(const Transform& tr);
  // Перевернуть все треугольники (нормали внутрь ↔ наружу).
  void flip();
};

// Слить вершины, совпадающие с точностью tol (мм); выбросить вырожденные треугольники.
// Нужно для STL: там у каждого треугольника свои копии вершин.
void merge_vertices(TriangleMesh& mesh, double tol = 1e-6);

// Проверка: замкнута ли поверхность (каждое ребро — ровно у двух треугольников, обход согласован).
struct MeshCheck {
  std::size_t open_edges = 0;          // рёбра с одним треугольником (дыры)
  std::size_t nonmanifold_edges = 0;   // рёбра больше чем с двумя треугольниками
  std::size_t flipped_edges = 0;       // соседние треугольники обходят ребро в одну сторону
  std::size_t degenerate = 0;          // треугольники нулевой площади
  std::size_t shells = 0;              // связных кусков
  bool closed() const { return open_edges == 0 && nonmanifold_edges == 0 && flipped_edges == 0; }
};
MeshCheck check(const TriangleMesh& mesh);

// Поставить на стол: низ на Z = 0, центр габарита в плане — в точку (x, y).
Transform place_on_bed(const TriangleMesh& mesh, double x, double y);

}  // namespace kika::geometry
