// Принтеры и пластики для своего слайсера.

#include <algorithm>
#include <cctype>

#include "kika/slicer/slicer.hpp"

namespace kika::slicer {

namespace {

// Стартовый код Marlin: нагрев, парковка, полоса прочистки у левого края стола.
// Перед печатью сверить с кодом своего принтера из его слайсера.
const char* kMarlinStart = R"(M140 S{first_layer_bed_temp}
M104 S{first_layer_nozzle_temp}
G28
M190 S{first_layer_bed_temp}
M109 S{first_layer_nozzle_temp}
G90
M83
G92 E0
G1 Z5 F3000
G1 X3 Y20 F6000
G1 Z0.3 F600
G1 X3 Y120 E10 F1200
G1 X3.6 Y120 F3000
G1 X3.6 Y20 E10 F1200
G1 Z2 F600
G92 E0)";

const char* kMarlinEnd = R"(M104 S0
M140 S0
M107
G91
G1 E-2 F2400
G1 Z10 F600
G90
G1 X5 Y{bed_y} F6000
M84)";

// QIDI Q2 (Klipper). Макрос PRINT_START прошивки сам паркует, выравнивает стол (Z_TILT_ADJUST),
// снимает карту стола (только под деталями, если они размечены EXCLUDE_OBJECT_DEFINE), чистит сопло
// и греет стол, камеру и сопло до BED, CHAMBER, HOTEND. Остальное — своё: подтверждаем температуры
// и печатаем полосу прочистки вдоль переднего края (левый передний угол 11 × 16 мм не трогаем —
// там механизм реза и чистки сопла).
const char* kQidiQ2Start = R"(PRINT_START BED={first_layer_bed_temp} HOTEND={first_layer_nozzle_temp} CHAMBER={chamber_temp}
SET_PRINT_STATS_INFO TOTAL_LAYER={total_layers}
M140 S{first_layer_bed_temp}
M141 S{chamber_temp}
M104 S{first_layer_nozzle_temp}
M190 S{first_layer_bed_temp}
M109 S{first_layer_nozzle_temp}
G90
M83
G92 E0
G1 X200 Y2 F12000
G1 Z0.3 F600
G1 E2 F300
G1 X140 Y2 E7 F1800
G1 Y3.2 F3000
G1 X190 Y3.2 E3.5 F3000
G1 Z1 F600
G92 E0)";

// Конец: отвести сопло, опустить стол, остудить; PRINT_END прошивки выключает вентиляторы,
// нагреватели и моторы.
const char* kQidiQ2End = R"(G1 E-2 F1800
G1 Z{lift_z} F600
G1 X90 Y265 F12000
G1 Z{park_z} F600
M104 S0
M140 S0
M141 S0
PRINT_END)";

const char* kKlipperLayer = "SET_PRINT_STATS_INFO CURRENT_LAYER={layer_num}";

}  // namespace

const std::vector<Printer>& printers() {
  static const std::vector<Printer> list = [] {
    Printer q2;  // значения по умолчанию в Printer — QIDI Q2
    q2.start_gcode = kQidiQ2Start;
    q2.end_gcode = kQidiQ2End;
    q2.layer_gcode = kKlipperLayer;

    Printer i7;
    i7.key = "sparkx_i7";
    i7.name = "Creality SPARKX i7";
    i7.flavor = Flavor::Marlin;
    i7.bed_x = 260;
    i7.bed_y = 260;
    i7.max_z = 255;
    i7.max_nozzle_temp = 300;
    i7.max_bed_temp = 100;
    i7.max_chamber_temp = 0;
    i7.max_speed = 300;
    i7.max_accel = 5000;
    i7.corner_speed = 10;
    i7.max_speed_z = 10;
    i7.retract_length = 0.8;
    i7.retract_speed = 40;
    i7.retract_min_travel = 2.0;
    i7.z_hop = 0;
    i7.aux_fan = false;
    i7.air_filtration = false;
    i7.nozzle_type = NozzleType::Brass;
    i7.bed_exclude.clear();
    i7.pause_gcode = "M601";
    i7.start_gcode = kMarlinStart;
    i7.end_gcode = kMarlinEnd;

    Printer generic = i7;
    generic.key = "generic_220";
    generic.name = "Принтер 220×220 (Marlin)";
    generic.bed_x = 220;
    generic.bed_y = 220;
    generic.max_z = 250;
    generic.max_bed_temp = 110;
    generic.max_nozzle_temp = 260;
    generic.max_speed = 150;
    generic.max_accel = 1500;
    return std::vector<Printer>{q2, i7, generic};
  }();
  return list;
}

const Printer& default_printer() { return printers().front(); }

const Printer* find_printer(std::string_view key) {
  for (const auto& p : printers())
    if (p.key == key) return &p;
  return nullptr;
}

std::string_view flavor_key(Flavor f) { return f == Flavor::Klipper ? "klipper" : "marlin"; }

std::optional<Flavor> flavor_from_key(std::string_view key) {
  std::string k;
  for (char c : key) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (k == "klipper") return Flavor::Klipper;
  if (k == "marlin" || k == "marlin2" || k == "reprap" || k == "reprapfirmware") return Flavor::Marlin;
  return std::nullopt;
}

namespace {

// Обычные пластики: значения — типичные для принтера с закрытой камерой и соплом 0,4 мм.
// тип, сопло, сопло 1-го слоя, стол, стол 1-го слоя, камера, поток, мм³/с, мм/с, плотность,
// обдув мин/макс, слоёв без обдува, полный обдув при слое короче, замедление при слое короче,
// обдув нависаний, боковой обдув, рекомендуемая температура сопла от и до, с волокном
struct FilamentRow {
  const char* type;
  double nozzle, nozzle1, bed, bed1, chamber, flow, vol, speed, density;
  double fan_min, fan_max;
  int fan_off;
  double full_time, slow_time, overhang, aux;
  double t_min, t_max;
  bool abrasive;
};

constexpr FilamentRow kFilaments[] = {
    {"PLA", 210, 215, 60, 60, 0, 0.98, 21, 500, 1.24, 1.0, 1.0, 1, 60, 8, 1.0, 0.7, 190, 240, false},
    {"PLA-CF", 220, 225, 60, 60, 0, 0.98, 15, 400, 1.29, 1.0, 1.0, 1, 60, 8, 1.0, 0.5, 210, 240, true},
    {"PETG", 240, 245, 70, 70, 0, 0.95, 12, 300, 1.27, 0.3, 0.5, 1, 20, 8, 0.9, 0, 220, 260, false},
    {"PETG-CF", 255, 255, 80, 80, 0, 0.95, 11, 250, 1.25, 0.05, 0.4, 3, 30, 6, 1.0, 0, 240, 270, true},
    {"ABS", 250, 255, 100, 100, 50, 0.95, 18, 400, 1.04, 0.1, 0.4, 3, 30, 4, 0.8, 0, 240, 270, false},
    {"ASA", 255, 255, 100, 100, 55, 0.92, 16, 400, 1.07, 0.1, 0.5, 3, 40, 4, 0.8, 0, 240, 280, false},
    {"PC", 270, 270, 100, 100, 60, 0.95, 10, 250, 1.20, 0.0, 0.3, 3, 30, 6, 0.6, 0, 260, 300, false},
    {"PA", 260, 260, 80, 80, 0, 0.96, 8, 200, 1.14, 0.0, 0.3, 3, 20, 8, 0.4, 0, 250, 290, false},
    {"PA-CF", 280, 280, 80, 80, 0, 0.96, 8, 200, 1.25, 0.2, 0.4, 3, 10, 8, 0.4, 0, 260, 300, true},
    {"TPU", 225, 225, 45, 45, 0, 1.0, 3.6, 40, 1.21, 1.0, 1.0, 1, 60, 8, 1.0, 0, 200, 250, false},
};

Filament from_row(const FilamentRow& r) {
  Filament f;
  f.name = r.type;
  f.type = r.type;
  f.nozzle_temp = r.nozzle;
  f.first_layer_nozzle_temp = r.nozzle1;
  f.bed_temp = r.bed;
  f.first_layer_bed_temp = r.bed1;
  f.chamber_temp = r.chamber;
  f.flow = r.flow;
  f.max_volumetric = r.vol;
  f.max_speed = r.speed;
  f.density = r.density;
  f.fan_min = r.fan_min;
  f.fan_max = r.fan_max;
  f.fan_off_layers = r.fan_off;
  f.fan_full_time = r.full_time;
  f.slowdown_time = r.slow_time;
  f.overhang_fan = r.overhang;
  f.aux_fan = r.aux;
  f.nozzle_temp_min = r.t_min;
  f.nozzle_temp_max = r.t_max;
  f.abrasive = r.abrasive;
  return f;
}

}  // namespace

const std::vector<Filament>& filaments() {
  static const std::vector<Filament> list = [] {
    std::vector<Filament> v;
    for (const auto& r : kFilaments) v.push_back(from_row(r));
    return v;
  }();
  return list;
}

Filament filament_preset(std::string_view type) {
  std::string t(type);
  for (char& c : t) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  for (const auto& f : filaments())
    if (f.type == t) return f;
  Filament f = filaments().front();  // неизвестный — как PLA, но со своим названием
  if (!type.empty()) {
    f.type = std::string(type);
    f.name = std::string(type);
  }
  return f;
}

namespace {

struct PatternInfo {
  Pattern p;
  const char* key;
  const char* name;
};

constexpr PatternInfo kPatterns[] = {
    {Pattern::Grid, "grid", "Сетка"},
    {Pattern::Gyroid, "gyroid", "Гироид"},
    {Pattern::Cubic, "cubic", "Кубы"},
    {Pattern::Honeycomb, "honeycomb", "Соты"},
    {Pattern::Honeycomb3D, "3dhoneycomb", "3D-соты"},
    {Pattern::Triangles, "triangles", "Треугольники"},
    {Pattern::TriHexagon, "tri-hexagon", "Тригексагон"},
    {Pattern::Rectilinear, "rectilinear", "Прямые через слой"},
    {Pattern::Lines, "line", "Линии"},
    {Pattern::Concentric, "concentric", "Концентрическое"},
};

}  // namespace

std::string_view pattern_key(Pattern p) {
  for (const auto& x : kPatterns)
    if (x.p == p) return x.key;
  return "grid";
}

std::string_view pattern_name(Pattern p) {
  for (const auto& x : kPatterns)
    if (x.p == p) return x.name;
  return "Сетка";
}

const std::vector<Pattern>& patterns() {
  static const std::vector<Pattern> list = [] {
    std::vector<Pattern> v;
    for (const auto& x : kPatterns) v.push_back(x.p);
    return v;
  }();
  return list;
}

std::optional<Pattern> pattern_from_key(std::string_view key) {
  std::string k;
  for (char c : key)
    if (c != '_' && c != ' ' && c != '-') k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  for (const auto& x : kPatterns) {
    std::string kk;
    for (const char* c = x.key; *c; ++c)
      if (*c != '-') kk.push_back(*c);
    if (k == kk) return x.p;
  }
  if (k == "zigzag" || k == "alignedrectilinear") return Pattern::Rectilinear;
  if (k == "lines") return Pattern::Lines;
  if (k == "trihexagon" || k == "stars") return Pattern::TriHexagon;
  if (k == "honeycomb3d") return Pattern::Honeycomb3D;
  return std::nullopt;
}

std::string_view support_key(Support s) {
  switch (s) {
    case Support::None: return "none";
    case Support::Normal: return "normal";
    case Support::Tree: return "tree";
  }
  return "none";
}

std::string_view support_name(Support s) {
  switch (s) {
    case Support::None: return "Без поддержек";
    case Support::Normal: return "Обычные";
    case Support::Tree: return "Древовидные";
  }
  return "Без поддержек";
}

namespace {

struct SurfaceInfo {
  SurfacePattern p;
  const char* key;
  const char* name;
};
constexpr SurfaceInfo kSurfaces[] = {
    {SurfacePattern::Monotonic, "monotonic", "Монотонный"},
    {SurfacePattern::MonotonicLine, "monotonicline", "Монотонный без соединений"},
    {SurfacePattern::Rectilinear, "rectilinear", "Прямолинейный (зигзаг)"},
    {SurfacePattern::Concentric, "concentric", "Концентрический"},
};

std::string lower_key(std::string_view key) {
  std::string k;
  for (char c : key) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return k;
}

}  // namespace

std::string_view surface_key(SurfacePattern p) {
  for (const auto& x : kSurfaces)
    if (x.p == p) return x.key;
  return "monotonic";
}

std::string_view surface_name(SurfacePattern p) {
  for (const auto& x : kSurfaces)
    if (x.p == p) return x.name;
  return "Монотонный";
}

std::optional<SurfacePattern> surface_from_key(std::string_view key) {
  const std::string k = lower_key(key);
  for (const auto& x : kSurfaces)
    if (k == x.key) return x.p;
  if (k == "monotonic_line" || k == "monotonic line") return SurfacePattern::MonotonicLine;
  if (k == "zig-zag" || k == "zigzag" || k == "alignedrectilinear") return SurfacePattern::Rectilinear;
  return std::nullopt;
}

const std::vector<SurfacePattern>& surface_patterns() {
  static const std::vector<SurfacePattern> list{SurfacePattern::Monotonic, SurfacePattern::MonotonicLine,
                                                SurfacePattern::Rectilinear, SurfacePattern::Concentric};
  return list;
}

std::string_view ironing_key(Ironing i) {
  switch (i) {
    case Ironing::None: return "none";
    case Ironing::Top: return "top";
    case Ironing::Topmost: return "topmost";
  }
  return "none";
}

std::string_view ironing_name(Ironing i) {
  switch (i) {
    case Ironing::None: return "Без глажки";
    case Ironing::Top: return "Все верхние поверхности";
    case Ironing::Topmost: return "Только самый верх";
  }
  return "Без глажки";
}

std::optional<Ironing> ironing_from_key(std::string_view key) {
  const std::string k = lower_key(key);
  if (k == "none" || k == "no" || k == "no ironing" || k.empty()) return Ironing::None;
  if (k == "top" || k == "top surfaces") return Ironing::Top;
  if (k == "topmost" || k == "topmost surface") return Ironing::Topmost;
  return std::nullopt;
}

std::string_view seam_key(Seam s) {
  switch (s) {
    case Seam::Aligned: return "aligned";
    case Seam::Nearest: return "nearest";
    case Seam::Rear: return "rear";
    case Seam::Random: return "random";
  }
  return "aligned";
}

std::string_view seam_name(Seam s) {
  switch (s) {
    case Seam::Aligned: return "Друг над другом";
    case Seam::Nearest: return "Ближе к соплу";
    case Seam::Rear: return "Сзади";
    case Seam::Random: return "Случайно";
  }
  return "Друг над другом";
}

std::optional<Seam> seam_from_key(std::string_view key) {
  std::string k;
  for (char c : key) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (k == "aligned") return Seam::Aligned;
  if (k == "nearest") return Seam::Nearest;
  if (k == "rear" || k == "back") return Seam::Rear;
  if (k == "random") return Seam::Random;
  return std::nullopt;
}

std::string_view nozzle_type_key(NozzleType t) {
  switch (t) {
    case NozzleType::Brass: return "brass";
    case NozzleType::Stainless: return "stainless_steel";
    case NozzleType::Hardened: return "hardened_steel";
  }
  return "brass";
}

std::string_view nozzle_type_name(NozzleType t) {
  switch (t) {
    case NozzleType::Brass: return "Латунь";
    case NozzleType::Stainless: return "Нержавеющая сталь";
    case NozzleType::Hardened: return "Закалённая сталь";
  }
  return "Латунь";
}

std::optional<NozzleType> nozzle_type_from_key(std::string_view key) {
  const std::string k = lower_key(key);
  if (k == "brass" || k == "undefine" || k == "undefined") return NozzleType::Brass;
  if (k == "stainless_steel" || k == "stainless") return NozzleType::Stainless;
  if (k == "hardened_steel" || k == "hardened" || k == "tungsten_carbide") return NozzleType::Hardened;
  return std::nullopt;
}

std::string_view fuzzy_key(FuzzySkin f) {
  switch (f) {
    case FuzzySkin::None: return "none";
    case FuzzySkin::Contour: return "external";
    case FuzzySkin::External: return "all";
    case FuzzySkin::All: return "allwalls";
  }
  return "none";
}

std::string_view fuzzy_name(FuzzySkin f) {
  switch (f) {
    case FuzzySkin::None: return "Нет";
    case FuzzySkin::Contour: return "Наружный контур";
    case FuzzySkin::External: return "Контур и отверстия";
    case FuzzySkin::All: return "Все стенки";
  }
  return "Нет";
}

std::optional<FuzzySkin> fuzzy_from_key(std::string_view key) {
  const std::string k = lower_key(key);
  if (k == "none" || k == "no" || k.empty()) return FuzzySkin::None;
  if (k == "external" || k == "contour") return FuzzySkin::Contour;
  if (k == "all" || k == "contour_and_hole") return FuzzySkin::External;
  if (k == "allwalls" || k == "all_walls") return FuzzySkin::All;
  return std::nullopt;
}

std::string_view brim_key(BrimType b) {
  switch (b) {
    case BrimType::Outer: return "outer_only";
    case BrimType::Inner: return "inner_only";
    case BrimType::Both: return "outer_and_inner";
    case BrimType::None: return "no_brim";
  }
  return "outer_only";
}

std::string_view brim_name(BrimType b) {
  switch (b) {
    case BrimType::Outer: return "Снаружи";
    case BrimType::Inner: return "Внутри отверстий";
    case BrimType::Both: return "Снаружи и внутри";
    case BrimType::None: return "Без каймы";
  }
  return "Снаружи";
}

std::optional<BrimType> brim_from_key(std::string_view key) {
  const std::string k = lower_key(key);
  if (k == "outer_only" || k == "outer" || k == "auto_brim" || k == "brim_ears") return BrimType::Outer;
  if (k == "inner_only" || k == "inner") return BrimType::Inner;
  if (k == "outer_and_inner" || k == "both") return BrimType::Both;
  if (k == "no_brim" || k == "none") return BrimType::None;
  return std::nullopt;
}

std::optional<Support> support_from_key(std::string_view key) {
  std::string k;
  for (char c : key) k.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  if (k == "none" || k == "off" || k == "false" || k.empty()) return Support::None;
  if (k == "normal" || k == "grid" || k == "normal(auto)" || k == "default") return Support::Normal;
  if (k == "tree" || k == "tree(auto)" || k == "organic" || k == "trees") return Support::Tree;
  return std::nullopt;
}

}  // namespace kika::slicer
