// Миниатюра детали для G-code: своя растеризация треугольников, PNG через zlib, base64.
//
// Вид — спереди справа сверху, как в слайсерах; деталь окрашена ровным цветом с освещением,
// фон прозрачный. Сглаживание — рисуем вдвое крупнее и усредняем 2 × 2.

#include "thumbnail.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>

#include <zlib.h>

namespace kika::slicer::detail {

namespace {

using geometry::Vec3;

Vec3 sub(const Vec3& a, const Vec3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
Vec3 cross(const Vec3& a, const Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
double dot(const Vec3& a, const Vec3& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
Vec3 unit(const Vec3& a) {
  const double l = std::sqrt(dot(a, a));
  return l > 0 ? Vec3{a[0] / l, a[1] / l, a[2] / l} : Vec3{0, 0, 1};
}

void put_u32(std::string& out, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<char>((v >> s) & 0xFF));
}

void chunk(std::string& png, const char* type, const std::string& data) {
  put_u32(png, static_cast<std::uint32_t>(data.size()));
  std::string body(type, 4);
  body += data;
  png += body;
  put_u32(png, static_cast<std::uint32_t>(crc32(0L, reinterpret_cast<const Bytef*>(body.data()), static_cast<uInt>(body.size()))));
}

std::string base64(const std::string& in) {
  static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((in.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 2 < in.size(); i += 3) {
    const auto n = (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8) |
                   static_cast<unsigned char>(in[i + 2]);
    out += t[(n >> 18) & 63];
    out += t[(n >> 12) & 63];
    out += t[(n >> 6) & 63];
    out += t[n & 63];
  }
  if (i < in.size()) {
    std::uint32_t n = static_cast<std::uint32_t>(static_cast<unsigned char>(in[i])) << 16;
    if (i + 1 < in.size()) n |= static_cast<std::uint32_t>(static_cast<unsigned char>(in[i + 1])) << 8;
    out += t[(n >> 18) & 63];
    out += t[(n >> 12) & 63];
    out += i + 1 < in.size() ? t[(n >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

}  // namespace

std::vector<std::uint8_t> render_rgba(const geometry::TriangleMesh& mesh, int w, int h) {
  const int ss = 2;  // сглаживание
  const int W = w * ss, H = h * ss;
  std::vector<float> depth(static_cast<std::size_t>(W) * H, std::numeric_limits<float>::infinity());
  std::vector<std::array<float, 3>> color(static_cast<std::size_t>(W) * H, {0, 0, 0});
  std::vector<std::uint8_t> out(static_cast<std::size_t>(w) * h * 4, 0);
  if (mesh.empty() || w <= 0 || h <= 0) return out;
  // камера: спереди (−Y) справа (+X) сверху
  const Vec3 fwd = unit({-1.0, 1.4, -1.1});
  const Vec3 right = unit(cross(fwd, {0, 0, 1}));
  const Vec3 up = cross(right, fwd);
  double umin = 1e300, umax = -1e300, vmin = 1e300, vmax = -1e300;
  for (const auto& p : mesh.vertices) {
    const double u = dot(p, right), v = dot(p, up);
    umin = std::min(umin, u);
    umax = std::max(umax, u);
    vmin = std::min(vmin, v);
    vmax = std::max(vmax, v);
  }
  const double margin = 0.08;
  const double sc = std::min(W * (1 - 2 * margin) / std::max(umax - umin, 1e-9), H * (1 - 2 * margin) / std::max(vmax - vmin, 1e-9));
  const double ox = 0.5 * W - sc * 0.5 * (umin + umax), oy = 0.5 * H + sc * 0.5 * (vmin + vmax);
  auto project = [&](const Vec3& p) {
    return std::array<double, 3>{ox + sc * dot(p, right), oy - sc * dot(p, up), dot(p, fwd)};
  };
  const Vec3 light = unit({-0.4, 0.9, -1.0});  // откуда свет — немного сбоку от камеры
  const std::array<float, 3> base{0.91f, 0.58f, 0.20f};  // янтарный пластик
  for (const auto& t : mesh.triangles) {
    const Vec3& a = mesh.vertices[t[0]];
    const Vec3& b = mesh.vertices[t[1]];
    const Vec3& c = mesh.vertices[t[2]];
    const Vec3 n = unit(cross(sub(b, a), sub(c, a)));
    if (dot(n, fwd) >= 0) continue;  // задняя сторона
    const float shade = static_cast<float>(0.35 + 0.65 * std::max(0.0, -dot(n, light)));
    const auto pa = project(a), pb = project(b), pc = project(c);
    const double area = (pb[0] - pa[0]) * (pc[1] - pa[1]) - (pb[1] - pa[1]) * (pc[0] - pa[0]);
    if (std::abs(area) < 1e-12) continue;
    const int x0 = std::max(0, static_cast<int>(std::floor(std::min({pa[0], pb[0], pc[0]}))));
    const int x1 = std::min(W - 1, static_cast<int>(std::ceil(std::max({pa[0], pb[0], pc[0]}))));
    const int y0 = std::max(0, static_cast<int>(std::floor(std::min({pa[1], pb[1], pc[1]}))));
    const int y1 = std::min(H - 1, static_cast<int>(std::ceil(std::max({pa[1], pb[1], pc[1]}))));
    for (int y = y0; y <= y1; ++y)
      for (int x = x0; x <= x1; ++x) {
        const double px = x + 0.5, py = y + 0.5;
        const double w0 = ((pb[0] - px) * (pc[1] - py) - (pb[1] - py) * (pc[0] - px)) / area;
        const double w1 = ((pc[0] - px) * (pa[1] - py) - (pc[1] - py) * (pa[0] - px)) / area;
        const double w2 = 1 - w0 - w1;
        if (w0 < 0 || w1 < 0 || w2 < 0) continue;
        const auto z = static_cast<float>(w0 * pa[2] + w1 * pb[2] + w2 * pc[2]);
        const std::size_t k = static_cast<std::size_t>(y) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x);
        if (z >= depth[k]) continue;
        depth[k] = z;
        color[k] = {base[0] * shade, base[1] * shade, base[2] * shade};
      }
  }
  for (int y = 0; y < h; ++y)
    for (int x = 0; x < w; ++x) {
      float r = 0, g = 0, b = 0;
      int n = 0;
      for (int dy = 0; dy < ss; ++dy)
        for (int dx = 0; dx < ss; ++dx) {
          const std::size_t k = static_cast<std::size_t>(y * ss + dy) * static_cast<std::size_t>(W) + static_cast<std::size_t>(x * ss + dx);
          if (std::isinf(depth[k])) continue;
          r += color[k][0];
          g += color[k][1];
          b += color[k][2];
          ++n;
        }
      if (n == 0) continue;
      const std::size_t o = (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)) * 4;
      out[o] = static_cast<std::uint8_t>(std::lround(255 * r / n));
      out[o + 1] = static_cast<std::uint8_t>(std::lround(255 * g / n));
      out[o + 2] = static_cast<std::uint8_t>(std::lround(255 * b / n));
      out[o + 3] = static_cast<std::uint8_t>(std::lround(255.0 * n / (ss * ss)));
    }
  return out;
}

std::string png_rgba(const std::vector<std::uint8_t>& rgba, int w, int h) {
  if (rgba.size() != static_cast<std::size_t>(w) * h * 4) throw std::invalid_argument("png_rgba: размер не совпадает");
  std::string raw;
  raw.reserve(static_cast<std::size_t>(h) * (static_cast<std::size_t>(w) * 4 + 1));
  for (int y = 0; y < h; ++y) {
    raw.push_back(0);  // строка без фильтра
    raw.append(reinterpret_cast<const char*>(rgba.data()) + static_cast<std::size_t>(y) * w * 4, static_cast<std::size_t>(w) * 4);
  }
  uLongf zlen = compressBound(static_cast<uLong>(raw.size()));
  std::string z(zlen, '\0');
  if (compress2(reinterpret_cast<Bytef*>(z.data()), &zlen, reinterpret_cast<const Bytef*>(raw.data()), static_cast<uLong>(raw.size()), 9) != Z_OK)
    throw std::runtime_error("png_rgba: не удалось сжать");
  z.resize(zlen);
  std::string png("\x89PNG\r\n\x1a\n", 8);
  std::string ihdr;
  put_u32(ihdr, static_cast<std::uint32_t>(w));
  put_u32(ihdr, static_cast<std::uint32_t>(h));
  ihdr += std::string("\x08\x06\x00\x00\x00", 5);  // 8 бит, RGBA, сжатие, фильтр, без чересстрочности
  chunk(png, "IHDR", ihdr);
  chunk(png, "IDAT", z);
  chunk(png, "IEND", "");
  return png;
}

std::string thumbnail_block(const geometry::TriangleMesh& mesh, int w, int h) {
  const std::string b64 = base64(png_rgba(render_rgba(mesh, w, h), w, h));
  std::string out = std::format(";\n; thumbnail begin {}x{} {}\n", w, h, b64.size());
  for (std::size_t i = 0; i < b64.size(); i += 78) out += "; " + b64.substr(i, 78) + "\n";
  out += "; thumbnail end\n;\n";
  return out;
}

}  // namespace kika::slicer::detail
