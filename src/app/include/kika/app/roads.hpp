#pragma once
// Нити детали для 3D-вида: каждый отрезок экструзии из G-code рисуется лентой сечением
// «ширина валика × высота слоя» (ромб с гладкими нормалями — выглядит как скруглённый валик).
// Цвет нити — из поля расчёта в ячейке сетки, через которую она проходит, поэтому отрезок
// разбивается на куски по границам ячеек. Без Qt: окно только загружает готовые массивы в OpenGL.

#include <cstdint>
#include <vector>

#include "kika/analysis/analysis.hpp"
#include "kika/app/scene.hpp"

namespace kika::app {

// Готовая сетка треугольников для OpenGL.
struct RoadMesh {
  std::vector<float> pos;          // x, y, z на вершину (система детали, мм)
  std::vector<std::int8_t> nrm;    // нормаль × 127 и 0 — по 4 на вершину
  std::vector<std::uint8_t> col;   // r, g, b, 255 на вершину
  std::vector<std::uint32_t> idx;  // треугольники

  std::size_t vertex_count() const { return pos.size() / 3; }
  std::size_t triangle_count() const { return idx.size() / 3; }
};

class Roads {
 public:
  // Модель и сцена должны жить дольше.
  Roads(const analysis::Model& model, const Scene& scene);

  std::size_t segment_count() const { return seg_.size(); }
  std::size_t piece_count() const { return pieces_.size(); }

  // Сетка по текущему полю, разрезу и случаю сцены, с подсветкой и деформацией × deform.
  // С деформацией нить разбивается по каждой ячейке, чтобы изгиб был виден и на длинных отрезках.
  void build(RoadMesh& out, const std::vector<Overlay>& overlays, double deform) const;

  // Кусок отрезка внутри одной ячейки сетки: доли длины отрезка и элемент (−1 — ячейка пустая).
  struct Piece {
    float t0 = 0, t1 = 0;
    std::int32_t elem = -1;
  };
  // Куски отрезка k (из segment_count()), по порядку вдоль отрезка.
  const Piece* pieces_begin(std::size_t k) const { return pieces_.data() + first_[k]; }
  const Piece* pieces_end(std::size_t k) const { return pieces_.data() + first_[k + 1]; }
  // Номер отрезка в model.tp.segments.
  std::uint32_t segment_index(std::size_t k) const { return seg_[k]; }

 private:
  Vec3 displaced(const Vec3& p, std::int32_t elem, double deform) const;

  const analysis::Model* model_;
  const Scene* scene_;
  std::vector<std::uint32_t> seg_;    // отрезки, вошедшие в модель
  std::vector<std::uint32_t> first_;  // начало кусков каждого отрезка в pieces_ (+ конец)
  std::vector<Piece> pieces_;
  std::vector<float> width_;          // ширина валика, мм
};

}  // namespace kika::app
