#pragma once
// Сетка МКЭ по воксельной модели: узлы, элементы, соседи, граничные грани.
// Перенос класса Mesh из fdmfea/fem.py.

#include <array>
#include <cstdint>
#include <vector>

#include "kika/fem/element.hpp"
#include "kika/voxel/voxel_model.hpp"

namespace kika::fem {

struct Mesh {
  const voxel::VoxelModel* vm = nullptr;

  std::int64_t n_nodes = 0;
  std::int64_t n_elems = 0;
  std::vector<std::int64_t> node_flat;              // плоский номер узла в сетке (nx+1)(ny+1)(nz+1)
  std::vector<std::array<std::int32_t, 3>> node_ijk;
  std::vector<std::array<double, 3>> xyz;           // координаты узлов, мм
  std::vector<std::array<std::int32_t, 8>> elem_nodes;
  std::vector<std::int64_t> elem_flat;
  std::vector<double> elem_vol;
  std::vector<std::array<std::int32_t, 6>> nbr;     // соседи по граням (−1 — нет)

  // Типоразмеры элементов (по высоте) и базисы
  std::vector<double> size_keys;
  std::vector<std::int32_t> elem_size_idx;
  std::vector<ElementBasis> bases;

  // Граничные грани (грань элемента без соседа)
  std::vector<std::int32_t> face_elem;
  std::vector<std::int8_t> face_dir;  // 0: −x, 1: +x, 2: −y, 3: +y, 4: −z, 5: +z
  std::vector<std::array<std::int32_t, 4>> face_nodes;
  std::vector<double> face_area;
  std::vector<std::array<double, 3>> face_center;
  std::vector<std::int64_t> face_key;  // elem_flat·6 + dir — устойчивый номер грани

  std::array<double, 3> face_normal(std::size_t f) const {
    const auto d = static_cast<std::size_t>(face_dir[f]);
    return {kFaceNormals[d][0], kFaceNormals[d][1], kFaceNormals[d][2]};
  }
  std::size_t n_faces() const { return face_elem.size(); }
};

Mesh build_mesh(const voxel::VoxelModel& vm);

}  // namespace kika::fem
