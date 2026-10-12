#pragma once
// Миниатюра детали для G-code (экран принтера, Moonraker, Fluidd). Внутренний заголовок слайсера.

#include <cstdint>
#include <string>
#include <vector>

#include "kika/geometry/mesh.hpp"

namespace kika::slicer::detail {

// Картинка w × h, RGBA по строкам сверху вниз, фон прозрачный.
std::vector<std::uint8_t> render_rgba(const geometry::TriangleMesh& mesh, int w, int h);
// PNG из RGBA.
std::string png_rgba(const std::vector<std::uint8_t>& rgba, int w, int h);
// Блок комментариев «; thumbnail begin WxH LEN … ; thumbnail end» (как у PrusaSlicer — его
// понимают Moonraker, Fluidd, Mainsail).
std::string thumbnail_block(const geometry::TriangleMesh& mesh, int w, int h);

}  // namespace kika::slicer::detail
