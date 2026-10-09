// Эталон: примеры из прототипа. Числа получены прототипом на Python
// (prototype/fdmfea/gcode.py) и должны совпадать с C++-версией.

#include <filesystem>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/gcode/parser.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using kika::gcode::Role;

namespace {

std::filesystem::path example(const char* name) {
  return std::filesystem::path(KIKA_SOURCE_DIR) / "prototype" / "examples" / name;
}

struct RoleRef {
  Role role;
  std::size_t count;
  double volume;
};

struct ExampleRef {
  const char* file;
  std::size_t segments;
  double volume;
  double excluded;
  double used;
  int layers;
  std::size_t lines;
  double bbox_min[3];
  double bbox_max[3];
  RoleRef roles[4];
};

const ExampleRef kExamples[] = {
    {"bracket_side.gcode",
     25112,
     11680.710335692162,
     40.176625805822255,
     9876.978539999482,
     150,
     46956,
     {93.225, 103.225, 0.0},
     {162.775, 152.775, 30.0},
     {{Role::OuterWall, 5961, 2890.2627805519887},
      {Role::InnerWall, 11889, 5583.475777905336},
      {Role::Solid, 2144, 888.9084532874888},
      {Role::Sparse, 5118, 2318.063323947348}}},
    {"bracket_upright.gcode",
     24131,
     11222.458077279656,
     33.803542843112375,
     8747.809769999716,
     350,
     43917,
     {103.225, 113.225, 0.0},
     {152.775, 142.775, 70.0},
     {{Role::OuterWall, 6264, 2363.1858827679275},
      {Role::InnerWall, 12484, 4447.42937437813},
      {Role::Solid, 1713, 1904.9494992157988},
      {Role::Sparse, 3670, 2506.8933209177994}}},
};

}  // namespace

TEST_CASE("Примеры прототипа: результат совпадает с Python", "[gcode][reference]") {
  for (const auto& ref : kExamples) {
    INFO(ref.file);
    const auto tp = kika::gcode::load(example(ref.file));
    const auto& i = tp.info;
    CHECK(i.slicer == "OrcaSlicer");
    CHECK(i.filament_type == "PLA");
    CHECK(i.infill_pattern == "grid");
    CHECK_THAT(*i.infill_density, WithinAbs(0.2, 1e-12));
    CHECK(i.wall_loops == 3);
    CHECK(i.top_layers == 4);
    CHECK(i.bottom_layers == 4);
    CHECK(i.nozzle_diameter == 0.4);
    CHECK(i.line_width == 0.45);
    CHECK(i.layer_height == 0.2);
    CHECK(i.first_layer_height == 0.2);
    CHECK(i.warnings.empty());

    CHECK(tp.size() == ref.segments);
    CHECK(i.n_lines == ref.lines);
    CHECK(i.n_layers == ref.layers);
    CHECK_THAT(tp.total_volume(), WithinRel(ref.volume, 1e-12));
    CHECK_THAT(i.excluded_volume, WithinRel(ref.excluded, 1e-12));
    CHECK_THAT(i.filament_used_mm, WithinRel(ref.used, 1e-12));

    const auto bb = tp.bbox();
    for (int k = 0; k < 3; ++k) {
      CHECK_THAT(bb.min[k], WithinAbs(ref.bbox_min[k], 1e-9));
      CHECK_THAT(bb.max[k], WithinAbs(ref.bbox_max[k], 1e-9));
    }
    for (const auto& r : ref.roles) {
      std::size_t count = 0;
      double volume = 0;
      for (const auto& s : tp.segments)
        if (s.role == r.role) {
          ++count;
          volume += s.volume;
        }
      CHECK(count == r.count);
      CHECK_THAT(volume, WithinRel(r.volume, 1e-12));
    }
    for (const auto& s : tp.segments) {
      REQUIRE(s.h == 0.2);
      REQUIRE(s.layer >= 0);
      REQUIRE(s.layer < ref.layers);
    }
  }
}
