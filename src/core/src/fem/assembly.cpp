// Перенос assemble, rigid_modes и Solver из fdmfea/fem.py.

#include "kika/fem/assembly.hpp"

#include <algorithm>
#include <chrono>
#include <stdexcept>

#include "util/parallel.hpp"

namespace kika::fem {

namespace {

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

}  // namespace

std::array<std::int64_t, 24> elem_dofs(const Mesh& mesh, std::size_t e) {
  std::array<std::int64_t, 24> d{};
  for (std::size_t a = 0; a < 8; ++a)
    for (std::size_t c = 0; c < 3; ++c) d[3 * a + c] = 3 * static_cast<std::int64_t>(mesh.elem_nodes[e][a]) + static_cast<std::int64_t>(c);
  return d;
}

linalg::CsrMatrix assemble(const Mesh& mesh, std::span<const CVec> cvec) {
  if (cvec.size() != mesh.elem_nodes.size())
    throw std::invalid_argument("assemble: число наборов C не совпадает с числом элементов");
  const auto nn = static_cast<std::size_t>(mesh.n_nodes);
  const std::size_t ne = mesh.elem_nodes.size();

  // узел → элементы
  std::vector<std::int64_t> ne_ptr(nn + 1, 0);
  for (const auto& en : mesh.elem_nodes)
    for (auto p : en) ++ne_ptr[static_cast<std::size_t>(p) + 1];
  for (std::size_t p = 0; p < nn; ++p) ne_ptr[p + 1] += ne_ptr[p];
  std::vector<std::int32_t> ne_list(static_cast<std::size_t>(ne_ptr[nn]));
  {
    std::vector<std::int64_t> next(ne_ptr.begin(), ne_ptr.end() - 1);
    for (std::size_t e = 0; e < ne; ++e)
      for (auto p : mesh.elem_nodes[e]) ne_list[static_cast<std::size_t>(next[static_cast<std::size_t>(p)]++)] = static_cast<std::int32_t>(e);
  }
  // узел → соседние узлы (включая сам узел), по возрастанию
  std::vector<std::int64_t> nb_ptr(nn + 1, 0);
  std::vector<std::int32_t> nb_list;
  nb_list.reserve(nn * 20);
  std::vector<std::int32_t> tmp;
  for (std::size_t p = 0; p < nn; ++p) {
    tmp.clear();
    for (auto k = ne_ptr[p]; k < ne_ptr[p + 1]; ++k)
      for (auto q : mesh.elem_nodes[static_cast<std::size_t>(ne_list[static_cast<std::size_t>(k)])]) tmp.push_back(q);
    std::sort(tmp.begin(), tmp.end());
    tmp.erase(std::unique(tmp.begin(), tmp.end()), tmp.end());
    nb_list.insert(nb_list.end(), tmp.begin(), tmp.end());
    nb_ptr[p + 1] = static_cast<std::int64_t>(nb_list.size());
  }

  // портрет матрицы: строка 3p+c содержит столбцы 3q+c' для всех соседей q
  linalg::CsrMatrix k;
  k.rows = k.cols = 3 * static_cast<std::int64_t>(nn);
  k.row_ptr.assign(3 * nn + 1, 0);
  for (std::size_t p = 0; p < nn; ++p) {
    const std::int64_t len = 3 * (nb_ptr[p + 1] - nb_ptr[p]);
    for (std::size_t c = 0; c < 3; ++c) k.row_ptr[3 * p + c + 1] = k.row_ptr[3 * p + c] + len;
  }
  k.col.resize(static_cast<std::size_t>(k.row_ptr.back()));
  k.val.assign(static_cast<std::size_t>(k.row_ptr.back()), 0.0);
  for (std::size_t p = 0; p < nn; ++p)
    for (std::size_t c = 0; c < 3; ++c) {
      auto w = static_cast<std::size_t>(k.row_ptr[3 * p + c]);
      for (auto t = nb_ptr[p]; t < nb_ptr[p + 1]; ++t)
        for (std::int32_t c2 = 0; c2 < 3; ++c2) k.col[w++] = 3 * nb_list[static_cast<std::size_t>(t)] + c2;
    }

  // добавление матриц элементов. Элементы раскрашены в 8 цветов по чётности (i, j, k):
  // элементы одного цвета не имеют общих узлов, поэтому внутри цвета их можно добавлять
  // параллельно, а порядок сложений в каждой ячейке (по цветам) не зависит от числа потоков.
  std::array<std::vector<std::int32_t>, 8> colors;
  for (std::size_t e = 0; e < ne; ++e) {
    // узел 0 элемента — его угол (ix, iy, iz)
    const auto& ijk = mesh.node_ijk[static_cast<std::size_t>(mesh.elem_nodes[e][0])];
    colors[static_cast<std::size_t>((ijk[0] & 1) + 2 * (ijk[1] & 1) + 4 * (ijk[2] & 1))].push_back(
        static_cast<std::int32_t>(e));
  }
  for (const auto& list : colors) {
    util::parallel_for(0, list.size(), 256, [&](std::size_t lo, std::size_t hi) {
      std::array<double, 576> ke{};
      for (std::size_t t = lo; t < hi; ++t) {
        const auto e = static_cast<std::size_t>(list[t]);
        mesh.bases[static_cast<std::size_t>(mesh.elem_size_idx[e])].stiffness(cvec[e], ke.data());
        const auto& en = mesh.elem_nodes[e];
        for (std::size_t a = 0; a < 8; ++a) {
          const auto p = static_cast<std::size_t>(en[a]);
          const std::int32_t* nb0 = nb_list.data() + nb_ptr[p];
          const std::int32_t* nb1 = nb_list.data() + nb_ptr[p + 1];
          for (std::size_t b = 0; b < 8; ++b) {
            const auto pos = static_cast<std::int64_t>(std::lower_bound(nb0, nb1, en[b]) - nb0);
            for (std::size_t c = 0; c < 3; ++c) {
              double* dst = k.val.data() + k.row_ptr[3 * p + c] + 3 * pos;
              const double* src = ke.data() + (3 * a + c) * 24 + 3 * b;
              dst[0] += src[0];
              dst[1] += src[1];
              dst[2] += src[2];
            }
          }
        }
      }
    });
  }
  return k;
}

std::vector<double> rigid_modes(std::span<const std::array<double, 3>> xyz) {
  const std::size_t n = xyz.size();
  std::array<double, 3> m{0, 0, 0};
  for (const auto& p : xyz)
    for (std::size_t a = 0; a < 3; ++a) m[a] += p[a];
  if (n > 0)
    for (auto& v : m) v /= static_cast<double>(n);
  std::vector<double> b(3 * n * 6, 0.0);
  for (std::size_t i = 0; i < n; ++i) {
    const double cx = xyz[i][0] - m[0], cy = xyz[i][1] - m[1], cz = xyz[i][2] - m[2];
    double* rx = &b[(3 * i + 0) * 6];
    double* ry = &b[(3 * i + 1) * 6];
    double* rz = &b[(3 * i + 2) * 6];
    rx[0] = 1;
    ry[1] = 1;
    rz[2] = 1;
    rx[3] = -cy;  // вокруг Z
    ry[3] = cx;
    ry[4] = -cz;  // вокруг X
    rz[4] = cy;
    rx[5] = cz;  // вокруг Y
    rz[5] = -cx;
  }
  return b;
}

Solver::Solver(const linalg::CsrMatrix& k, std::span<const char> fixed, const Mesh& mesh, double tol,
               double springs, int max_iterations)
    : tol_(tol), max_iterations_(max_iterations) {
  const auto t0 = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < fixed.size(); ++i)
    if (!fixed[i]) free_.push_back(static_cast<std::int64_t>(i));
  linalg::CsrMatrix kff = linalg::submatrix(k, free_);
  const std::vector<double> diag = kff.diagonal();
  double mean = 0.0;
  for (double v : diag) mean += v;
  if (!diag.empty()) mean /= static_cast<double>(diag.size());
  eps_spring_ = springs * mean;
  // слабые пружины на диагональ (диагональ в портрете есть всегда)
  for (std::int64_t i = 0; i < kff.rows; ++i) {
    const auto b = kff.col.begin() + kff.row_ptr[i];
    const auto e = kff.col.begin() + kff.row_ptr[i + 1];
    const auto it = std::lower_bound(b, e, static_cast<std::int32_t>(i));
    if (it == e || *it != i) throw std::logic_error("Solver: в строке матрицы нет диагонального элемента");
    kff.val[static_cast<std::size_t>(it - kff.col.begin())] += eps_spring_;
  }
  const auto all_modes = rigid_modes(mesh.xyz);
  std::vector<double> modes(free_.size() * 6);
  std::vector<std::array<std::int64_t, 3>> cells(free_.size());
  for (std::size_t r = 0; r < free_.size(); ++r) {
    const auto dof = static_cast<std::size_t>(free_[r]);
    std::copy_n(&all_modes[dof * 6], 6, &modes[r * 6]);
    const auto& ijk = mesh.node_ijk[dof / 3];
    cells[r] = {ijk[0], ijk[1], ijk[2]};
  }
  ml_ = std::make_unique<linalg::SmoothedAggregation>(std::move(kff), std::move(modes), 6, std::move(cells));
  setup_time_ = seconds_since(t0);
}

std::vector<double> Solver::solve(std::span<const double> rhs_free, SolveInfo* info) {
  const auto t0 = std::chrono::steady_clock::now();
  std::vector<double> x(rhs_free.size(), 0.0);
  const auto res = ml_->solve(rhs_free, x, tol_, max_iterations_);
  if (info) {
    info->method = "AMG+CG";
    info->iterations = res.iterations;
    info->rel_residual = res.rel_residual;
    info->levels = ml_->n_levels();
    info->setup_time = setup_time_;
    info->time = seconds_since(t0);
  }
  if (res.rel_residual > 1e-4)
    throw std::runtime_error(
        "Решатель не сошёлся: проверьте закрепления (деталь может свободно двигаться или вращаться) и "
        "разумность нагрузок.");
  return x;
}

}  // namespace kika::fem
