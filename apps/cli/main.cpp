// kika — консольная программа. Сейчас умеет разбирать G-code; расчёт добавится
// по мере переноса модулей прототипа (см. docs/ARCHITECTURE.md).

#include <array>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>

#include "kika/gcode/parser.hpp"
#include "kika/version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using kika::gcode::Role;
using Json = nlohmann::ordered_json;

fs::path path_from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8_from_path(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

// Число с десятичной запятой, как принято в отчётах.
std::string num(double v, int decimals) {
  std::string s = std::format("{:.{}f}", v, decimals);
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

template <class T>
Json opt(const std::optional<T>& v) {
  return v ? Json(*v) : Json(nullptr);
}

constexpr std::array<Role, 5> kPartRoles = {Role::OuterWall, Role::InnerWall, Role::Solid, Role::Sparse,
                                            Role::Unknown};

Json info_json(const fs::path& file, const kika::gcode::Toolpaths& tp, double seconds) {
  const auto& i = tp.info;
  const auto bb = tp.bbox();
  Json roles = Json::object();
  for (Role r : kPartRoles) {
    std::size_t count = 0;
    double volume = 0.0;
    for (const auto& s : tp.segments)
      if (s.role == r) {
        ++count;
        volume += s.volume;
      }
    roles[std::string(kika::gcode::role_key(r))] = {{"segments", count}, {"volume_mm3", volume}};
  }
  Json settings = Json::object();
  for (const auto& [k, v] : i.settings) settings[k] = v;
  return Json{
      {"file", utf8_from_path(file.filename())},
      {"slicer", i.slicer},
      {"filament_type", i.filament_type},
      {"filament_diameter", i.filament_diameter},
      {"filament_density", opt(i.filament_density)},
      {"layer_height", opt(i.layer_height)},
      {"first_layer_height", opt(i.first_layer_height)},
      {"nozzle_diameter", opt(i.nozzle_diameter)},
      {"line_width", opt(i.line_width)},
      {"infill_density", opt(i.infill_density)},
      {"infill_pattern", i.infill_pattern},
      {"wall_loops", opt(i.wall_loops)},
      {"top_layers", opt(i.top_layers)},
      {"bottom_layers", opt(i.bottom_layers)},
      {"n_lines", i.n_lines},
      {"n_layers", i.n_layers},
      {"segments", tp.size()},
      {"volume_mm3", tp.total_volume()},
      {"excluded_volume_mm3", i.excluded_volume},
      {"filament_used_mm", i.filament_used_mm},
      {"bbox", {{"min", bb.min}, {"max", bb.max}}},
      {"roles", roles},
      {"settings", settings},
      {"warnings", i.warnings},
      {"parse_time_s", seconds},
  };
}

void print_info_text(const fs::path& file, const kika::gcode::Toolpaths& tp, double seconds) {
  const auto& i = tp.info;
  const auto bb = tp.bbox();
  std::cout << "Файл:          " << utf8_from_path(file.filename()) << "\n";
  std::cout << "Слайсер:       " << i.slicer << "\n";
  std::cout << "Пластик:       " << (i.filament_type.empty() ? "не указан" : i.filament_type) << ", пруток "
            << num(i.filament_diameter, 2) << " мм\n";
  std::cout << "Слой:          " << num(i.layer_height.value_or(0.0), 3) << " мм, первый "
            << num(i.first_layer_height.value_or(0.0), 3) << " мм, слоёв " << i.n_layers << "\n";
  std::cout << "Заполнение:    ";
  if (i.infill_density)
    std::cout << num(*i.infill_density * 100.0, 0) << "%";
  else
    std::cout << "?";
  if (!i.infill_pattern.empty()) std::cout << " " << i.infill_pattern;
  if (i.wall_loops) std::cout << ", стенок " << *i.wall_loops;
  if (i.top_layers) std::cout << ", сверху " << *i.top_layers;
  if (i.bottom_layers) std::cout << ", снизу " << *i.bottom_layers;
  std::cout << "\n";
  std::cout << "Габарит:       " << num(bb.max[0] - bb.min[0], 1) << " × " << num(bb.max[1] - bb.min[1], 1)
            << " × " << num(bb.max[2] - bb.min[2], 1) << " мм (X " << num(bb.min[0], 1) << "…"
            << num(bb.max[0], 1) << ", Y " << num(bb.min[1], 1) << "…" << num(bb.max[1], 1) << ", Z "
            << num(bb.min[2], 2) << "…" << num(bb.max[2], 2) << ")\n";
  std::cout << "Отрезков:      " << tp.size() << ", объём детали " << num(tp.total_volume(), 1) << " мм³\n";
  for (Role r : kPartRoles) {
    std::size_t count = 0;
    double volume = 0.0;
    for (const auto& s : tp.segments)
      if (s.role == r) {
        ++count;
        volume += s.volume;
      }
    if (count)
      std::cout << "  " << kika::gcode::role_name(r) << ": " << count << " отрезков, " << num(volume, 1)
                << " мм³\n";
  }
  std::cout << "Отброшено:     " << num(i.excluded_volume, 1)
            << " мм³ (поддержки, юбка, кайма, башня, стартовый код)\n";
  std::cout << "Разбор:        " << num(seconds, 3) << " с, строк " << i.n_lines << "\n";
  for (const auto& w : i.warnings) std::cout << "Внимание: " << w << "\n";
}

int cmd_info(const std::string& file_arg, bool as_json) {
  const fs::path file = path_from_utf8(file_arg);
  const auto t0 = std::chrono::steady_clock::now();
  const auto tp = kika::gcode::load(file);
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  if (as_json)
    std::cout << info_json(file, tp, seconds).dump(2) << "\n";
  else
    print_info_text(file, tp, seconds);
  return 0;
}

int cmd_segments(const std::string& file_arg, const std::string& out_arg) {
  const auto tp = kika::gcode::load(path_from_utf8(file_arg));
  std::ofstream file_out;
  std::ostream* out = &std::cout;
  if (!out_arg.empty()) {
    file_out.open(path_from_utf8(out_arg), std::ios::binary);
    if (!file_out) throw std::runtime_error("Не удалось создать файл: " + out_arg);
    out = &file_out;
  }
  *out << "x0,y0,x1,y1,z,h,volume,role,layer\n";
  for (const auto& s : tp.segments)
    *out << std::format("{},{},{},{},{},{},{},{},{}\n", s.x0, s.y0, s.x1, s.y1, s.z, s.h, s.volume,
                        static_cast<int>(s.role), s.layer);
  return 0;
}

// Окно консоли открыто только ради kika: запуск двойным щелчком или перетаскиванием
// файла на kika.exe. Тогда перед выходом ждём Enter, иначе окно закроется раньше,
// чем текст успеют прочитать. При запуске из командной строки и в CI не ждём.
bool own_console_window() {
#ifdef _WIN32
  DWORD ids[2];
  if (GetConsoleProcessList(ids, 2) != 1) return false;
  return GetFileType(GetStdHandle(STD_INPUT_HANDLE)) == FILE_TYPE_CHAR &&
         GetFileType(GetStdHandle(STD_OUTPUT_HANDLE)) == FILE_TYPE_CHAR;
#else
  return false;
#endif
}

int finish(int code) {
  if (own_console_window()) {
    std::cout << "\nНажмите Enter, чтобы закрыть окно…" << std::flush;
    std::string line;
    std::getline(std::cin, line);
  }
  return code;
}

void print_usage() {
  std::cout << "kika " << kika::kVersion << " — расчёт прочности деталей для FDM-печати\n\n"
            << "Перетащите файл G-code на kika.exe — программа покажет, что в нём найдено:\n"
            << "слайсер, пластик, слои, объём по типам линий.\n\n"
            << "Из командной строки:\n"
            << "  kika деталь.gcode                         то же, что перетаскивание\n"
            << "  kika info деталь.gcode                    сведения о печати\n"
            << "  kika info деталь.gcode --json             то же в JSON\n"
            << "  kika segments деталь.gcode -o отрезки.csv все отрезки экструзии в CSV\n"
            << "  kika --help                               справка по всем командам\n";
}

// «kika файл.gcode [файл2.gcode …]» — файлы, перетащенные на kika.exe.
bool all_existing_files(int argc, char** argv) {
  if (argc < 2) return false;
  for (int k = 1; k < argc; ++k) {
    const std::string a = argv[k];
    if (a.empty() || a.front() == '-' || a == "info" || a == "segments") return false;
    std::error_code ec;
    if (!fs::is_regular_file(path_from_utf8(a), ec)) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
  SetConsoleOutputCP(CP_UTF8);
#endif
  CLI::App app{"kika — расчёт прочности деталей для FDM-печати по траекториям печати"};
  argv = app.ensure_utf8(argv);

  if (argc == 1) {
    print_usage();
    return finish(0);
  }
  if (all_existing_files(argc, argv)) {
    int code = 0;
    for (int k = 1; k < argc; ++k) {
      if (k > 1) std::cout << "\n";
      try {
        cmd_info(argv[k], false);
      } catch (const std::exception& e) {
        std::cerr << "Ошибка: " << e.what() << "\n";
        code = 1;
      }
    }
    return finish(code);
  }

  app.set_version_flag("--version", std::string(kika::kVersion));
  app.require_subcommand(1);

  std::string gcode_file;
  bool as_json = false;
  auto* info = app.add_subcommand("info", "Что извлечено из G-code: слайсер, пластик, слои, объём");
  info->add_option("gcode", gcode_file, "Файл G-code")->required();
  info->add_flag("--json", as_json, "Вывести в формате JSON");

  std::string out_file;
  auto* segs = app.add_subcommand("segments", "Отрезки экструзии в CSV (для сверки и отладки)");
  segs->add_option("gcode", gcode_file, "Файл G-code")->required();
  segs->add_option("-o,--out", out_file, "Куда записать CSV (по умолчанию — на экран)");

  try {
    app.parse(argc, argv);
  } catch (const CLI::ParseError& e) {
    return finish(app.exit(e));
  }

  try {
    if (*info) return finish(cmd_info(gcode_file, as_json));
    if (*segs) return finish(cmd_segments(gcode_file, out_file));
  } catch (const std::exception& e) {
    std::cerr << "Ошибка: " << e.what() << "\n";
    return finish(1);
  }
  return finish(0);
}
