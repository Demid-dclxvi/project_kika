#include <array>
#include <string>

#include "kika/gcode/toolpaths.hpp"
#include "util/text.hpp"

namespace kika::gcode {

namespace {

bool contains(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

constexpr std::array<std::string_view, 11> kExcludeKeys = {
    "support", "skirt", "brim",  "tower", "custom",   "ironing",
    "purge",   "flush", "prime", "wipe",  "undefined",
};

}  // namespace

Role classify_role(std::string_view type_label) {
  const std::string s = util::to_lower(util::trim(type_label));
  if (s.empty()) return Role::Unknown;
  for (std::string_view k : kExcludeKeys)
    if (contains(s, k)) return Role::Excluded;
  if (contains(s, "outer") || contains(s, "external")) return Role::OuterWall;
  if (contains(s, "wall") || contains(s, "perimeter") || s.starts_with("inner")) return Role::InnerWall;
  if (contains(s, "sparse") || s == "fill" || s == "infill" || s == "internal infill" ||
      (contains(s, "infill") && !contains(s, "solid") && !contains(s, "bridge") && !contains(s, "gap")))
    return Role::Sparse;
  for (std::string_view k : {"top", "bottom", "skin", "solid", "bridge", "gap"})
    if (contains(s, k)) return Role::Solid;
  return Role::Unknown;
}

std::string_view role_name(Role role) {
  switch (role) {
    case Role::Excluded: return "Не относится к детали";
    case Role::OuterWall: return "Внешняя стенка";
    case Role::InnerWall: return "Внутренняя стенка";
    case Role::Solid: return "Сплошное заполнение / верх / низ";
    case Role::Sparse: return "Разреженное заполнение";
    case Role::Unknown: return "Без типа";
  }
  return "?";
}

std::string_view role_key(Role role) {
  switch (role) {
    case Role::Excluded: return "excluded";
    case Role::OuterWall: return "outer_wall";
    case Role::InnerWall: return "inner_wall";
    case Role::Solid: return "solid";
    case Role::Sparse: return "sparse";
    case Role::Unknown: return "unknown";
  }
  return "?";
}

}  // namespace kika::gcode
