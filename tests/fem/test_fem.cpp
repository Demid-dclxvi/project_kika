// МКЭ и решатель: перенос fdmfea/fem.py и fdmfea/amg.py. Эталонные числа получены прототипом;
// поэлементная сверка на полных кронштейнах — в tools/compare_with_prototype.py.

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/fem/assembly.hpp"
#include "kika/fem/element.hpp"
#include "kika/fem/mesh.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/material/material.hpp"
#include "kika/parallel.hpp"
#include "kika/voxel/voxel_model.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

kika::linalg::Mat6 isotropic(double e, double nu) {
  const double g = e / (2 * (1 + nu));
  return kika::material::orthotropic_stiffness(e, e, e, g, g, g, nu, nu, nu);
}

kika::linalg::Mat6 random_spd(std::mt19937& gen) {
  std::uniform_real_distribution<double> d(-1.0, 1.0);
  kika::linalg::Mat6 a, c;
  for (auto& v : a.a) v = d(gen);
  for (std::size_t i = 0; i < 6; ++i)
    for (std::size_t j = 0; j < 6; ++j) {
      double s = i == j ? 6.0 : 0.0;
      for (std::size_t k = 0; k < 6; ++k) s += a(i, k) * a(j, k);
      c(i, j) = s;
    }
  return c;
}

// Задача для проверки решателя: кронштейн на грубой сетке, низ закреплён, на верх — сила.
struct SmallProblem {
  kika::voxel::VoxelModel vm;
  kika::fem::Mesh mesh;
  kika::linalg::CsrMatrix k;
  std::vector<char> fixed;
  std::vector<double> f;
};

SmallProblem small_problem() {
  SmallProblem p;
  const auto tp =
      kika::gcode::load(std::filesystem::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / "bracket_side.gcode");
  kika::voxel::Options opt;
  opt.max_elems = 1500;
  p.vm = kika::voxel::voxelize(tp, opt);
  p.mesh = kika::fem::build_mesh(p.vm);
  const auto c = kika::fem::c_to_vec(kika::material::stiffness(*kika::material::find_material("PLA")));
  std::vector<kika::fem::CVec> cv(p.vm.size());
  for (std::size_t e = 0; e < cv.size(); ++e)
    for (std::size_t q = 0; q < c.size(); ++q) cv[e][q] = p.vm.rho(e) * c[q];
  p.k = kika::fem::assemble(p.mesh, cv);
  double zmin = 1e300, zmax = -1e300;
  for (const auto& x : p.mesh.xyz) {
    zmin = std::min(zmin, x[2]);
    zmax = std::max(zmax, x[2]);
  }
  const auto nn = static_cast<std::size_t>(p.mesh.n_nodes);
  p.fixed.assign(3 * nn, 0);
  p.f.assign(3 * nn, 0.0);
  std::size_t ntop = 0;
  for (const auto& x : p.mesh.xyz)
    if (x[2] == zmax) ++ntop;
  for (std::size_t i = 0; i < nn; ++i) {
    if (p.mesh.xyz[i][2] == zmin) p.fixed[3 * i] = p.fixed[3 * i + 1] = p.fixed[3 * i + 2] = 1;
    if (p.mesh.xyz[i][2] == zmax) {
      p.f[3 * i] += 10.0 / static_cast<double>(ntop);
      p.f[3 * i + 2] += -50.0 / static_cast<double>(ntop);
    }
  }
  return p;
}

std::vector<double> solve_full(SmallProblem& p, kika::fem::SolveInfo* info = nullptr) {
  kika::fem::Solver s(p.k, p.fixed, p.mesh);
  std::vector<double> rf;
  for (auto d : s.free_dofs()) rf.push_back(p.f[static_cast<std::size_t>(d)]);
  const auto uf = s.solve(rf, info);
  std::vector<double> u(p.f.size(), 0.0);
  for (std::size_t i = 0; i < uf.size(); ++i) u[static_cast<std::size_t>(s.free_dofs()[i])] = uf[i];
  return u;
}

}  // namespace

TEST_CASE("Матрица жёсткости элемента совпадает с прототипом", "[fem][reference]") {
  const kika::fem::ElementBasis b(1.0, 0.7, 0.35);
  std::array<double, 576> ke{};
  b.stiffness(kika::fem::c_to_vec(isotropic(1000.0, 0.3)), ke.data());
  CHECK_THAT(ke[0], WithinRel(120.92681623931625, 1e-12));
  CHECK_THAT(ke[1], WithinRel(26.241987179487175, 1e-12));
  CHECK_THAT(ke[5 * 24 + 17], WithinRel(-256.6525488400488, 1e-12));
  CHECK_THAT(ke[23 * 24 + 23], WithinRel(304.408959096459, 1e-12));
  double tr = 0;
  for (std::size_t i = 0; i < 24; ++i) tr += ke[i * 25];
  CHECK_THAT(tr, WithinRel(4583.379120879121, 1e-12));
}

TEST_CASE("Элемент: симметрия, движение как жёсткого целого, однородная деформация", "[fem]") {
  std::mt19937 gen(7);
  const kika::fem::ElementBasis b(0.6, 0.6, 0.2);
  const auto c = random_spd(gen);
  const auto cv = kika::fem::c_to_vec(c);
  std::array<double, 576> ke{};
  b.stiffness(cv, ke.data());
  double kmax = 0;
  for (double v : ke) kmax = std::max(kmax, std::abs(v));
  for (std::size_t i = 0; i < 24; ++i)
    for (std::size_t j = 0; j < i; ++j) REQUIRE_THAT(ke[i * 24 + j] - ke[j * 24 + i], WithinAbs(0.0, 1e-12 * kmax));

  // узловые координаты элемента
  std::array<std::array<double, 3>, 8> x{};
  for (std::size_t a = 0; a < 8; ++a)
    x[a] = {0.3 * kika::fem::kLocalNodes[a][0], 0.3 * kika::fem::kLocalNodes[a][1],
            0.1 * kika::fem::kLocalNodes[a][2]};

  SECTION("перемещение как жёсткого целого не даёт сил") {
    const std::array<double, 3> t{0.1, -0.2, 0.3}, w{0.01, -0.02, 0.015};
    std::array<double, 24> u{};
    for (std::size_t a = 0; a < 8; ++a) {
      u[3 * a + 0] = t[0] + w[1] * x[a][2] - w[2] * x[a][1];
      u[3 * a + 1] = t[1] + w[2] * x[a][0] - w[0] * x[a][2];
      u[3 * a + 2] = t[2] + w[0] * x[a][1] - w[1] * x[a][0];
    }
    for (std::size_t i = 0; i < 24; ++i) {
      double s = 0;
      for (std::size_t j = 0; j < 24; ++j) s += ke[i * 24 + j] * u[j];
      CHECK_THAT(s, WithinAbs(0.0, 1e-12 * kmax));
    }
  }
  SECTION("однородная деформация передаётся точно (патч-тест)") {
    const std::array<double, 6> eps{1e-3, -2e-4, 5e-4, 3e-4, -1e-4, 2e-4};  // ε11 ε22 ε33 γ23 γ13 γ12
    std::array<double, 24> u{};
    for (std::size_t a = 0; a < 8; ++a) {
      const auto& p = x[a];
      u[3 * a + 0] = eps[0] * p[0] + 0.5 * eps[5] * p[1] + 0.5 * eps[4] * p[2];
      u[3 * a + 1] = 0.5 * eps[5] * p[0] + eps[1] * p[1] + 0.5 * eps[3] * p[2];
      u[3 * a + 2] = 0.5 * eps[4] * p[0] + 0.5 * eps[3] * p[1] + eps[2] * p[2];
    }
    std::array<double, 48> eg{};
    std::array<double, 6> em{};
    b.strains(cv, u.data(), eg.data(), em.data());
    for (std::size_t g = 0; g < 8; ++g)
      for (std::size_t i = 0; i < 6; ++i) CHECK_THAT(eg[g * 6 + i], WithinAbs(eps[i], 1e-15));
    for (std::size_t i = 0; i < 6; ++i) CHECK_THAT(em[i], WithinAbs(eps[i], 1e-15));
  }
  SECTION("∫B dV = объём × B в центре") {
    const auto& bi = b.b_integral();
    // для узла 6 (+1,+1,+1): ∂N/∂x в центре = 1/(4·dx)
    CHECK_THAT(bi[0 * 24 + 18], WithinRel(b.volume() / (4 * 0.6), 1e-12));
    CHECK_THAT(bi[2 * 24 + 20], WithinRel(b.volume() / (4 * 0.2), 1e-12));
  }
}

TEST_CASE("Сетка по вокселям совпадает с прототипом", "[fem][reference]") {
  auto p = small_problem();
  const auto& m = p.mesh;
  CHECK(p.vm.size() == 1316);
  CHECK(m.n_nodes == 2037);
  CHECK(m.n_faces() == 1330);
  CHECK(m.size_keys.size() == 2);
  double area = 0;
  for (double a : m.face_area) area += a;
  CHECK_THAT(area, WithinRel(8961.593506896777, 1e-12));
  // соседство симметрично: если e видит f через грань d, то f видит e через противоположную
  for (std::size_t e = 0; e < m.nbr.size(); ++e)
    for (std::size_t d = 0; d < 6; ++d) {
      const auto f = m.nbr[e][d];
      if (f >= 0) REQUIRE(m.nbr[static_cast<std::size_t>(f)][d ^ 1] == static_cast<std::int32_t>(e));
    }
}

TEST_CASE("Сборка и решение совпадают с прототипом", "[fem][solver][reference]") {
  auto p = small_problem();
  double tr = 0;
  for (double v : p.k.diagonal()) tr += v;
  CHECK_THAT(tr, WithinRel(21824922.387138005, 1e-12));
  kika::fem::SolveInfo info;
  const auto u = solve_full(p, &info);
  CHECK(info.rel_residual < 1e-7);
  double umax = 0, uxmax = -1e300, uzmin = 1e300;
  for (std::size_t i = 0; i < u.size(); ++i) {
    umax = std::max(umax, std::abs(u[i]));
    if (i % 3 == 0) uxmax = std::max(uxmax, u[i]);
    if (i % 3 == 2) uzmin = std::min(uzmin, u[i]);
  }
  // прототип решает эту задачу прямым методом; итерационный — до 1e-7 по невязке
  CHECK_THAT(umax, WithinRel(0.02850792858660481, 1e-6));
  CHECK_THAT(uxmax, WithinRel(0.02850792858660481, 1e-6));
  CHECK_THAT(uzmin, WithinRel(-0.00612077481291269, 1e-6));

  // невязка K·u = f по свободным степеням свободы
  const auto ku = p.k.multiply(u);
  double rmax = 0, fmax = 0;
  for (std::size_t i = 0; i < u.size(); ++i)
    if (!p.fixed[i]) {
      rmax = std::max(rmax, std::abs(ku[i] - p.f[i]));
      fmax = std::max(fmax, std::abs(p.f[i]));
    }
  CHECK(rmax < 1e-5 * fmax);
}

TEST_CASE("Результат не зависит от числа потоков", "[fem][solver][parallel]") {
  const int saved = kika::thread_count();
  kika::set_thread_count(1);
  auto p1 = small_problem();
  const auto u1 = solve_full(p1);
  kika::set_thread_count(3);
  auto p3 = small_problem();
  const auto u3 = solve_full(p3);
  kika::set_thread_count(saved);
  REQUIRE(p1.k.val.size() == p3.k.val.size());
  CHECK(p1.k.val == p3.k.val);
  CHECK(u1 == u3);
}

TEST_CASE("Незакреплённая деталь — понятная ошибка решателя", "[fem][solver]") {
  auto p = small_problem();
  std::fill(p.fixed.begin(), p.fixed.end(), 0);
  // без закреплений и без пружин матрица вырождена — итерации не сходятся
  kika::fem::Solver s(p.k, p.fixed, p.mesh, 1e-7, 0.0, 60);
  std::vector<double> rf(s.free_dofs().size(), 0.0);
  rf[0] = 1.0;
  CHECK_THROWS_AS(s.solve(rf), std::runtime_error);
}
