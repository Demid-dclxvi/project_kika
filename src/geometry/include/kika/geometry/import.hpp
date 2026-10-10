#pragma once
// Чтение моделей: STL (текстовый и двоичный), 3MF (в том числе проекты Orca Slicer и Bambu Studio),
// STEP — если программа собрана с OpenCascade. Результат — треугольная сетка в мм.

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "kika/geometry/mesh.hpp"

namespace kika::geometry {

// Файл не читается или в нём нет модели (сообщение по-русски).
class ImportError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

struct ImportedModel {
  TriangleMesh mesh;        // мм, в системе координат файла
  std::string format;       // STL, 3MF, STEP
  std::string name;         // имя объекта из файла, если есть
  std::size_t objects = 0;  // сколько объектов собрано в сетку
  std::vector<std::string> warnings;
};

ImportedModel parse_stl(std::string_view data);
ImportedModel parse_3mf(std::string_view data);

// По расширению файла: .stl, .3mf, .step/.stp.
ImportedModel load_model(const std::filesystem::path& path);

// Можно ли открыть файл как модель (по расширению).
bool is_model_file(const std::filesystem::path& path);

// Собрана ли программа с чтением STEP (OpenCascade).
bool step_supported();

// G-code внутри 3MF (.gcode.3mf из Bambu Studio и Orca Slicer): Metadata/plate_N.gcode.
// plate — номер стола с 1; пусто — G-code в архиве нет.
std::optional<std::string> gcode_from_3mf(std::string_view data, int plate = 1);

// Проверка сетки после чтения: перевёрнутые нормали исправляются, о дырах — предупреждения.
void check_and_fix(ImportedModel& model);

}  // namespace kika::geometry
