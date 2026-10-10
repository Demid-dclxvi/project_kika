// Чтение модели по расширению файла и проверка сетки после чтения.

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <sstream>

#include "kika/geometry/import.hpp"

namespace kika::geometry {

#ifdef KIKA_HAS_STEP
// step.cpp — только в сборке с OpenCascade
ImportedModel load_step(const std::filesystem::path& path);
#endif

namespace {

std::string utf8(const std::filesystem::path& p);

std::string lower_ext(const std::filesystem::path& p) {
  std::string e = utf8(p.extension());
  for (char& c : e) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return e;
}

std::string utf8(const std::filesystem::path& p) {
  const auto u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string read_file(const std::filesystem::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw ImportError("Не удалось открыть файл.");
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

}  // namespace

bool step_supported() {
#ifdef KIKA_HAS_STEP
  return true;
#else
  return false;
#endif
}

bool is_model_file(const std::filesystem::path& path) {
  const std::string e = lower_ext(path);
  return e == ".stl" || e == ".3mf" || e == ".step" || e == ".stp";
}

void check_and_fix(ImportedModel& m) {
  merge_vertices(m.mesh, 1e-5);
  if (m.mesh.empty()) throw ImportError("В модели нет треугольников.");
  const MeshCheck c = check(m.mesh);
  // нормали внутрь (весь объём отрицательный) — переворачиваем
  if (m.mesh.volume() < 0) {
    m.mesh.flip();
    m.warnings.push_back("Нормали модели смотрели внутрь — исправлено.");
  }
  if (c.open_edges > 0)
    m.warnings.push_back(std::format("Поверхность не замкнута: {} рёбер без пары. Нарезка закроет небольшие щели, "
                                     "но большие дыры дадут пустоты — лучше исправить модель в CAD.",
                                     c.open_edges));
  if (c.nonmanifold_edges > 0)
    m.warnings.push_back(std::format("{} рёбер принадлежат больше чем двум треугольникам (самопересечения или "
                                     "склеенные тела) — нарезка их объединит.",
                                     c.nonmanifold_edges));
  if (c.flipped_edges > 0)
    m.warnings.push_back(std::format("У части треугольников обход не согласован с соседями ({} рёбер).", c.flipped_edges));
  const Vec3 s = m.mesh.bbox().size();
  const double big = std::max({s[0], s[1], s[2]});
  if (big < 1.0)
    m.warnings.push_back(std::format("Модель очень маленькая ({:.3g} мм): возможно, она сохранена в метрах.", big));
  else if (big > 2000.0)
    m.warnings.push_back(std::format("Модель очень большая ({:.0f} мм): возможно, она сохранена в микронах.", big));
}

ImportedModel load_model(const std::filesystem::path& path) {
  const std::string e = lower_ext(path);
  ImportedModel m;
  if (e == ".step" || e == ".stp") {
#ifdef KIKA_HAS_STEP
    m = load_step(path);
#else
    throw ImportError("Эта сборка программы не открывает STEP: в ней нет OpenCascade. STEP открывают kika-gui и kika "
                      "из папки программы с окном; или сохраните модель в STL или 3MF.");
#endif
  } else if (e == ".stl") {
    m = parse_stl(read_file(path));
  } else if (e == ".3mf") {
    m = parse_3mf(read_file(path));
  } else {
    throw ImportError("Неизвестный формат модели «" + e + "»: поддерживаются STL, 3MF" +
                      (step_supported() ? ", STEP." : "."));
  }
  if (m.name.empty()) m.name = utf8(path.stem());
  check_and_fix(m);
  return m;
}

}  // namespace kika::geometry
