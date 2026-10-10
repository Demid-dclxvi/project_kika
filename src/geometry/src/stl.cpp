// Чтение STL: двоичный (80 байт заголовка, число треугольников, по 50 байт на треугольник)
// и текстовый (solid … facet … vertex x y z … endsolid).

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>

#include "kika/geometry/import.hpp"

namespace kika::geometry {

namespace {

float le_float(const char* p) {
  // файлы STL — с порядком байтов little-endian, как у x86 и ARM в обычном режиме
  std::uint32_t u = static_cast<std::uint32_t>(static_cast<unsigned char>(p[0])) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(p[1])) << 8) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(p[2])) << 16) |
                    (static_cast<std::uint32_t>(static_cast<unsigned char>(p[3])) << 24);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

ImportedModel parse_binary(std::string_view d, std::uint32_t n) {
  ImportedModel m;
  m.format = "STL";
  m.objects = 1;
  m.mesh.vertices.reserve(static_cast<std::size_t>(n) * 3);
  m.mesh.triangles.reserve(n);
  std::size_t bad = 0;
  for (std::uint32_t i = 0; i < n; ++i) {
    const char* p = d.data() + 84 + static_cast<std::size_t>(i) * 50 + 12;  // нормаль пропускаем
    const auto base = static_cast<std::uint32_t>(m.mesh.vertices.size());
    bool finite = true;
    for (int k = 0; k < 3; ++k) {
      Vec3 v{le_float(p), le_float(p + 4), le_float(p + 8)};
      for (double x : v) finite = finite && std::isfinite(x);
      m.mesh.vertices.push_back(v);
      p += 12;
    }
    if (!finite) {
      m.mesh.vertices.resize(base);
      ++bad;
      continue;
    }
    m.mesh.triangles.push_back({base, base + 1, base + 2});
  }
  if (bad) m.warnings.push_back(std::to_string(bad) + " треугольников с испорченными координатами пропущено.");
  return m;
}

ImportedModel parse_ascii(std::string_view d) {
  ImportedModel m;
  m.format = "STL";
  std::size_t p = 0;
  auto skip = [&] {
    while (p < d.size() && is_space(d[p])) ++p;
  };
  auto word = [&]() -> std::string_view {
    skip();
    const std::size_t b = p;
    while (p < d.size() && !is_space(d[p])) ++p;
    return d.substr(b, p - b);
  };
  std::size_t line_hint = 0;
  auto number = [&]() -> double {
    const std::string_view w = word();
    double v = 0;
    const auto r = std::from_chars(w.data(), w.data() + w.size(), v);
    if (r.ec != std::errc() || r.ptr != w.data() + w.size()) {
      line_hint = static_cast<std::size_t>(std::count(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(p), '\n')) + 1;
      throw ImportError("STL: ожидалось число, а в строке " + std::to_string(line_hint) + " — «" + std::string(w.substr(0, 40)) + "»");
    }
    return v;
  };
  int in_facet = 0;
  std::size_t solids = 0;
  for (;;) {
    const std::string_view w = word();
    if (w.empty()) break;
    if (w == "solid") {
      ++solids;
      // имя тела — до конца строки
      while (p < d.size() && d[p] != '\n' && (d[p] == ' ' || d[p] == '\t')) ++p;
      const std::size_t b = p;
      while (p < d.size() && d[p] != '\n' && d[p] != '\r') ++p;
      if (m.name.empty()) m.name = std::string(d.substr(b, p - b));
    } else if (w == "vertex") {
      const double x = number(), y = number(), z = number();
      m.mesh.vertices.push_back({x, y, z});
      ++in_facet;
    } else if (w == "endloop") {
      if (in_facet == 3) {
        const auto b = static_cast<std::uint32_t>(m.mesh.vertices.size() - 3);
        m.mesh.triangles.push_back({b, b + 1, b + 2});
      } else if (in_facet > 3) {
        // многоугольник — веером (бывает в старых программах)
        const auto b = static_cast<std::uint32_t>(m.mesh.vertices.size()) - static_cast<std::uint32_t>(in_facet);
        for (int k = 1; k + 1 < in_facet; ++k)
          m.mesh.triangles.push_back({b, b + static_cast<std::uint32_t>(k), b + static_cast<std::uint32_t>(k + 1)});
      } else {
        m.mesh.vertices.resize(m.mesh.vertices.size() - static_cast<std::size_t>(in_facet));
      }
      in_facet = 0;
    }
    // facet, normal, outer, loop, endfacet, endsolid и числа нормали пропускаются
  }
  m.objects = std::max<std::size_t>(solids, 1);
  return m;
}

}  // namespace

ImportedModel parse_stl(std::string_view d) {
  if (d.size() >= 84) {
    std::uint32_t n;
    n = static_cast<std::uint32_t>(static_cast<unsigned char>(d[80])) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d[81])) << 8) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d[82])) << 16) |
        (static_cast<std::uint32_t>(static_cast<unsigned char>(d[83])) << 24);
    // двоичный — если размер сходится с числом треугольников (заголовок может начинаться со «solid»)
    if (84 + static_cast<std::uint64_t>(n) * 50 == d.size()) {
      if (n == 0) throw ImportError("STL пустой: в нём нет треугольников.");
      return parse_binary(d, n);
    }
  }
  std::size_t b = 0;
  while (b < d.size() && is_space(d[b])) ++b;
  if (d.substr(b, 5) == "solid" && d.find("facet") != std::string_view::npos) {
    auto m = parse_ascii(d);
    if (m.mesh.empty()) throw ImportError("STL пустой: в нём нет треугольников.");
    return m;
  }
  if (d.size() >= 84) {
    // двоичный с неверным числом треугольников в заголовке (некоторые программы пишут 0)
    const std::size_t n = (d.size() - 84) / 50;
    if (n > 0) {
      auto m = parse_binary(d, static_cast<std::uint32_t>(n));
      m.warnings.push_back("В заголовке STL неверное число треугольников — прочитано по размеру файла.");
      return m;
    }
  }
  throw ImportError("Файл не похож на STL.");
}

}  // namespace kika::geometry
