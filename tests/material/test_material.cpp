// Материалы: перенос fdmfea/materials.py. Эталонные числа получены прототипом.

#include <cmath>
#include <numbers>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/material/material.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace kika::material;

namespace {

const Material& pla() {
  const Material* m = find_material("PLA");
  REQUIRE(m != nullptr);
  return *m;
}

}  // namespace

TEST_CASE("Встроенная база материалов", "[material]") {
  CHECK(builtin_materials().size() == 10);
  for (const auto& m : builtin_materials()) {
    INFO(m.key);
    CHECK(m.E1 > 0);
    CHECK(m.Zt > 0);
    CHECK_NOTHROW(stiffness(m));
  }
  CHECK(find_material("НЕТ ТАКОГО") == nullptr);
}

TEST_CASE("Жёсткость — обратная к податливости и симметричная", "[material]") {
  const auto& m = pla();
  const Mat6 c = stiffness(m);
  for (std::size_t i = 0; i < 6; ++i)
    for (std::size_t j = 0; j < 6; ++j) CHECK_THAT(c(i, j), WithinAbs(c(j, i), 1e-9));
  // одноосное растяжение вдоль нити: σ1 = E1·ε1 при свободных поперечных деформациях
  const auto s = kika::linalg::inverse(c);
  REQUIRE(s.has_value());
  CHECK_THAT(1.0 / (*s)(0, 0), WithinRel(m.E1, 1e-12));
  CHECK_THAT(1.0 / (*s)(2, 2), WithinRel(m.E3, 1e-12));
  CHECK_THAT(1.0 / (*s)(5, 5), WithinRel(m.G12, 1e-12));
  CHECK_THROWS(orthotropic_stiffness(1000, 1000, 1000, 400, 400, 400, 0.9, 0.9, 0.9));
}

TEST_CASE("Поворот жёсткости вокруг Z", "[material]") {
  const Mat6 c = stiffness(pla());
  const Mat6 r0 = rotate_z(c, 0.0);
  const Mat6 r90 = rotate_z(c, std::numbers::pi / 2);
  for (std::size_t i = 0; i < 36; ++i) CHECK_THAT(r0.a[i], WithinAbs(c.a[i], 1e-9));
  CHECK_THAT(r90(0, 0), WithinRel(c(1, 1), 1e-12));  // ось нити теперь вдоль Y
  CHECK_THAT(r90(1, 1), WithinRel(c(0, 0), 1e-12));
  CHECK_THAT(r90(2, 2), WithinRel(c(2, 2), 1e-12));
  CHECK_THAT(r90(3, 3), WithinRel(c(4, 4), 1e-12));
}

TEST_CASE("Критерий Хоффмана на одноосных состояниях", "[material]") {
  const auto& m = pla();
  const auto hc = hoffman_coeffs(m);
  CHECK_THAT(hoffman_sf({m.Xt, 0, 0, 0, 0, 0}, hc), WithinRel(1.0, 1e-12));
  CHECK_THAT(hoffman_sf({-m.Xc, 0, 0, 0, 0, 0}, hc), WithinRel(1.0, 1e-12));
  CHECK_THAT(hoffman_sf({0, 0, m.Zt, 0, 0, 0}, hc), WithinRel(1.0, 1e-12));
  CHECK_THAT(hoffman_sf({0, 0, 0, 0, 0, m.S12 / 2}, hc), WithinRel(2.0, 1e-12));
  CHECK(hoffman_sf({0, 0, 0, 0, 0, 0}, hc) == 1e6);
  CHECK(failure_mode({0, 0, m.Zt, 0, 0, 0}, m) == FailureMode::InterlayerTension);
  CHECK(failure_mode({-m.Xc, 0, 0, 0, 0, 0}, m) == FailureMode::FiberCompression);
  CHECK(failure_mode({0, 0, 0, 0, m.S13, 0}, m) == FailureMode::InterlayerShear);
}

TEST_CASE("Образцы ±45° и поперёк слоёв — как в прототипе", "[material][reference]") {
  const auto l45 = laminate_check(pla(), {45, -45}, 0);
  CHECK_THAT(l45.strength, WithinRel(41.68469440787639, 1e-12));
  CHECK_THAT(l45.modulus, WithinRel(2683.7455830388685, 1e-12));
  const auto lz = laminate_check(pla(), {0, 90}, 2);
  CHECK(lz.strength == 30.0);
  CHECK_THAT(lz.modulus, WithinRel(2203.336143926448, 1e-12));
  const auto petg = laminate_check(*find_material("PETG"), {45, -45}, 0);
  CHECK_THAT(petg.strength, WithinRel(40.874682228550824, 1e-12));
  CHECK_THAT(petg.modulus, WithinRel(1910.3172465239647, 1e-12));
}

TEST_CASE("Материал по типу пластика из G-code", "[material]") {
  CHECK(guess_material("PLA") == "PLA");
  CHECK(guess_material("PLA+") == "PLA");
  CHECK(guess_material("PETG-CF") == "PETG-CF");
  CHECK(guess_material("PA6-CF") == "PA-CF");
  CHECK(guess_material("PCTG") == "PETG");
  CHECK(guess_material("TPU 95A") == "TPU");
  CHECK(guess_material("eSUN PLA+") == "PLA");
  CHECK(guess_material("Generic PETG") == "PETG");
  CHECK_FALSE(guess_material("WOOD").has_value());
  CHECK_FALSE(guess_material("").has_value());
}

TEST_CASE("Поправки на температуру и усталость", "[material]") {
  const auto& m = pla();
  CHECK(temperature_factor(m, std::nullopt).factor == 1.0);
  CHECK(temperature_factor(m, kRefTemperature).factor == 1.0);
  CHECK_THAT(temperature_factor(m, m.hdt).factor, WithinRel(0.6, 1e-12));
  const auto hot = temperature_factor(m, m.hdt + 10);
  CHECK(hot.factor < 0.6);
  CHECK(hot.warning.has_value());
  CHECK(fatigue_factor(m, 1.0) == 1.0);
  CHECK_THAT(fatigue_factor(m, 1e6), WithinRel(std::pow(1e6, -1.0 / m.fatigue_k), 1e-12));
  CHECK(infill_kz("Gyroid") == 0.5);
  CHECK(infill_kz("3D honeycomb") == 0.5);
  CHECK(infill_kz("незнакомый") == 0.6);
}
