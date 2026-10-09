// Перенос test_roles из prototype/tests/test_gcode.py

#include <catch2/catch_test_macros.hpp>

#include "kika/gcode/toolpaths.hpp"

using kika::gcode::classify_role;
using kika::gcode::Role;

TEST_CASE("Роль по подписи типа линии", "[gcode][roles]") {
  CHECK(classify_role("Outer wall") == Role::OuterWall);
  CHECK(classify_role("External perimeter") == Role::OuterWall);
  CHECK(classify_role("WALL-OUTER") == Role::OuterWall);
  CHECK(classify_role("Inner wall") == Role::InnerWall);
  CHECK(classify_role("Overhang perimeter") == Role::InnerWall);
  CHECK(classify_role("Sparse infill") == Role::Sparse);
  CHECK(classify_role("Internal infill") == Role::Sparse);
  CHECK(classify_role("FILL") == Role::Sparse);
  CHECK(classify_role("Internal solid infill") == Role::Solid);
  CHECK(classify_role("Top surface") == Role::Solid);
  CHECK(classify_role("Bridge infill") == Role::Solid);
  CHECK(classify_role("Gap infill") == Role::Solid);
  CHECK(classify_role("SKIN") == Role::Solid);
  CHECK(classify_role("") == Role::Unknown);
  CHECK(classify_role("Something new") == Role::Unknown);
}

TEST_CASE("Служебные линии не относятся к детали", "[gcode][roles]") {
  for (const char* label : {"Support", "Support interface", "Skirt", "Brim", "Skirt/Brim", "Prime tower",
                            "Wipe tower", "Custom", "Ironing", "SUPPORT-INTERFACE"}) {
    INFO(label);
    CHECK(classify_role(label) == Role::Excluded);
  }
}
