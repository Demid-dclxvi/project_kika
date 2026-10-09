#pragma once
// Воксельная расчётная модель по траекториям печати. Перенос fdmfea/voxelize.py.
//
// Для каждого вокселя:
//   • доля объёма, занятая стенками и сплошным заполнением (rho_shell);
//   • доля разреженного заполнения (rho_sparse), усреднённая по ячейке рисунка;
//   • распределение направлений нитей — гистограмма по kBins углам в плоскости слоя.
// Слои печати группируются так, чтобы границы вокселей по Z совпадали с границами слоёв.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "kika/gcode/toolpaths.hpp"

namespace kika::voxel {

// Число корзин направлений: центры 0°, 15°, 30° … 165°.
inline constexpr int kBins = 12;

struct Stats {
  double deposited_volume = 0;  // объём пластика, разложенный по вокселям, мм³
  double model_volume = 0;      // объём модели после отбора вокселей, мм³
  double removed_fraction = 0;  // доля массы в отброшенных несвязанных кусках
  std::array<int, 3> grid{};    // nx, ny, nz
  double line_width = 0;        // типичная ширина валика, мм
  double dz_mean = 0;           // средняя высота вокселя, мм
};

struct VoxelModel {
  double s = 0;   // средний размер вокселя в XY, мм
  double sx = 0;  // шаг по X
  double sy = 0;  // шаг по Y
  double x0 = 0;  // координата левой грани сетки по X
  double y0 = 0;
  int nx = 0, ny = 0, nz = 0;
  std::vector<double> z_edges;  // nz + 1 границ по Z

  // Активные воксели (упорядочены по плоскому номеру (iz·ny + iy)·nx + ix)
  std::vector<std::int32_t> ix, iy, iz;
  std::vector<double> rho_shell;
  std::vector<double> rho_sparse;
  std::vector<double> hist_shell;   // size()·kBins, нормировано (или нули)
  std::vector<double> hist_sparse;  // size()·kBins
  std::vector<gcode::Role> role;    // доминирующая роль: OuterWall, InnerWall, Solid или Sparse

  double infill_density = 0;
  std::string infill_pattern;
  double layer_height = 0;
  Stats stats;

  std::size_t size() const { return ix.size(); }
  double dz_layer(std::size_t k) const { return z_edges[k + 1] - z_edges[k]; }
  double dz(std::size_t e) const { return dz_layer(static_cast<std::size_t>(iz[e])); }
  std::int64_t flat(std::size_t e) const {
    return (static_cast<std::int64_t>(iz[e]) * ny + iy[e]) * nx + ix[e];
  }
  std::array<double, 3> center(std::size_t e) const {
    const auto k = static_cast<std::size_t>(iz[e]);
    return {x0 + (ix[e] + 0.5) * sx, y0 + (iy[e] + 0.5) * sy, 0.5 * (z_edges[k] + z_edges[k + 1])};
  }
  double rho(std::size_t e) const { return rho_shell[e] + rho_sparse[e]; }
  std::span<const double> shell_hist(std::size_t e) const { return {&hist_shell[e * kBins], kBins}; }
  std::span<const double> sparse_hist(std::size_t e) const { return {&hist_sparse[e * kBins], kBins}; }
};

struct Options {
  std::optional<double> voxel;  // размер вокселя, мм; по умолчанию подбирается под max_elems
  int max_elems = 120000;
  double rho_min_shell = 0.12;
  double rho_min_sparse = 0.02;
  bool keep_largest = true;  // оставить только самый большой связный кусок
};

// Исключение std::length_error, если сетка получается слишком мелкой.
VoxelModel voxelize(const gcode::Toolpaths& tp, const Options& options = {});

}  // namespace kika::voxel
