// Воксельная модель: перенос fdmfea/voxelize.py. Эталонные числа получены прототипом;
// поэлементная сверка всех массивов — в tools/compare_with_prototype.py.

#include <cmath>
#include <filesystem>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/gcode/parser.hpp"
#include "kika/voxel/voxel_model.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::gcode::Role;

namespace {

kika::gcode::Toolpaths example(const char* name) {
  return kika::gcode::load(std::filesystem::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / name);
}

}  // namespace

TEST_CASE("Воксели кронштейнов совпадают с прототипом", "[voxel][reference]") {
  struct Ref {
    const char* file;
    std::size_t n;
    int nx, ny, nz;
    double s, deposited, model_volume;
    std::size_t outer, inner, solid, sparse;
  };
  const Ref refs[] = {
      {"bracket_side.gcode", 109716, 118, 85, 50, 0.6024854788533571, 11680.71033185879, 11641.85932587522,
       19823, 27523, 5270, 57100},
      {"bracket_upright.gcode", 110999, 86, 52, 117, 0.5969344035031939, 11222.458070335299, 11207.33416608267,
       16370, 20780, 12420, 61429},
  };
  for (const auto& r : refs) {
    INFO(r.file);
    const auto vm = kika::voxel::voxelize(example(r.file));
    REQUIRE(vm.size() == r.n);
    CHECK(vm.nx == r.nx);
    CHECK(vm.ny == r.ny);
    CHECK(vm.nz == r.nz);
    CHECK_THAT(vm.s, WithinRel(r.s, 1e-12));
    CHECK_THAT(vm.stats.deposited_volume, WithinRel(r.deposited, 1e-10));
    CHECK_THAT(vm.stats.model_volume, WithinRel(r.model_volume, 1e-10));
    CHECK(vm.stats.removed_fraction == 0.0);
    CHECK_THAT(vm.infill_density, WithinAbs(0.2, 1e-12));
    std::size_t cnt[6] = {};
    for (auto role : vm.role) ++cnt[static_cast<int>(role)];
    CHECK(cnt[static_cast<int>(Role::OuterWall)] == r.outer);
    CHECK(cnt[static_cast<int>(Role::InnerWall)] == r.inner);
    CHECK(cnt[static_cast<int>(Role::Solid)] == r.solid);
    CHECK(cnt[static_cast<int>(Role::Sparse)] == r.sparse);
  }
}

TEST_CASE("Свойства воксельной модели", "[voxel]") {
  const auto tp = example("bracket_side.gcode");
  kika::voxel::Options opt;
  opt.max_elems = 25000;
  const auto vm = kika::voxel::voxelize(tp, opt);
  CHECK(vm.size() > 15000);
  CHECK(vm.size() < 30000);
  // объём не теряется: всё, что разложено, — в модели (с точностью до отброшенных крошек)
  CHECK_THAT(vm.stats.model_volume, WithinRel(vm.stats.deposited_volume, 0.02));
  for (std::size_t e = 0; e < vm.size(); ++e) {
    REQUIRE(vm.rho(e) <= 1.0 + 1e-12);
    REQUIRE(vm.rho(e) > 0.0);
    double hs = 0, hp = 0;
    for (double v : vm.shell_hist(e)) hs += v;
    for (double v : vm.sparse_hist(e)) hp += v;
    if (vm.rho_shell[e] > 0) REQUIRE_THAT(hs, WithinAbs(1.0, 1e-9));
    if (vm.rho_sparse[e] > 0) REQUIRE_THAT(hp, WithinAbs(1.0, 1e-9));
  }
  // границы вокселей по Z совпадают с границами слоёв печати (кратны 0,2 мм)
  for (double z : vm.z_edges) CHECK_THAT(z / 0.2, WithinAbs(std::round(z / 0.2), 1e-9));
  // плоские номера идут по возрастанию
  for (std::size_t e = 1; e < vm.size(); ++e) REQUIRE(vm.flat(e) > vm.flat(e - 1));
}

TEST_CASE("Слишком мелкий воксель — понятная ошибка", "[voxel]") {
  kika::voxel::Options opt;
  opt.voxel = 0.01;
  CHECK_THROWS_AS(kika::voxel::voxelize(example("bracket_side.gcode"), opt), std::length_error);
}
