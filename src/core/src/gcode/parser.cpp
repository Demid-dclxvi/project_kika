// Разбор G-code. Перенос fdmfea/gcode.py: логика и порядок проверок сохранены,
// чтобы результат совпадал с прототипом (сверка — tools/compare_with_prototype.py).

#include "kika/gcode/parser.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <fstream>
#include <iterator>
#include <numbers>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "util/text.hpp"

namespace kika::gcode {

namespace {

using util::to_upper;
using util::trim;

constexpr double kPi = std::numbers::pi;

// ---------------------------------------------------------------------------
// Настройки печати в комментариях разных слайсеров

enum class Attr {
  FilamentDiameter,
  FilamentType,
  FilamentDensity,
  LayerHeight,
  FirstLayerHeight,
  NozzleDiameter,
  LineWidth,
  InfillDensity,
  InfillPattern,
  WallLoops,
  TopLayers,
  BottomLayers,
};

constexpr std::array<std::pair<std::string_view, Attr>, 22> kSettingKeys = {{
    {"filament_diameter", Attr::FilamentDiameter},
    {"filament_type", Attr::FilamentType},
    {"filament_density", Attr::FilamentDensity},
    {"layer_height", Attr::LayerHeight},
    {"first_layer_height", Attr::FirstLayerHeight},
    {"initial_layer_print_height", Attr::FirstLayerHeight},
    {"nozzle_diameter", Attr::NozzleDiameter},
    {"line_width", Attr::LineWidth},
    {"extrusion_width", Attr::LineWidth},
    {"sparse_infill_density", Attr::InfillDensity},
    {"fill_density", Attr::InfillDensity},
    {"infill_sparse_density", Attr::InfillDensity},
    {"sparse_infill_pattern", Attr::InfillPattern},
    {"fill_pattern", Attr::InfillPattern},
    {"infill_pattern", Attr::InfillPattern},
    {"wall_loops", Attr::WallLoops},
    {"perimeters", Attr::WallLoops},
    {"wall_line_count", Attr::WallLoops},
    {"top_shell_layers", Attr::TopLayers},
    {"top_solid_layers", Attr::TopLayers},
    {"bottom_shell_layers", Attr::BottomLayers},
    {"bottom_solid_layers", Attr::BottomLayers},
}};

std::optional<Attr> find_setting(std::string_view key) {
  for (const auto& [k, a] : kSettingKeys)
    if (k == key) return a;
  return std::nullopt;
}

// Первое значение списка: "0.4,0.4" → "0.4"; кавычки и хвост после ';' отбрасываются.
std::string_view first_value(std::string_view v) {
  v = trim(v);
  while (!v.empty() && v.front() == '"') v.remove_prefix(1);
  while (!v.empty() && v.back() == '"') v.remove_suffix(1);
  if (auto p = v.find(','); p != std::string_view::npos) v = v.substr(0, p);
  if (auto p = v.find(';'); p != std::string_view::npos) v = v.substr(0, p);
  return trim(v);
}

std::optional<int> to_int_truncated(double v) {
  if (!std::isfinite(v) || std::abs(v) > 1e9) return std::nullopt;
  return static_cast<int>(std::trunc(v));
}

// Форматы: "key = value" (Orca, Prusa, Bambu), "Layer height: 0.2" (Cura).
void parse_setting_comment(std::string_view c, Info& info, bool& filament_diameter_set) {
  std::string_view k;
  std::string_view v;
  if (auto eq = c.find('='); eq != std::string_view::npos) {
    k = c.substr(0, eq);
    v = c.substr(eq + 1);
  } else if (auto col = c.find(':'); col != std::string_view::npos) {
    k = c.substr(0, col);
    v = c.substr(col + 1);
  } else {
    return;
  }
  std::string key = util::to_lower(trim(k));
  std::replace(key.begin(), key.end(), ' ', '_');
  const auto attr = find_setting(key);
  if (!attr) return;
  info.settings[key] = std::string(trim(v));
  const std::string_view val = first_value(v);

  switch (*attr) {
    case Attr::FilamentType:
      if (info.filament_type.empty()) info.filament_type = std::string(val);
      return;
    case Attr::InfillPattern:
      if (info.infill_pattern.empty()) info.infill_pattern = std::string(val);
      return;
    case Attr::InfillDensity: {
      if (val.ends_with('%')) {
        if (auto d = util::parse_double(val.substr(0, val.size() - 1))) info.infill_density = *d / 100.0;
      } else if (auto d = util::parse_double(val)) {
        info.infill_density = *d > 1.0 ? *d / 100.0 : *d;
      }
      return;
    }
    case Attr::WallLoops:
    case Attr::TopLayers:
    case Attr::BottomLayers: {
      const auto d = util::parse_double(val);
      if (!d) return;
      const auto n = to_int_truncated(*d);
      if (!n) return;
      if (*attr == Attr::WallLoops) info.wall_loops = n;
      if (*attr == Attr::TopLayers) info.top_layers = n;
      if (*attr == Attr::BottomLayers) info.bottom_layers = n;
      return;
    }
    default:
      break;
  }

  // Числовые настройки
  std::string_view num = val;
  while (!num.empty() && num.back() == '%') num.remove_suffix(1);
  const auto fv = util::parse_double(num);
  if (!fv) return;
  switch (*attr) {
    case Attr::LineWidth:
      if (val.ends_with('%')) return;  // ширина в процентах от сопла — не используем
      info.line_width = *fv;
      return;
    case Attr::FilamentDiameter:
      if (filament_diameter_set) return;  // берём первый (основной) пруток
      info.filament_diameter = *fv;
      filament_diameter_set = true;
      return;
    case Attr::FilamentDensity: info.filament_density = *fv; return;
    case Attr::LayerHeight: info.layer_height = *fv; return;
    case Attr::FirstLayerHeight: info.first_layer_height = *fv; return;
    case Attr::NozzleDiameter: info.nozzle_diameter = *fv; return;
    default: return;
  }
}

std::string detect_slicer(std::string_view text) {
  // Как в прототипе: ищем название в первых и последних 20 000 символах.
  constexpr std::size_t kWindow = 20000;
  std::string probe(text.substr(0, std::min(kWindow, text.size())));
  probe += text.substr(text.size() > kWindow ? text.size() - kWindow : 0);
  probe = util::to_lower(probe);
  constexpr std::array<std::pair<std::string_view, std::string_view>, 10> kNames = {{
      {"orcaslicer", "OrcaSlicer"},
      {"bambustudio", "Bambu Studio"},
      {"bambu studio", "Bambu Studio"},
      {"superslicer", "SuperSlicer"},
      {"prusaslicer", "PrusaSlicer"},
      {"cura", "Cura"},
      {"simplify3d", "Simplify3D"},
      {"ideamaker", "ideaMaker"},
      {"creality print", "Creality Print"},
      {"slic3r", "Slic3r"},
  }};
  for (const auto& [key, name] : kNames)
    if (probe.find(key) != std::string::npos) return std::string(name);
  return "unknown";
}

bool is_layer_marker_line(std::string_view line) {
  return line.starts_with(";LAYER_CHANGE") || line.starts_with("; CHANGE_LAYER") ||
         line.starts_with(";LAYER:") || line.starts_with("; layer ");
}

// ---------------------------------------------------------------------------
// Слова G-code: буква и число. Числа без экспоненты: в G-code её не бывает,
// а "X10E5" — это X=10 и E=5 (прототип читал это как X=10e5).

struct Words {
  std::array<double, 26> value{};
  std::array<bool, 26> present{};

  bool has(char letter) const { return present[static_cast<std::size_t>(letter - 'A')]; }
  double get(char letter, double fallback = 0.0) const {
    const auto i = static_cast<std::size_t>(letter - 'A');
    return present[i] ? value[i] : fallback;
  }
};

// Длина числа [-+]?(\d+\.?\d*|\.\d+) с позиции pos, 0 — если числа нет.
std::size_t number_length(std::string_view s, std::size_t pos) {
  std::size_t i = pos;
  if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
  const std::size_t digits_from = i;
  while (i < s.size() && util::is_digit(s[i])) ++i;
  if (i > digits_from) {
    if (i < s.size() && s[i] == '.') {
      ++i;
      while (i < s.size() && util::is_digit(s[i])) ++i;
    }
    return i - pos;
  }
  if (i < s.size() && s[i] == '.') {
    const std::size_t frac_from = ++i;
    while (i < s.size() && util::is_digit(s[i])) ++i;
    if (i > frac_from) return i - pos;
  }
  return 0;
}

Words scan_words(std::string_view code, std::size_t from) {
  Words w;
  std::size_t i = from;
  while (i < code.size()) {
    const char c = code[i];
    if (!util::is_alpha(c)) {
      ++i;
      continue;
    }
    std::size_t j = i + 1;
    while (j < code.size() && util::is_space(code[j])) ++j;
    const std::size_t len = number_length(code, j);
    if (len == 0) {
      ++i;
      continue;
    }
    std::string_view num = code.substr(j, len);
    if (num.front() == '+') num.remove_prefix(1);
    if (auto v = util::parse_double(num)) {
      const auto k = static_cast<std::size_t>(util::to_upper(c) - 'A');
      w.value[k] = *v;
      w.present[k] = true;
    }
    i = j + len;
  }
  return w;
}

// ---------------------------------------------------------------------------
// Дуги G2/G3: точки на дуге с хордой не длиннее seg_len (как в прототипе).

struct Point2 {
  double x;
  double y;
};

// Как numpy.linspace(start, stop, num)[k] при endpoint=True.
double linspace_at(double start, double stop, std::size_t num, std::size_t k) {
  if (num < 2) return start;
  if (k + 1 == num) return stop;
  const double div = static_cast<double>(num - 1);
  const double delta = stop - start;
  const double step = delta / div;
  if (step == 0.0) return static_cast<double>(k) / div * delta + start;
  return static_cast<double>(k) * step + start;
}

std::vector<Point2> arc_points(double x0, double y0, double x1, double y1, const Words& p, double scale,
                               bool clockwise, double seg_len) {
  double cx = 0.0;
  double cy = 0.0;
  if (p.has('R') && !(p.has('I') || p.has('J'))) {
    const double r = p.get('R') * scale;
    const double dx = x1 - x0;
    const double dy = y1 - y0;
    const double d = std::hypot(dx, dy);
    if (d < 1e-9 || std::abs(r) < d / 2 - 1e-6) return {{x0, y0}, {x1, y1}};
    const double hgt = std::sqrt(std::max(r * r - d * d / 4, 0.0));
    const double mx = (x0 + x1) / 2;
    const double my = (y0 + y1) / 2;
    const double lx = -dy / d;  // «влево» от направления хорды
    const double ly = dx / d;
    double sgn = clockwise ? -1.0 : 1.0;  // G2 — центр справа, G3 — слева (при R > 0)
    if (r < 0) sgn = -sgn;
    cx = mx + sgn * hgt * lx;
    cy = my + sgn * hgt * ly;
  } else {
    cx = x0 + p.get('I') * scale;
    cy = y0 + p.get('J') * scale;
  }
  const double r = std::hypot(x0 - cx, y0 - cy);
  const double a0 = std::atan2(y0 - cy, x0 - cx);
  const double a1 = std::atan2(y1 - cy, x1 - cx);
  double sweep = a1 - a0;
  if (clockwise) {
    if (sweep >= -1e-9) sweep -= 2 * kPi;
  } else {
    if (sweep <= 1e-9) sweep += 2 * kPi;
  }
  // Ограничение числа точек защищает от абсурдных радиусов в испорченном файле.
  constexpr double kMaxPoints = 100000.0;
  const double nd = std::ceil(std::abs(sweep) * r / seg_len);
  const std::size_t n =
      std::isfinite(nd) ? static_cast<std::size_t>(std::clamp(nd, 1.0, kMaxPoints)) : std::size_t{1};
  std::vector<Point2> pts(n + 1);
  for (std::size_t k = 0; k <= n; ++k) {
    const double ang = a0 + sweep * linspace_at(0.0, 1.0, n + 1, k);
    pts[k] = {cx + r * std::cos(ang), cy + r * std::sin(ang)};
  }
  pts.back() = {x1, y1};
  return pts;
}

// ---------------------------------------------------------------------------
// Уточнение толщины и номеров слоёв по фактическим Z.

double round3(double v) { return std::nearbyint(v * 1000.0) / 1000.0; }  // как numpy.round(v, 3)

double median(std::vector<double> v) {
  std::sort(v.begin(), v.end());
  const std::size_t n = v.size();
  if (n % 2 == 1) return v[n / 2];
  return (v[n / 2 - 1] + v[n / 2]) / 2.0;
}

void fix_layers(Toolpaths& tp) {
  Info& info = tp.info;
  auto& segs = tp.segments;
  const std::size_t n = segs.size();

  std::vector<double> zr(n);
  for (std::size_t i = 0; i < n; ++i) zr[i] = round3(segs[i].z);
  std::vector<double> uz = zr;
  std::sort(uz.begin(), uz.end());
  uz.erase(std::unique(uz.begin(), uz.end()), uz.end());

  // толщина по разнице Z между соседними слоями
  std::vector<double> dz(uz.size());
  for (std::size_t i = 0; i < uz.size(); ++i) dz[i] = uz[i] - (i == 0 ? 0.0 : uz[i - 1]);

  double lh = 0.2;
  if (info.layer_height && *info.layer_height != 0.0)
    lh = *info.layer_height;
  else if (dz.size() > 1)
    lh = median(std::vector<double>(dz.begin() + 1, dz.end()));
  if (lh <= 0) lh = 0.2;

  // слишком мелкие ступени (подъёмы сопла, «вазы») — не слои
  std::set<std::int32_t> marker_layers;
  for (const Segment& s : segs) marker_layers.insert(s.layer);
  const std::size_t n_marker = marker_layers.size();
  const bool vase = uz.size() > 4 * std::max<std::size_t>(1, n_marker) && n_marker > 1;
  if (vase)
    info.warnings.emplace_back(
        "Z меняется внутри слоёв (режим вазы или неплоская печать) — толщина слоя взята из настроек.");

  auto index_of = [&uz](double z) {
    return static_cast<std::size_t>(std::lower_bound(uz.begin(), uz.end(), z) - uz.begin());
  };
  const double h_max = 2.0 * std::max(lh, 0.05) + 0.3;
  for (std::size_t i = 0; i < n; ++i) {
    double h = segs[i].h;
    if (h <= 0) h = vase ? lh : dz[index_of(zr[i])];
    segs[i].h = std::clamp(h, 0.02, h_max);  // защита от выбросов
    segs[i].layer = static_cast<std::int32_t>(index_of(zr[i]));  // номера слоёв по уникальным Z
  }

  info.n_layers = static_cast<int>(uz.size());
  if (!info.layer_height) info.layer_height = lh;
  if (!info.first_layer_height && !uz.empty()) info.first_layer_height = uz.front();
}

}  // namespace

// ---------------------------------------------------------------------------

Toolpaths parse(std::string_view text, const ParseOptions& options) {
  Toolpaths tp;
  Info& info = tp.info;
  info.slicer = detect_slicer(text);

  // Первый проход: настройки в комментариях (обычно в начале и в конце файла) и маркеры слоёв.
  bool filament_diameter_set = false;
  bool has_layer_markers = false;
  std::size_t n_lines = 0;
  util::for_each_line(text, [&](std::string_view ln) {
    ++n_lines;
    if (!has_layer_markers && is_layer_marker_line(ln)) has_layer_markers = true;
    if (!ln.starts_with(';')) return;
    const std::string_view c = trim(ln.substr(1));
    if (!c.empty() && (c.find('=') != std::string_view::npos || c.find(':') != std::string_view::npos) &&
        c.size() < 400)
      parse_setting_comment(c, info, filament_diameter_set);
  });
  info.n_lines = n_lines;
  const double fil_area = kPi * (info.filament_diameter / 2.0) * (info.filament_diameter / 2.0);

  // Состояние принтера
  double x = 0.0, y = 0.0, z = 0.0, e = 0.0;
  bool abs_xyz = true;
  bool abs_e = true;
  bool e_mode_explicit = false;
  double scale = 1.0;
  // Если есть маркеры слоёв, всё до первого из них — стартовый код.
  bool started = !has_layer_markers;
  bool finished = false;
  Role cur_role = Role::Unknown;
  std::optional<double> cur_height;
  std::int32_t layer_idx = -1;
  bool in_wipe = false;
  double excluded_vol = 0.0;
  double used = 0.0;

  auto emit = [&](double xa, double ya, double xb, double yb, double zz, double dvol) {
    if (!started || finished || cur_role == Role::Excluded || in_wipe) {
      excluded_vol += dvol;
      return;
    }
    const double h = (cur_height && *cur_height != 0.0) ? *cur_height : -1.0;
    tp.segments.push_back(Segment{xa, ya, xb, yb, zz, h, dvol, cur_role, layer_idx});
  };

  util::for_each_line(text, [&](std::string_view raw) {
    if (raw.empty()) return;
    std::string_view code = raw;

    // Комментарии
    if (const auto sc = raw.find(';'); sc != std::string_view::npos) {
      const std::string_view cs = trim(raw.substr(sc + 1));
      code = raw.substr(0, sc);
      if (!cs.empty()) {
        const std::string up = to_upper(cs);
        const auto after_colon = [&cs]() { return cs.substr(cs.find(':') + 1); };
        if (up.starts_with("TYPE:") || up.starts_with("FEATURE:")) {
          cur_role = classify_role(after_colon());
        } else if (up.starts_with("FEATURE ")) {  // Simplify3D
          cur_role = classify_role(cs.substr(8));
        } else if (up == "LAYER_CHANGE" || up == "CHANGE_LAYER" || up.starts_with("LAYER:") ||
                   (up.starts_with("LAYER ") && up.find("Z =") != std::string::npos)) {
          if (up.starts_with("LAYER:") && info.slicer == "Cura") {
            if (auto li = util::parse_int(after_colon()); li && *li < 0)
              cur_role = Role::Excluded;  // подложка (raft) в Cura
          }
          started = true;
          ++layer_idx;
        } else if (up.starts_with("HEIGHT:") || up.starts_with("LAYER_HEIGHT:")) {
          if (auto hv = util::parse_double(after_colon())) cur_height = *hv;
        } else if (up.starts_with("WIPE_START")) {
          in_wipe = true;
        } else if (up.starts_with("WIPE_END")) {
          in_wipe = false;
        } else if (up.starts_with("MACHINE_END_GCODE_START") || up.starts_with("END OF GCODE") ||
                   up == "END GCODE" || up.starts_with("FILAMENT_GCODE_END")) {
          if (started) finished = true;
        }
      }
    }
    code = trim(code);
    if (code.empty()) return;

    // Номер строки N… и контрольная сумма *… (прототип такие строки пропускал целиком)
    if (util::to_upper(code.front()) == 'N' && code.size() > 1 && util::is_digit(code[1])) {
      std::size_t i = 1;
      while (i < code.size() && util::is_digit(code[i])) ++i;
      code = trim(code.substr(i));
      if (const auto star = code.find('*'); star != std::string_view::npos) code = trim(code.substr(0, star));
      if (code.empty()) return;
    }

    const char c0 = util::to_upper(code.front());
    if (c0 != 'G' && c0 != 'M') return;  // T-команды, макросы Klipper и т. п.
    std::size_t sp = 1;
    while (sp < code.size() && (util::is_digit(code[sp]) || code[sp] == '.')) ++sp;
    std::string cmd(1, c0);
    cmd.append(code.substr(1, sp - 1));

    if (c0 == 'M') {
      if (cmd == "M82") {
        abs_e = true;
        e_mode_explicit = true;
      } else if (cmd == "M83") {
        abs_e = false;
        e_mode_explicit = true;
      }
      return;
    }

    const bool is_move = cmd == "G0" || cmd == "G1" || cmd == "G00" || cmd == "G01";
    const bool is_arc = cmd == "G2" || cmd == "G3" || cmd == "G02" || cmd == "G03";
    if (is_move || is_arc) {
      const Words p = scan_words(code, sp);
      double nx = x, ny = y, nz = z;
      if (p.has('X')) nx = p.get('X') * scale + (abs_xyz ? 0.0 : x);
      if (p.has('Y')) ny = p.get('Y') * scale + (abs_xyz ? 0.0 : y);
      if (p.has('Z')) nz = p.get('Z') * scale + (abs_xyz ? 0.0 : z);
      double de = 0.0;
      if (p.has('E')) {
        const double ev = p.get('E') * scale;
        if (abs_e) {
          de = ev - e;
          e = ev;
        } else {
          de = ev;
        }
      }
      if (de > 1e-7) used += de;
      const bool has_center = p.has('I') || p.has('J');
      const bool full_circle = is_arc && has_center && nx == x && ny == y;
      if (de > 1e-7 && (nx != x || ny != y || full_circle)) {
        const double dvol_total = de * fil_area;
        if (is_arc && (has_center || p.has('R'))) {
          const bool cw = cmd == "G2" || cmd == "G02";
          const auto pts = arc_points(x, y, nx, ny, p, scale, cw, options.arc_segment_length);
          std::vector<double> ls(pts.size() - 1);
          double tot = 0.0;
          for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
            ls[k] = std::hypot(pts[k + 1].x - pts[k].x, pts[k + 1].y - pts[k].y);
            tot += ls[k];
          }
          if (tot > 1e-9) {
            // объём распределяется по длинам хорд, Z — линейно вдоль дуги
            for (std::size_t k = 0; k + 1 < pts.size(); ++k) {
              const double zk = linspace_at(z, nz, pts.size(), k + 1);
              emit(pts[k].x, pts[k].y, pts[k + 1].x, pts[k + 1].y, zk, dvol_total * ls[k] / tot);
            }
          }
        } else {
          emit(x, y, nx, ny, nz, dvol_total);
        }
      }
      x = nx;
      y = ny;
      z = nz;
    } else if (cmd == "G92") {
      const Words p = scan_words(code, sp);
      if (p.has('E')) e = p.get('E') * scale;
      if (p.has('X')) x = p.get('X') * scale;
      if (p.has('Y')) y = p.get('Y') * scale;
      if (p.has('Z')) z = p.get('Z') * scale;
    } else if (cmd == "G90") {
      abs_xyz = true;
      if (!e_mode_explicit) abs_e = true;
    } else if (cmd == "G91") {
      abs_xyz = false;
      if (!e_mode_explicit) abs_e = false;
    } else if (cmd == "G20") {
      scale = 25.4;
    } else if (cmd == "G21") {
      scale = 1.0;
    }
  });

  info.filament_used_mm = used;
  info.excluded_volume = excluded_vol;
  if (tp.segments.empty())
    throw ParseError(
        "В G-code не найдено ни одного отрезка экструзии детали. "
        "Проверьте, что файл получен из слайсера и содержит печать модели.");

  fix_layers(tp);
  if (info.filament_type.empty())
    info.warnings.emplace_back("Тип пластика в G-code не указан — выберите материал вручную.");
  if (!has_layer_markers)
    info.warnings.emplace_back("В G-code нет маркеров слоёв: стартовый код мог попасть в модель.");
  return tp;
}

Toolpaths load(const std::filesystem::path& path, const ParseOptions& options) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    const std::u8string name = path.u8string();
    throw ParseError("Не удалось открыть файл G-code: " + std::string(name.begin(), name.end()));
  }
  std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  return parse(text, options);
}

}  // namespace kika::gcode
