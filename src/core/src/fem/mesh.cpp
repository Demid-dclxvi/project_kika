#include "kika/fem/mesh.hpp"

#include <algorithm>
#include <cmath>

namespace kika::fem {

Mesh build_mesh(const voxel::VoxelModel& vm) {
  Mesh m;
  m.vm = &vm;
  const std::size_t n = vm.size();
  m.n_elems = static_cast<std::int64_t>(n);
  const std::int64_t nx1 = vm.nx + 1, ny1 = vm.ny + 1;

  // узлы: уникальные углы элементов
  std::vector<std::int64_t> corners(n * 8);
  for (std::size_t e = 0; e < n; ++e)
    for (int a = 0; a < 8; ++a) {
      const std::int64_t i = vm.ix[e] + (kLocalNodes[a][0] + 1) / 2;
      const std::int64_t j = vm.iy[e] + (kLocalNodes[a][1] + 1) / 2;
      const std::int64_t k = vm.iz[e] + (kLocalNodes[a][2] + 1) / 2;
      corners[e * 8 + static_cast<std::size_t>(a)] = (k * ny1 + j) * nx1 + i;
    }
  m.node_flat = corners;
  std::sort(m.node_flat.begin(), m.node_flat.end());
  m.node_flat.erase(std::unique(m.node_flat.begin(), m.node_flat.end()), m.node_flat.end());
  m.n_nodes = static_cast<std::int64_t>(m.node_flat.size());
  m.elem_nodes.resize(n);
  for (std::size_t e = 0; e < n; ++e)
    for (int a = 0; a < 8; ++a)
      m.elem_nodes[e][static_cast<std::size_t>(a)] = static_cast<std::int32_t>(
          std::lower_bound(m.node_flat.begin(), m.node_flat.end(), corners[e * 8 + static_cast<std::size_t>(a)]) -
          m.node_flat.begin());
  m.node_ijk.resize(m.node_flat.size());
  m.xyz.resize(m.node_flat.size());
  for (std::size_t p = 0; p < m.node_flat.size(); ++p) {
    const std::int64_t f = m.node_flat[p];
    const std::int64_t k = f / (nx1 * ny1);
    const std::int64_t rem = f % (nx1 * ny1);
    const std::int64_t j = rem / nx1;
    const std::int64_t i = rem % nx1;
    m.node_ijk[p] = {static_cast<std::int32_t>(i), static_cast<std::int32_t>(j), static_cast<std::int32_t>(k)};
    m.xyz[p] = {vm.x0 + static_cast<double>(i) * vm.sx, vm.y0 + static_cast<double>(j) * vm.sy,
                vm.z_edges[static_cast<std::size_t>(k)]};
  }

  m.elem_flat.resize(n);
  m.elem_vol.resize(n);
  for (std::size_t e = 0; e < n; ++e) {
    m.elem_flat[e] = vm.flat(e);
    m.elem_vol[e] = vm.sx * vm.sy * vm.dz(e);
  }

  // типоразмеры по высоте вокселя (округление до 1e-5 мм, как в прототипе)
  std::vector<double> key(n);
  for (std::size_t e = 0; e < n; ++e) key[e] = std::nearbyint(vm.dz(e) * 1e5) / 1e5;
  m.size_keys = key;
  std::sort(m.size_keys.begin(), m.size_keys.end());
  m.size_keys.erase(std::unique(m.size_keys.begin(), m.size_keys.end()), m.size_keys.end());
  m.elem_size_idx.resize(n);
  for (std::size_t e = 0; e < n; ++e)
    m.elem_size_idx[e] = static_cast<std::int32_t>(
        std::lower_bound(m.size_keys.begin(), m.size_keys.end(), key[e]) - m.size_keys.begin());
  m.bases.reserve(m.size_keys.size());
  for (double d : m.size_keys) m.bases.emplace_back(vm.sx, vm.sy, d);

  // соседи по граням
  constexpr int steps[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
  m.nbr.resize(n);
  for (std::size_t e = 0; e < n; ++e)
    for (int d = 0; d < 6; ++d) {
      const std::int64_t ii = vm.ix[e] + steps[d][0];
      const std::int64_t jj = vm.iy[e] + steps[d][1];
      const std::int64_t kk = vm.iz[e] + steps[d][2];
      std::int32_t found = -1;
      if (ii >= 0 && ii < vm.nx && jj >= 0 && jj < vm.ny && kk >= 0 && kk < vm.nz) {
        const std::int64_t f2 = (kk * vm.ny + jj) * vm.nx + ii;
        const auto it = std::lower_bound(m.elem_flat.begin(), m.elem_flat.end(), f2);
        if (it != m.elem_flat.end() && *it == f2) found = static_cast<std::int32_t>(it - m.elem_flat.begin());
      }
      m.nbr[e][static_cast<std::size_t>(d)] = found;
    }

  // граничные грани
  for (std::size_t e = 0; e < n; ++e)
    for (int d = 0; d < 6; ++d) {
      if (m.nbr[e][static_cast<std::size_t>(d)] >= 0) continue;
      m.face_elem.push_back(static_cast<std::int32_t>(e));
      m.face_dir.push_back(static_cast<std::int8_t>(d));
      std::array<std::int32_t, 4> fn{};
      std::array<double, 3> c{};
      for (int q = 0; q < 4; ++q) {
        fn[static_cast<std::size_t>(q)] = m.elem_nodes[e][static_cast<std::size_t>(kFaceNodes[d][q])];
        for (int ax = 0; ax < 3; ++ax)
          c[static_cast<std::size_t>(ax)] += m.xyz[static_cast<std::size_t>(fn[static_cast<std::size_t>(q)])][static_cast<std::size_t>(ax)];
      }
      for (double& v : c) v /= 4.0;
      m.face_nodes.push_back(fn);
      m.face_center.push_back(c);
      const double dz = vm.dz(e);
      m.face_area.push_back(d < 2 ? vm.sy * dz : (d < 4 ? vm.sx * dz : vm.sx * vm.sy));
      m.face_key.push_back(m.elem_flat[e] * 6 + d);
    }
  return m;
}

}  // namespace kika::fem
