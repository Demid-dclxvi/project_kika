// Предпросмотр нарезки: разбор G-code в движения, сводка по типам линий, сетка нитей.

#include "kika/app/preview.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <numbers>

namespace kika::app {

namespace {

struct TypeInfo {
  LineType type;
  const char* key;
  const char* name;
  std::uint32_t color;
};

// Цвета — привычные по предпросмотру Orca/Prusa оттенки, подобраны различимыми между собой.
constexpr TypeInfo kTypes[] = {
    {LineType::OuterWall, "outer_wall", "Наружная стенка", 0xF29A38},
    {LineType::InnerWall, "inner_wall", "Внутренние стенки", 0xF5D547},
    {LineType::OverhangWall, "overhang_wall", "Нависающие стенки", 0x4361C8},
    {LineType::Sparse, "sparse", "Разреженное заполнение", 0xB13B32},
    {LineType::Solid, "solid", "Сплошное заполнение", 0x9459C8},
    {LineType::Top, "top", "Верх", 0xE8546E},
    {LineType::Bottom, "bottom", "Низ", 0x6B84D9},
    {LineType::Bridge, "bridge", "Мосты", 0x3F98CF},
    {LineType::GapFill, "gap_fill", "Заполнение щелей", 0xFFFFFF},
    {LineType::Ironing, "ironing", "Глажка", 0xF4B3A6},
    {LineType::Skirt, "skirt", "Юбка", 0x19A88A},
    {LineType::Brim, "brim", "Кайма", 0x1C7F6B},
    {LineType::Support, "support", "Поддержки", 0x49B04D},
    {LineType::SupportInterface, "support_interface", "Плотные слои поддержек", 0x2B7A30},
    {LineType::PrimeTower, "prime_tower", "Черновая башня", 0xB3A47A},
    {LineType::Custom, "custom", "Прочее (старт, прочистка)", 0x8E8E8E},
    {LineType::Travel, "travel", "Перемещения", 0x4C6FA8},
};

const TypeInfo& info(LineType t) { return kTypes[static_cast<std::size_t>(t)]; }

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

bool has(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }

std::string_view trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
  return s;
}

std::optional<double> num(std::string_view s) {
  s = trim(s);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  double v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc() || r.ptr == s.data()) return std::nullopt;
  return v;
}

// Значение «ключ = значение» из настроек в конце G-code.
std::optional<std::string_view> config_value(std::string_view comment, std::string_view key) {
  // comment — текст после ';' без пробелов в начале
  if (!comment.starts_with(key)) return std::nullopt;
  std::string_view rest = trim(comment.substr(key.size()));
  if (rest.empty() || (rest.front() != '=' && rest.front() != ':')) return std::nullopt;
  return trim(rest.substr(1));
}

// Ramp: от тёмно-синего через бирюзовый и зелёный к жёлтому (как viridis — различим и при
// нарушениях цветового зрения).
constexpr std::array<std::uint32_t, 6> kRamp = {0x3B2C85, 0x2F5F9E, 0x22918C, 0x35B779, 0x8FD744, 0xF2E31E};

Rgb ramp_color(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double k = t * static_cast<double>(kRamp.size() - 1);
  const std::size_t i = std::min(kRamp.size() - 2, static_cast<std::size_t>(std::floor(k)));
  const Rgb a = rgb(kRamp[i]), b = rgb(kRamp[i + 1]);
  const float f = static_cast<float>(k - static_cast<double>(i));
  return {a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2] + (b[2] - a[2]) * f};
}

std::uint8_t byte_of(float c) { return static_cast<std::uint8_t>(std::lround(std::clamp(c, 0.0f, 1.0f) * 255.0f)); }

// Значение для раскраски шкалой (у перемещений не считается).
double value_of(const Preview& p, const PreviewMove& m, ColorBy c) {
  switch (c) {
    case ColorBy::Speed: return m.speed;
    case ColorBy::Flow: {
      const float l = m.length();
      return l > 0 ? m.volume / l * m.speed : 0.0;
    }
    case ColorBy::Width: return m.width;
    case ColorBy::Height: return m.height;
    case ColorBy::Fan: return m.fan * 100.0;
    case ColorBy::LayerTime: return m.layer < p.layers.size() ? p.layers[m.layer].time : 0.0;
    case ColorBy::Type: return 0;
  }
  return 0;
}

// Пределы шкалы по видимым линиям.
std::pair<double, double> value_range(const Preview& p, const PreviewView& v) {
  double lo = 1e300, hi = -1e300;
  for (const auto& m : p.moves) {
    if (!m.extrusion() || !v.visible[static_cast<std::size_t>(m.type)]) continue;
    const double x = value_of(p, m, v.color);
    lo = std::min(lo, x);
    hi = std::max(hi, x);
  }
  if (lo > hi) lo = hi = 0;
  return {lo, hi};
}

Rgb move_color(const Preview& p, const PreviewMove& m, const PreviewView& v, double lo, double hi) {
  if (v.color == ColorBy::Type) return line_type_color(m.type);
  const double x = value_of(p, m, v.color);
  return ramp_color(hi > lo ? (x - lo) / (hi - lo) : 0.5);
}

std::string num_label(double v, int digits) {
  std::string s = std::format("{:.{}f}", v, digits);
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

}  // namespace

float PreviewMove::length() const { return std::hypot(x1 - x0, y1 - y0); }

std::string_view line_type_name(LineType t) { return info(t).name; }
std::string_view line_type_key(LineType t) { return info(t).key; }
Rgb line_type_color(LineType t) { return rgb(info(t).color); }

std::optional<LineType> line_type_from_key(std::string_view key) {
  for (const auto& x : kTypes)
    if (key == x.key) return x.type;
  return std::nullopt;
}

LineType line_type_of(std::string_view label) {
  const std::string s = lower(trim(label));
  if (s.empty()) return LineType::Custom;
  // Cura
  if (s == "wall-outer") return LineType::OuterWall;
  if (s == "wall-inner") return LineType::InnerWall;
  if (s == "skin") return LineType::Solid;
  if (s == "fill") return LineType::Sparse;
  if (s == "skirt") return LineType::Skirt;
  if (s == "support-interface" || s == "support-roof" || s == "support-floor") return LineType::SupportInterface;
  if (s == "support" || s == "support-infill") return LineType::Support;
  if (s == "prime-tower") return LineType::PrimeTower;
  // OrcaSlicer, Bambu Studio, PrusaSlicer, свой
  if (has(s, "ironing")) return LineType::Ironing;
  if (has(s, "tower") || has(s, "wipe") || has(s, "prime")) return LineType::PrimeTower;
  if (has(s, "support")) return has(s, "interface") || has(s, "roof") ? LineType::SupportInterface : LineType::Support;
  if (has(s, "skirt")) return has(s, "brim") ? LineType::Skirt : LineType::Skirt;
  if (has(s, "brim")) return LineType::Brim;
  if (has(s, "overhang")) return LineType::OverhangWall;
  if (has(s, "outer") || has(s, "external")) return LineType::OuterWall;
  if (has(s, "wall") || has(s, "perimeter")) return LineType::InnerWall;
  if (has(s, "gap")) return LineType::GapFill;
  if (has(s, "bridge")) return LineType::Bridge;
  if (has(s, "top")) return LineType::Top;
  if (has(s, "bottom")) return LineType::Bottom;
  if (has(s, "solid")) return LineType::Solid;
  if (has(s, "infill") || has(s, "fill")) return LineType::Sparse;
  return LineType::Custom;
}

const TypeStat* Preview::stat(LineType t) const {
  for (const auto& s : stats)
    if (s.type == t) return &s;
  return nullptr;
}

std::optional<double> parse_duration(std::string_view s) {
  s = trim(s);
  double total = 0;
  bool any = false;
  while (!s.empty()) {
    std::size_t i = 0;
    while (i < s.size() && (std::isdigit(static_cast<unsigned char>(s[i])) || s[i] == '.')) ++i;
    if (i == 0) {
      s.remove_prefix(1);
      continue;
    }
    const auto v = num(s.substr(0, i));
    s.remove_prefix(i);
    if (!v || s.empty()) break;
    const char u = static_cast<char>(std::tolower(static_cast<unsigned char>(s.front())));
    const double k = u == 'd' ? 86400 : u == 'h' ? 3600 : u == 'm' ? 60 : u == 's' ? 1 : 0;
    if (k == 0) break;
    total += *v * k;
    any = true;
    s.remove_prefix(1);
  }
  return any ? std::optional<double>(total) : std::nullopt;
}

Preview parse_preview(std::string_view g) {
  Preview p;
  // состояние станка
  double x = 0, y = 0, z = 0, e = 0, f = 3000.0 / 60.0;
  bool rel_xyz = false, rel_e = false, inches = false;
  double accel = 1500, fan = 0;
  LineType type = LineType::Custom;
  // слои: по отметкам слайсера (;LAYER_CHANGE, ;LAYER:) или по высоте выдавливания
  bool markers = false, seen_marker = false;
  bool pending_z = false;  // высота слоя — по следующей отметке ;Z: или выдавливанию (прочистка до первой отметки)
  bool have_box = false;
  std::optional<double> marked_z, marked_h;
  std::uint32_t layer = 0;
  bool layer_open = false;
  double layer_z = 0;
  std::optional<double> footer_time;
  std::optional<double> footer_cost;
  if (g.find(";LAYER_CHANGE") != std::string_view::npos || g.find(";LAYER:") != std::string_view::npos) markers = true;

  auto open_layer = [&](double at_z) {
    if (!layer_open) {
      layer_open = true;
      p.layers.push_back({});
      p.layers.back().first = static_cast<std::uint32_t>(p.moves.size());
      p.layers.back().z = at_z;
      layer = static_cast<std::uint32_t>(p.layers.size() - 1);
      layer_z = at_z;
    }
  };
  // время движения: разгон и торможение до скорости в углу 10 мм/с
  auto move_time = [&](double len, double v) {
    v = std::max(v, 0.1);
    const double a = std::max(accel, 10.0), vc = std::min(v, 10.0);
    const double d_acc = (v * v - vc * vc) / a;  // разгон + торможение
    if (len >= d_acc) return len / v + (v - vc) * (v - vc) / (a * v);
    const double vp = std::sqrt(vc * vc + a * len);  // пик скорости на коротком отрезке
    return 2 * (vp - vc) / a;
  };
  const double fil_area0 = std::numbers::pi * 0.25 * 1.75 * 1.75;
  double fil_area = fil_area0;
  auto add_move = [&](double nx, double ny, double nz, double de, double feed) {
    const double dx = nx - x, dy = ny - y;
    const double len = std::hypot(dx, dy);
    if (len < 1e-6) return;
    PreviewMove m;
    m.x0 = static_cast<float>(x);
    m.y0 = static_cast<float>(y);
    m.x1 = static_cast<float>(nx);
    m.y1 = static_cast<float>(ny);
    m.z = static_cast<float>(nz);
    m.speed = static_cast<float>(feed);
    m.fan = static_cast<float>(fan);
    m.time = static_cast<float>(move_time(len, feed));
    if (de > 1e-6) {
      if (!markers && (!layer_open || std::abs(nz - layer_z) > 1e-4)) {
        // без отметок слоя: новый слой — выдавливание на другой высоте
        if (layer_open && nz < layer_z - 1e-4) {
          // ниже текущего (прочистка в начале, рисунок с подъёмом) — в текущий слой
        } else {
          layer_open = false;
          open_layer(nz);
        }
      }
      open_layer(marked_z.value_or(nz));
      if (pending_z) {
        p.layers.back().z = marked_z.value_or(nz);
        layer_z = p.layers.back().z;
        pending_z = false;
      }
      m.type = type;
      m.volume = static_cast<float>(de * fil_area);
      PreviewLayer& L = p.layers[layer];
      double h = marked_h.value_or(0);
      if (h <= 0) h = layer == 0 ? L.z : L.z - p.layers[layer - 1].z;
      if (h <= 0 || h > 5) h = 0.2;
      L.height = h;
      m.height = static_cast<float>(h);
      // ширина по объёму: сечение — прямоугольник со скруглёнными боками (как у Slic3r/Orca)
      const double w = de * fil_area / (len * h) + h * (1 - std::numbers::pi / 4);
      m.width = static_cast<float>(std::clamp(w, 0.05, 10.0));
      const std::array<double, 3> lo{std::min(x, nx), std::min(y, ny), nz - h}, hi{std::max(x, nx), std::max(y, ny), nz};
      for (std::size_t a = 0; a < 3; ++a) {
        p.min[a] = have_box ? std::min(p.min[a], lo[a]) : lo[a];
        p.max[a] = have_box ? std::max(p.max[a], hi[a]) : hi[a];
      }
      have_box = true;
      p.filament_mm += de;
    } else {
      m.type = LineType::Travel;
      if (p.layers.empty()) return;  // перемещения в стартовом коде до первого слоя не показываем
    }
    m.layer = layer;
    p.moves.push_back(m);
  };

  std::size_t pos = 0;
  int head_lines = 0;  // слайсер — в одной из первых строк
  while (pos < g.size()) {
    std::size_t end = g.find('\n', pos);
    if (end == std::string_view::npos) end = g.size();
    std::string_view line = trim(g.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty()) continue;
    if (line.front() == ';') {
      const std::string_view c = trim(line.substr(1));
      if (p.slicer.empty() && head_lines < 20) {
        // «; generated by OrcaSlicer 2.1.0 on …», «; generated by PrusaSlicer 2.7.1 on …», «;Generated with Cura_SteamEngine …»
        const std::string lc = lower(c);
        for (const char* tag : {"generated by ", "generated with "})
          if (const auto k = lc.find(tag); k != std::string::npos) {
            std::string_view rest = c.substr(k + std::string_view(tag).size());
            const auto sp = rest.find(' ');
            p.slicer = std::string(rest.substr(0, sp));
            if (p.slicer.starts_with("Cura")) p.slicer = "Cura";
          }
      }
      ++head_lines;
      if (c.starts_with("TYPE:")) {
        type = line_type_of(c.substr(5));
      } else if (c.starts_with("FEATURE:")) {
        type = line_type_of(c.substr(8));
      } else if (c == "LAYER_CHANGE" || c.starts_with("LAYER:")) {
        // выдавливание до первой отметки (прочистка в стартовом коде) — в первый слой
        if (seen_marker)
          layer_open = false;
        else
          pending_z = layer_open;
        seen_marker = true;
        marked_z.reset();
        marked_h.reset();
      } else if (c.starts_with("Z:")) {
        marked_z = num(c.substr(2));
        if (layer_open && marked_z && pending_z) {
          p.layers.back().z = *marked_z;
          layer_z = *marked_z;
          pending_z = false;
        }
      } else if (c.starts_with("HEIGHT:")) {
        marked_h = num(c.substr(7));
      } else if (c.starts_with("TIME:")) {
        if (const auto t = num(c.substr(5))) footer_time = *t;  // Cura: секунды
      } else if (auto v1 = config_value(c, "estimated printing time (normal mode)")) {
        footer_time = parse_duration(*v1);
      } else if (const auto k = c.find("total estimated time:"); k != std::string_view::npos) {
        if (auto t = parse_duration(c.substr(k + 21))) footer_time = *t;
      } else if (auto v2 = config_value(c, "filament_density")) {
        if (const auto d = num(v2->substr(0, v2->find(',')))) p.density = *d;
      } else if (auto v3 = config_value(c, "filament_diameter")) {
        if (const auto d = num(v3->substr(0, v3->find(',')))) {
          p.filament_diameter = *d;
          fil_area = std::numbers::pi * 0.25 * *d * *d;
        }
      } else if (auto v4 = config_value(c, "filament cost")) {
        footer_cost = num(*v4);
      } else if (auto v5 = config_value(c, "total filament cost")) {
        footer_cost = num(*v5);
      }
      continue;
    }
    ++head_lines;
    // команда; комментарий в конце строки отбрасываем
    std::string_view code = line.substr(0, line.find(';'));
    code = trim(code);
    if (code.empty()) continue;
    // номер строки N… и контрольная сумма *…
    if (code.front() == 'N' || code.front() == 'n') {
      const auto sp = code.find(' ');
      code = sp == std::string_view::npos ? std::string_view() : trim(code.substr(sp));
    }
    if (const auto star = code.find('*'); star != std::string_view::npos) code = trim(code.substr(0, star));
    if (code.empty()) continue;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(code.front())));
    std::size_t k = 1;
    while (k < code.size() && (std::isdigit(static_cast<unsigned char>(code[k])) || code[k] == '.')) ++k;
    const std::string_view number = code.substr(1, k - 1);
    // параметры: буква и число, пробелы между ними необязательны (G1X10Y5)
    std::array<std::optional<double>, 26> prm{};
    std::string_view rest = code.substr(k);
    if (letter == 'G' || letter == 'M') {
      std::size_t i = 0;
      while (i < rest.size()) {
        const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(rest[i])));
        if (c >= 'A' && c <= 'Z') {
          std::size_t j = i + 1;
          while (j < rest.size() && rest[j] != ' ' && !(std::isalpha(static_cast<unsigned char>(rest[j])))) ++j;
          if (const auto v = num(rest.substr(i + 1, j - i - 1))) prm[static_cast<std::size_t>(c - 'A')] = *v;
          i = j;
        } else {
          ++i;
        }
      }
    }
    auto P = [&](char c) { return prm[static_cast<std::size_t>(c - 'A')]; };
    const double unit = inches ? 25.4 : 1.0;
    if (letter == 'G') {
      if (number == "0" || number == "1" || number == "00" || number == "01") {
        if (P('F')) f = std::max(0.01, *P('F') * unit / 60.0);
        double nx = x, ny = y, nz = z, de = 0;
        if (P('X')) nx = rel_xyz ? x + *P('X') * unit : *P('X') * unit;
        if (P('Y')) ny = rel_xyz ? y + *P('Y') * unit : *P('Y') * unit;
        if (P('Z')) nz = rel_xyz ? z + *P('Z') * unit : *P('Z') * unit;
        if (P('E')) {
          de = rel_e ? *P('E') * unit : *P('E') * unit - e;
          e = rel_e ? e + de : *P('E') * unit;
        }
        if (!P('X') && !P('Y')) {
          if (de < -1e-6) ++p.retractions;
          z = nz;
          continue;
        }
        add_move(nx, ny, nz, de, f);
        x = nx;
        y = ny;
        z = nz;
      } else if (number == "2" || number == "3" || number == "02" || number == "03") {
        // дуга: разбиваем на хорды ~0,5 мм
        if (P('F')) f = std::max(0.01, *P('F') * unit / 60.0);
        const double ex = P('X') ? (rel_xyz ? x + *P('X') * unit : *P('X') * unit) : x;
        const double ey = P('Y') ? (rel_xyz ? y + *P('Y') * unit : *P('Y') * unit) : y;
        double de = 0;
        if (P('E')) {
          de = rel_e ? *P('E') * unit : *P('E') * unit - e;
          e = rel_e ? e + de : *P('E') * unit;
        }
        double cx, cy;
        if (P('I') || P('J')) {
          cx = x + P('I').value_or(0) * unit;
          cy = y + P('J').value_or(0) * unit;
        } else if (P('R')) {
          // центр по радиусу (R < 0 — большая дуга)
          const double r = *P('R') * unit, mx = (x + ex) / 2, my = (y + ey) / 2;
          const double dx = ex - x, dy = ey - y, d = std::hypot(dx, dy);
          if (d < 1e-9) continue;
          const double hh = std::sqrt(std::max(0.0, r * r - d * d / 4));
          const double s = ((number == "2" || number == "02") ? -1.0 : 1.0) * (r < 0 ? -1.0 : 1.0);
          cx = mx - s * hh * dy / d;
          cy = my + s * hh * dx / d;
        } else {
          add_move(ex, ey, z, de, f);
          x = ex;
          y = ey;
          continue;
        }
        const bool ccw = number == "3" || number == "03";
        const double r = std::hypot(x - cx, y - cy);
        double a0 = std::atan2(y - cy, x - cx), a1 = std::atan2(ey - cy, ex - cx);
        double sweep = a1 - a0;
        if (ccw && sweep <= 1e-9) sweep += 2 * std::numbers::pi;
        if (!ccw && sweep >= -1e-9) sweep -= 2 * std::numbers::pi;
        const int n = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) * r / 0.5)), 1, 720);
        for (int i = 1; i <= n; ++i) {
          const double a = a0 + sweep * i / n;
          const double px = i == n ? ex : cx + r * std::cos(a), py = i == n ? ey : cy + r * std::sin(a);
          add_move(px, py, z, de / n, f);
          x = px;
          y = py;
        }
      } else if (number == "90") {
        rel_xyz = false;
        rel_e = false;  // как в Marlin: G90 — и экструдер абсолютный, пока нет M83
      } else if (number == "91") {
        rel_xyz = true;
      } else if (number == "92") {
        if (P('E')) e = *P('E') * unit;
        if (P('X')) x = *P('X') * unit;
        if (P('Y')) y = *P('Y') * unit;
        if (P('Z')) z = *P('Z') * unit;
        if (!P('E') && !P('X') && !P('Y') && !P('Z')) e = 0;
      } else if (number == "20") {
        inches = true;
      } else if (number == "21") {
        inches = false;
      } else if (number == "28") {
        if (!P('X') && !P('Y') && !P('Z')) x = y = z = 0;
        if (P('X')) x = 0;
        if (P('Y')) y = 0;
        if (P('Z')) z = 0;
      }
    } else if (letter == 'M') {
      if (number == "82") {
        rel_e = false;
      } else if (number == "83") {
        rel_e = true;
      } else if (number == "106") {
        const double fan_p = P('P').value_or(0);
        if (fan_p == 0 || fan_p == 1) fan = std::clamp(P('S').value_or(255) / 255.0, 0.0, 1.0);
      } else if (number == "107") {
        if (P('P').value_or(0) <= 1) fan = 0;
      } else if (number == "204") {
        if (P('S')) accel = *P('S');
        else if (P('P')) accel = *P('P');
      }
    } else if (code.starts_with("SET_VELOCITY_LIMIT")) {
      const auto a = code.find("ACCEL=");
      if (a != std::string_view::npos) {
        std::string_view v = code.substr(a + 6);
        if (const auto d = num(v.substr(0, v.find(' ')))) accel = *d;
      }
    }
  }
  if (layer_open && !p.layers.empty()) p.layers.back().last = static_cast<std::uint32_t>(p.moves.size());
  // границы слоёв: до начала следующего; пустые слои (только перемещения) убираем
  for (std::size_t i = 0; i < p.layers.size(); ++i)
    p.layers[i].last = i + 1 < p.layers.size() ? p.layers[i + 1].first : static_cast<std::uint32_t>(p.moves.size());
  // хвостовые перемещения после последнего выдавливания слоя — в нём же; слои без выдавливания
  // (стоят только перемещения) склеиваем с предыдущим
  {
    std::vector<PreviewLayer> keep;
    std::vector<std::uint32_t> remap(p.layers.size(), 0);
    for (std::size_t i = 0; i < p.layers.size(); ++i) {
      bool any = false;
      for (std::uint32_t m = p.layers[i].first; m < p.layers[i].last && !any; ++m) any = p.moves[m].extrusion();
      if (!any && !keep.empty()) {
        keep.back().last = p.layers[i].last;
        remap[i] = static_cast<std::uint32_t>(keep.size() - 1);
        continue;
      }
      remap[i] = static_cast<std::uint32_t>(keep.size());
      keep.push_back(p.layers[i]);
    }
    for (auto& m : p.moves) m.layer = remap[m.layer];
    p.layers = std::move(keep);
  }
  // время: своя оценка, а если слайсер написал итог — по нему (доли типов линий — по своей)
  double est = 0;
  for (const auto& m : p.moves) est += m.time;
  p.time_s = est;
  if (footer_time && *footer_time > 0 && est > 0) {
    const double k = *footer_time / est;
    for (auto& m : p.moves) m.time = static_cast<float>(m.time * k);
    p.time_s = *footer_time;
    p.time_from_file = true;
  }
  for (auto& L : p.layers) {
    L.time = 0;
    for (std::uint32_t m = L.first; m < L.last; ++m) L.time += p.moves[m].time;
  }
  // сводка по типам
  std::array<TypeStat, kLineTypeCount> st{};
  for (std::size_t t = 0; t < kLineTypeCount; ++t) st[t].type = static_cast<LineType>(t);
  for (const auto& m : p.moves) {
    auto& s = st[static_cast<std::size_t>(m.type)];
    ++s.moves;
    s.length += m.length();
    s.time += m.time;
    s.volume += m.volume;
  }
  for (auto& s : st) {
    s.grams = s.volume * 1e-3 * p.density;
    if (s.moves > 0) p.stats.push_back(s);
  }
  p.filament_g = p.filament_mm * fil_area * 1e-3 * p.density;
  p.cost = footer_cost;
  return p;
}

// ---------------------------------------------------------------------------- вид

PreviewView::PreviewView() {
  visible.fill(true);
  visible[static_cast<std::size_t>(LineType::Travel)] = false;
}

std::string_view color_by_name(ColorBy c) {
  switch (c) {
    case ColorBy::Type: return "Тип линии";
    case ColorBy::Speed: return "Скорость";
    case ColorBy::Flow: return "Расход";
    case ColorBy::Width: return "Ширина линии";
    case ColorBy::Height: return "Высота слоя";
    case ColorBy::Fan: return "Обдув";
    case ColorBy::LayerTime: return "Время слоя";
  }
  return "Тип линии";
}

std::string_view color_by_key(ColorBy c) {
  switch (c) {
    case ColorBy::Type: return "type";
    case ColorBy::Speed: return "speed";
    case ColorBy::Flow: return "flow";
    case ColorBy::Width: return "width";
    case ColorBy::Height: return "height";
    case ColorBy::Fan: return "fan";
    case ColorBy::LayerTime: return "layer_time";
  }
  return "type";
}

std::optional<ColorBy> color_by_from_key(std::string_view key) {
  for (ColorBy c : {ColorBy::Type, ColorBy::Speed, ColorBy::Flow, ColorBy::Width, ColorBy::Height, ColorBy::Fan,
                    ColorBy::LayerTime})
    if (key == color_by_key(c)) return c;
  return std::nullopt;
}

std::string fmt_duration(double s) {
  const long long t = std::max(0LL, std::llround(s));
  const long long h = t / 3600, m = t / 60 % 60, sec = t % 60;
  if (h > 0) return std::format("{} ч {:02} мин", h, m);
  if (m > 0) return std::format("{} мин {:02} с", m, sec);
  return std::format("{} с", sec);
}

PreviewLegend preview_legend(const Preview& p, const PreviewView& v) {
  PreviewLegend lg;
  lg.title = std::string(color_by_name(v.color));
  if (v.color == ColorBy::Type) {
    double total = 0;
    for (const auto& s : p.stats) total += s.time;
    for (const auto& s : p.stats) {
      PreviewLegend::Entry e;
      e.color = line_type_color(s.type);
      e.label = std::string(line_type_name(s.type));
      e.type = s.type;
      e.visible = v.visible[static_cast<std::size_t>(s.type)];
      e.time = fmt_duration(s.time);
      e.share = total > 0 ? std::format("{} %", std::lround(100 * s.time / total)) : std::string();
      e.grams = s.type == LineType::Travel ? std::string() : num_label(s.grams, s.grams < 10 ? 2 : 1) + " г";
      lg.entries.push_back(std::move(e));
    }
    return lg;
  }
  const auto [lo, hi] = value_range(p, v);
  lg.lo = lo;
  lg.hi = hi;
  int digits = 0;
  switch (v.color) {
    case ColorBy::Speed: lg.unit = "мм/с"; break;
    case ColorBy::Flow: lg.unit = "мм³/с", digits = 1; break;
    case ColorBy::Width: lg.unit = "мм", digits = 2; break;
    case ColorBy::Height: lg.unit = "мм", digits = 2; break;
    case ColorBy::Fan: lg.unit = "%"; break;
    case ColorBy::LayerTime: lg.unit = "с", digits = 1; break;
    case ColorBy::Type: break;
  }
  const int steps = hi - lo < 1e-9 ? 1 : 6;
  for (int i = steps - 1; i >= 0; --i) {
    const double t = steps == 1 ? 0.5 : static_cast<double>(i) / (steps - 1);
    PreviewLegend::Entry e;
    e.color = ramp_color(t);
    e.label = num_label(lo + (hi - lo) * (steps == 1 ? 0.0 : t), digits) + " " + lg.unit;
    lg.entries.push_back(std::move(e));
  }
  return lg;
}

// ---------------------------------------------------------------------------- сетка

void build_preview_mesh(const Preview& p, const PreviewView& v, PreviewMesh& out) {
  auto& mesh = out.mesh;
  mesh.pos.clear();
  mesh.nrm.clear();
  mesh.col.clear();
  mesh.idx.clear();
  out.layer_end.assign(p.layers.size(), 0);
  out.move_end.assign(p.moves.size(), 0);
  out.travel.clear();
  out.travel_layer_end.assign(p.layers.size(), 0);
  out.travel_move_end.assign(p.moves.size(), 0);
  const auto [lo, hi] = v.color == ColorBy::Type ? std::pair<double, double>{0, 0} : value_range(p, v);
  const bool travels = v.visible[static_cast<std::size_t>(LineType::Travel)];
  std::size_t n_ext = 0;
  for (const auto& m : p.moves) n_ext += m.extrusion() ? 1 : 0;
  mesh.pos.reserve(n_ext * 4 * 3 + 64);
  mesh.idx.reserve(n_ext * 24 + 64);

  // кольцо из 4 вершин: верх, лево, низ, право (ромб — выглядит скруглённым валиком)
  auto add_ring = [&](double px, double py, double pz, double ux, double uy, double hw, double hh, const Rgb& c) {
    const auto base = static_cast<std::uint32_t>(mesh.pos.size() / 3);
    const double off[4][3] = {{0, 0, hh}, {-uy * hw, ux * hw, 0}, {0, 0, -hh}, {uy * hw, -ux * hw, 0}};
    const double nl = std::hypot(ux, uy) > 0 ? 1.0 / std::hypot(ux, uy) : 1.0;
    const double nrm[4][3] = {{0, 0, 1}, {-uy * nl, ux * nl, 0}, {0, 0, -1}, {uy * nl, -ux * nl, 0}};
    const std::uint8_t rgbv[3] = {byte_of(c[0]), byte_of(c[1]), byte_of(c[2])};
    for (std::size_t q = 0; q < 4; ++q) {
      mesh.pos.push_back(static_cast<float>(px + off[q][0]));
      mesh.pos.push_back(static_cast<float>(py + off[q][1]));
      mesh.pos.push_back(static_cast<float>(pz + off[q][2]));
      for (std::size_t i = 0; i < 3; ++i) {
        mesh.nrm.push_back(static_cast<std::int8_t>(std::lround(std::clamp(nrm[q][i], -1.0, 1.0) * 127.0)));
        mesh.col.push_back(rgbv[i]);
      }
      mesh.nrm.push_back(0);
      mesh.col.push_back(255);
    }
    return base;
  };
  auto add_quads = [&](std::uint32_t ra, std::uint32_t rb) {
    for (std::uint32_t q = 0; q < 4; ++q) {
      const std::uint32_t q1 = (q + 1) % 4;
      mesh.idx.insert(mesh.idx.end(), {ra + q, ra + q1, rb + q1, ra + q, rb + q1, rb + q});
    }
  };

  // предыдущая нить: её конечное кольцо можно продолжить, если следующая нить начинается там же,
  // того же цвета и поворачивает не круче 60°
  struct Tail {
    bool open = false;
    float x = 0, y = 0, z = 0;
    double ux = 0, uy = 0, hw = 0, hh = 0;
    Rgb c{};
    std::uint32_t ring = 0;  // кольцо в конце
    std::size_t idx_at = 0;  // где в idx лежат четыре квада последней нити (для правки стыка)
  } tail;
  std::size_t li = 0;
  for (std::size_t k = 0; k < p.moves.size(); ++k) {
    const auto& m = p.moves[k];
    while (li < p.layers.size() && k >= p.layers[li].last) {
      out.layer_end[li] = static_cast<std::uint32_t>(mesh.idx.size());
      out.travel_layer_end[li] = static_cast<std::uint32_t>(out.travel.size() / 3);
      ++li;
      tail.open = false;
    }
    if (!m.extrusion()) {
      tail.open = false;
      if (travels) {
        for (float c : {m.x0, m.y0, m.z, m.x1, m.y1, m.z}) out.travel.push_back(c);
      }
    } else if (v.visible[static_cast<std::size_t>(m.type)]) {
      const double dx = m.x1 - m.x0, dy = m.y1 - m.y0, len = std::hypot(dx, dy);
      const double ux = dx / len, uy = dy / len;
      const double hw = 0.5 * m.width, hh = 0.5 * m.height;
      const double zc = m.z - hh;
      const Rgb c = move_color(p, m, v, lo, hi);
      const bool joins = tail.open && tail.x == m.x0 && tail.y == m.y0 && tail.z == m.z && tail.c == c &&
                         std::abs(tail.hw - hw) < 0.05 && std::abs(tail.hh - hh) < 1e-4 && (tail.ux * ux + tail.uy * uy) > 0.5;
      std::uint32_t r0;
      if (joins) {
        // стык: кольцо по биссектрисе, ширина — по косому срезу
        const double bx = tail.ux + ux, by = tail.uy + uy, bl = std::hypot(bx, by);
        const double cosh = std::max(0.5, (bx / bl) * ux + (by / bl) * uy);
        const auto shared = tail.ring;
        const double jx = bx / bl, jy = by / bl, jw = hw / cosh;
        // переписать вершины общего кольца
        const double off[4][3] = {{0, 0, hh}, {-jy * jw, jx * jw, 0}, {0, 0, -hh}, {jy * jw, -jx * jw, 0}};
        const double nrm[4][3] = {{0, 0, 1}, {-jy, jx, 0}, {0, 0, -1}, {jy, -jx, 0}};
        for (std::size_t q = 0; q < 4; ++q) {
          const std::size_t vi = (shared + q) * 3;
          mesh.pos[vi] = static_cast<float>(m.x0 + off[q][0]);
          mesh.pos[vi + 1] = static_cast<float>(m.y0 + off[q][1]);
          mesh.pos[vi + 2] = static_cast<float>(zc + off[q][2]);
          const std::size_t ni = (shared + q) * 4;
          for (std::size_t i = 0; i < 3; ++i)
            mesh.nrm[ni + i] = static_cast<std::int8_t>(std::lround(std::clamp(nrm[q][i], -1.0, 1.0) * 127.0));
        }
        r0 = shared;
      } else {
        // начало нити выступает на полширины: стыки углов без щелей
        r0 = add_ring(m.x0 - ux * hw, m.y0 - uy * hw, zc, ux, uy, hw, hh, c);
      }
      const std::uint32_t r1 = add_ring(m.x1 + ux * hw, m.y1 + uy * hw, zc, ux, uy, hw, hh, c);
      add_quads(r0, r1);
      tail = {true, m.x1, m.y1, m.z, ux, uy, hw, hh, c, r1, mesh.idx.size()};
      // конец без выступа, если дальше продолжение: правится при стыке (кольцо переписывается)
    } else {
      tail.open = false;
    }
    out.move_end[k] = static_cast<std::uint32_t>(mesh.idx.size());
    out.travel_move_end[k] = static_cast<std::uint32_t>(out.travel.size() / 3);
  }
  for (; li < p.layers.size(); ++li) {
    out.layer_end[li] = static_cast<std::uint32_t>(mesh.idx.size());
    out.travel_layer_end[li] = static_cast<std::uint32_t>(out.travel.size() / 3);
  }
}

std::pair<std::uint32_t, std::uint32_t> preview_range(const Preview& p, const PreviewMesh& m, std::size_t lo, std::size_t hi,
                                                      std::optional<std::size_t> upto) {
  if (p.layers.empty() || m.layer_end.size() != p.layers.size()) return {0, 0};
  hi = std::min(hi, p.layers.size() - 1);
  lo = std::min(lo, hi);
  const std::uint32_t a = lo == 0 ? 0 : m.layer_end[lo - 1];
  std::uint32_t b = m.layer_end[hi];
  if (upto && *upto < m.move_end.size()) {
    const std::size_t first = p.layers[hi].first;
    const std::size_t k = std::max(first, *upto);
    if (k < p.layers[hi].last) b = m.move_end[k];
  }
  return {a, std::max(a, b)};
}

std::pair<std::uint32_t, std::uint32_t> travel_range(const Preview& p, const PreviewMesh& m, std::size_t lo, std::size_t hi,
                                                     std::optional<std::size_t> upto) {
  if (p.layers.empty() || m.travel_layer_end.size() != p.layers.size()) return {0, 0};
  hi = std::min(hi, p.layers.size() - 1);
  lo = std::min(lo, hi);
  const std::uint32_t a = lo == 0 ? 0 : m.travel_layer_end[lo - 1];
  std::uint32_t b = m.travel_layer_end[hi];
  if (upto && *upto < m.travel_move_end.size()) {
    const std::size_t k = std::max<std::size_t>(p.layers[hi].first, *upto);
    if (k < p.layers[hi].last) b = m.travel_move_end[k];
  }
  return {a, std::max(a, b)};
}

}  // namespace kika::app
