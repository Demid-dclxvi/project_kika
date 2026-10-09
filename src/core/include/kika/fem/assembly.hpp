#pragma once
// Сборка глобальной матрицы жёсткости и решатель K_ff·u_f = f_f. Перенос fdmfea/fem.py.

#include <array>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "kika/fem/mesh.hpp"
#include "kika/linalg/amg.hpp"
#include "kika/linalg/sparse.hpp"

namespace kika::fem {

// Степени свободы элемента: 3·узел + компонента, 24 шт.
std::array<std::int64_t, 24> elem_dofs(const Mesh& mesh, std::size_t e);

// Глобальная матрица жёсткости 3N×3N. cvec — 21 компонента C каждого элемента.
// Портрет строится заранее по соседству узлов, поэтому сборка идёт без сортировки троек.
linalg::CsrMatrix assemble(const Mesh& mesh, std::span<const CVec> cvec);

// Шесть перемещений тела как жёсткого целого (3N × 6, построчно): сдвиги X, Y, Z,
// повороты вокруг Z, X, Y относительно центра узлов.
std::vector<double> rigid_modes(std::span<const std::array<double, 3>> xyz);

struct SolveInfo {
  std::string method;  // «AMG+CG»
  int iterations = 0;
  double rel_residual = 0.0;
  int levels = 0;
  double setup_time = 0.0;
  double time = 0.0;
};

// Решатель для заданного набора закреплённых степеней свободы.
// Всегда многосеточный AMG + CG: для маленьких задач уровней нет и грубая задача
// решается напрямую (Холецкий), что равносильно прямому решателю прототипа.
class Solver {
 public:
  // fixed — признак закреплённой степени свободы (3N); springs — слабые пружины на диагонали
  // в долях средней диагонали (1e-12, а если деталь не закреплена по какой-то оси — 1e-7).
  Solver(const linalg::CsrMatrix& k, std::span<const char> fixed, const Mesh& mesh, double tol = 1e-7,
         double springs = 1e-12, int max_iterations = 800);

  const std::vector<std::int64_t>& free_dofs() const { return free_; }
  double spring() const { return eps_spring_; }

  // Решение для правой части по свободным степеням свободы. Бросает std::runtime_error,
  // если итерации не сошлись (деталь не закреплена или нагрузки неразумны).
  std::vector<double> solve(std::span<const double> rhs_free, SolveInfo* info = nullptr);

 private:
  std::vector<std::int64_t> free_;
  double eps_spring_ = 0.0;
  double tol_ = 1e-7;
  int max_iterations_ = 800;
  double setup_time_ = 0.0;
  std::unique_ptr<linalg::SmoothedAggregation> ml_;
};

}  // namespace kika::fem
