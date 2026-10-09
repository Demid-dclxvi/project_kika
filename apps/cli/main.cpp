// kika — консольная программа: сведения о G-code и расчёт прочности по заданию.

#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "kika/analysis/analysis.hpp"
#include "kika/gcode/parser.hpp"
#include "kika/material/material.hpp"
#include "kika/parallel.hpp"
#include "kika/util/json.hpp"
#include "kika/version.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

namespace fs = std::filesystem;
using kika::gcode::Role;
using kika::json::Value;

// Ошибка в командной строке: печатается вместе с подсказкой.
class UsageError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

fs::path path_from_utf8(const std::string& s) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

std::string utf8_from_path(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

std::string read_file(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Не удалось открыть файл: " + utf8_from_path(p));
  std::ostringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

void write_file(const fs::path& p, const std::string& text) {
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Не удалось создать файл: " + utf8_from_path(p));
  f << text;
}

template <class T>
void write_binary(const fs::path& p, const std::vector<T>& v) {
  std::ofstream f(p, std::ios::binary);
  if (!f) throw std::runtime_error("Не удалось создать файл: " + utf8_from_path(p));
  f.write(reinterpret_cast<const char*>(v.data()), static_cast<std::streamsize>(v.size() * sizeof(T)));
}

// Число с десятичной запятой, как принято в отчётах.
std::string num(double v, int decimals) {
  std::string s = std::format("{:.{}f}", v, decimals);
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

template <class T>
Value opt(const std::optional<T>& v) {
  return v ? Value(*v) : Value(nullptr);
}

// ---------------------------------------------------------------------------
// Разбор параметров командной строки

struct Option {
  std::vector<std::string> names;  // "-o", "--out"
  bool takes_value = false;
};

struct Args {
  std::vector<std::string> positional;
  std::map<std::string, std::string> values;  // по последнему (длинному) имени
  std::map<std::string, bool> flags;

  bool flag(const std::string& name) const { return flags.count(name) > 0; }
  std::optional<std::string> value(const std::string& name) const {
    const auto it = values.find(name);
    if (it == values.end()) return std::nullopt;
    return it->second;
  }
};

Args parse_args(const std::vector<std::string>& args, std::size_t start, const std::vector<Option>& spec) {
  Args out;
  bool only_positional = false;
  for (std::size_t i = start; i < args.size(); ++i) {
    std::string a = args[i];
    if (only_positional || a.size() < 2 || a[0] != '-' || (a[1] >= '0' && a[1] <= '9') || a[1] == '.') {
      out.positional.push_back(a);
      continue;
    }
    if (a == "--") {
      only_positional = true;
      continue;
    }
    std::optional<std::string> inline_value;
    if (const auto eq = a.find('='); a.rfind("--", 0) == 0 && eq != std::string::npos) {
      inline_value = a.substr(eq + 1);
      a = a.substr(0, eq);
    }
    const Option* found = nullptr;
    for (const auto& o : spec)
      for (const auto& n : o.names)
        if (n == a) found = &o;
    if (!found) throw UsageError("неизвестный параметр " + a);
    const std::string key = found->names.back();
    if (found->takes_value) {
      if (inline_value) {
        out.values[key] = *inline_value;
      } else {
        if (i + 1 >= args.size()) throw UsageError("после " + a + " нужно значение");
        out.values[key] = args[++i];
      }
    } else {
      if (inline_value) throw UsageError(a + " задаётся без значения");
      out.flags[key] = true;
    }
  }
  return out;
}

double parse_number(const std::string& s, const std::string& what) {
  try {
    std::size_t pos = 0;
    const double v = std::stod(s, &pos);
    if (pos == s.size()) return v;
  } catch (...) {
  }
  throw UsageError(what + ": ожидается число, а указано «" + s + "»");
}

// ---------------------------------------------------------------------------
// kika info

constexpr std::array<Role, 5> kPartRoles = {Role::OuterWall, Role::InnerWall, Role::Solid, Role::Sparse,
                                            Role::Unknown};

Value info_json(const fs::path& file, const kika::gcode::Toolpaths& tp, double seconds) {
  const auto& i = tp.info;
  const auto bb = tp.bbox();
  Value roles = Value::object();
  for (Role r : kPartRoles) {
    std::size_t count = 0;
    double volume = 0.0;
    for (const auto& s : tp.segments)
      if (s.role == r) {
        ++count;
        volume += s.volume;
      }
    Value rv = Value::object();
    rv["segments"] = count;
    rv["volume_mm3"] = volume;
    roles[kika::gcode::role_key(r)] = std::move(rv);
  }
  Value settings = Value::object();
  for (const auto& [k, v] : i.settings) settings[k] = v;
  Value bbox = Value::object();
  bbox["min"] = Value::array_of(bb.min);
  bbox["max"] = Value::array_of(bb.max);
  Value j = Value::object();
  j["file"] = utf8_from_path(file.filename());
  j["slicer"] = i.slicer;
  j["filament_type"] = i.filament_type;
  j["filament_diameter"] = i.filament_diameter;
  j["filament_density"] = opt(i.filament_density);
  j["layer_height"] = opt(i.layer_height);
  j["first_layer_height"] = opt(i.first_layer_height);
  j["nozzle_diameter"] = opt(i.nozzle_diameter);
  j["line_width"] = opt(i.line_width);
  j["infill_density"] = opt(i.infill_density);
  j["infill_pattern"] = i.infill_pattern;
  j["wall_loops"] = opt(i.wall_loops);
  j["top_layers"] = opt(i.top_layers);
  j["bottom_layers"] = opt(i.bottom_layers);
  j["n_lines"] = i.n_lines;
  j["n_layers"] = i.n_layers;
  j["segments"] = tp.size();
  j["volume_mm3"] = tp.total_volume();
  j["excluded_volume_mm3"] = i.excluded_volume;
  j["filament_used_mm"] = i.filament_used_mm;
  j["bbox"] = std::move(bbox);
  j["roles"] = std::move(roles);
  j["settings"] = std::move(settings);
  j["warnings"] = Value::array_of(i.warnings);
  j["parse_time_s"] = seconds;
  return j;
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
    std::cout << kika::json::dump(info_json(file, tp, seconds), 2) << "\n";
  else
    print_info_text(file, tp, seconds);
  return 0;
}

// ---------------------------------------------------------------------------
// kika segments

int cmd_segments(const std::string& file_arg, const std::optional<std::string>& out_arg) {
  const auto tp = kika::gcode::load(path_from_utf8(file_arg));
  std::ofstream file_out;
  std::ostream* out = &std::cout;
  if (out_arg) {
    file_out.open(path_from_utf8(*out_arg), std::ios::binary);
    if (!file_out) throw std::runtime_error("Не удалось создать файл: " + *out_arg);
    out = &file_out;
  }
  *out << "x0,y0,x1,y1,z,h,volume,role,layer\n";
  for (const auto& s : tp.segments)
    *out << std::format("{},{},{},{},{},{},{},{},{}\n", s.x0, s.y0, s.x1, s.y1, s.z, s.h, s.volume,
                        static_cast<int>(s.role), s.layer);
  return 0;
}

// ---------------------------------------------------------------------------
// kika materials

int cmd_materials() {
  for (const auto& m : kika::material::builtin_materials())
    std::cout << std::format("{:<8} {}\n         E = {:.0f}/{:.0f}/{:.0f} МПа, прочность {:.0f}/{:.0f}/{:.0f} МПа, "
                             "HDT {:.0f} °C\n",
                             m.key, m.name, m.E1, m.E2, m.E3, m.Xt, m.Yt, m.Zt, m.hdt);
  std::cout << "\nE — модуль вдоль нити / поперёк нити в слое / по Z (между слоями); прочность на растяжение "
               "в тех же направлениях.\n";
  return 0;
}

// ---------------------------------------------------------------------------
// kika run

// Печать хода расчёта, не чаще раза в 0,4 с (как в прототипе).
kika::analysis::Progress progress_printer(bool quiet) {
  if (quiet) return {};
  struct State {
    std::chrono::steady_clock::time_point last{};
    std::string text;
  };
  auto st = std::make_shared<State>();
  return [st](std::string_view, double frac, std::string_view text) {
    const auto now = std::chrono::steady_clock::now();
    if (text != st->text && (now - st->last > std::chrono::milliseconds(400) || frac >= 1.0)) {
      std::cout << std::format("  [{:5.1f}%] {}", frac * 100, text) << std::endl;
      st->last = now;
      st->text = std::string(text);
    }
  };
}

struct RunOptions {
  std::optional<std::string> gcode;
  std::string job;
  std::optional<std::string> json_out;
  std::optional<std::string> dump_dir;
  std::optional<double> voxel;
  std::optional<long long> max_elems;
  std::optional<double> tol;
  bool quiet = false;
};

void dump_arrays(const fs::path& dir, const kika::analysis::Model& model,
                 const kika::analysis::AnalysisResult& res) {
  fs::create_directories(dir);
  const auto& vm = model.vm;
  write_binary(dir / "voxel_rho_shell.bin", vm.rho_shell);
  write_binary(dir / "voxel_rho_sparse.bin", vm.rho_sparse);
  write_binary(dir / "voxel_hist_shell.bin", vm.hist_shell);
  write_binary(dir / "voxel_hist_sparse.bin", vm.hist_sparse);
  std::vector<std::int64_t> flat(vm.size());
  for (std::size_t e = 0; e < vm.size(); ++e) flat[e] = vm.flat(e);
  write_binary(dir / "voxel_flat.bin", flat);
  for (std::size_t k = 0; k < res.cases.size(); ++k) {
    const auto& c = res.cases[k];
    const std::string p = "case" + std::to_string(k) + "_";
    write_binary(dir / (p + "u.bin"), c.u);
    write_binary(dir / (p + "sf.bin"), c.sf);
    write_binary(dir / (p + "vm.bin"), c.von_mises);
    write_binary(dir / (p + "sigma.bin"), c.sigma);
    write_binary(dir / (p + "mode.bin"), c.mode);
  }
}

int cmd_run(const RunOptions& o) {
  const auto t0 = std::chrono::steady_clock::now();
  const fs::path job_path = path_from_utf8(o.job);
  kika::json::Value job_json;
  try {
    job_json = kika::json::parse(read_file(job_path));
  } catch (const kika::json::ParseError& e) {
    throw std::runtime_error("задание " + utf8_from_path(job_path.filename()) + ": " + e.what());
  }
  auto job = kika::analysis::parse_job(job_json);
  if (o.tol) job.solver_tol = *o.tol;
  std::optional<std::string> g = o.gcode ? o.gcode : job.gcode;
  if (!g) throw UsageError("Укажите файл G-code (в командной строке или полем \"gcode\" в задании).");
  fs::path gpath = path_from_utf8(*g);
  std::error_code ec;
  if (!gpath.is_absolute() && !fs::exists(gpath, ec)) {
    const fs::path cand = fs::absolute(job_path, ec).parent_path() / gpath;
    if (fs::exists(cand, ec)) gpath = cand;
  }
  kika::analysis::ModelOptions mo;
  mo.voxel = o.voxel ? o.voxel : job.voxel;
  mo.max_elems = static_cast<int>(o.max_elems ? *o.max_elems : (job.max_elems ? *job.max_elems : 150000));

  std::cout << "G-code: " << utf8_from_path(gpath) << std::endl;
  auto tp = kika::gcode::load(gpath);
  const auto model = kika::analysis::build_model(std::move(tp), mo, progress_printer(o.quiet));
  const auto sm = kika::analysis::model_summary(model);
  const std::string ft = sm.find("filament_type")->as_string();
  std::cout << std::format("  слайсер {}, пластик {}, слой {} мм, заполнение {:.0f}% {}, габарит {} мм\n",
                           sm.find("slicer")->as_string(), ft.empty() ? "?" : ft,
                           kika::json::dump(*sm.find("layer_height")), sm.find("infill_density")->as_double() * 100,
                           sm.find("infill_pattern")->as_string(), kika::json::dump(*sm.find("size")));
  std::cout << std::format("  сетка: воксель {} мм, {} элементов, {} узлов\n", kika::json::dump(*sm.find("voxel")),
                           sm.find("elements")->as_int(), sm.find("nodes")->as_int());
  for (const auto& w : sm.find("warnings")->as_array()) std::cout << "  ⚠ " << w.as_string() << "\n";
  std::cout << std::flush;

  const auto res = kika::analysis::run_analysis(model, job, progress_printer(o.quiet));
  std::cout << "\n" << kika::analysis::text_summary(res) << "\n";
  if (o.json_out) {
    Value arr = Value::array();
    for (const auto& c : res.cases) arr.push_back(kika::analysis::to_json(c.summary));
    write_file(path_from_utf8(*o.json_out), kika::json::dump(arr, 2) + "\n");
    std::cout << "\nИтоги в JSON: " << *o.json_out << "\n";
  }
  if (o.dump_dir) dump_arrays(path_from_utf8(*o.dump_dir), model, res);
  const double total = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::cout << "\nВсего " << num(total, 1) << " с\n";
  return 0;
}

// ---------------------------------------------------------------------------

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
            << "Перетащите на kika.exe файл G-code — программа покажет, что в нём найдено:\n"
            << "слайсер, пластик, слои, объём по типам линий. Перетащите файл задания (.json) —\n"
            << "программа посчитает прочность детали по этому заданию.\n\n"
            << "Из командной строки:\n"
            << "  kika деталь.gcode                         то же, что перетаскивание\n"
            << "  kika info деталь.gcode [--json]           сведения о печати\n"
            << "  kika run [деталь.gcode] задание.json      расчёт прочности\n"
            << "      --json итоги.json                     записать итоги в JSON\n"
            << "      --voxel 0.5                           размер вокселя, мм\n"
            << "      --max-elems 150000                    предел числа элементов\n"
            << "      --threads 4                           число потоков (по умолчанию — все ядра)\n"
            << "      --tol 1e-7                            точность решателя (относительная невязка)\n"
            << "      --quiet                               без строк хода расчёта\n"
            << "  kika materials                            встроенная база материалов\n"
            << "  kika segments деталь.gcode -o отрезки.csv все отрезки экструзии в CSV\n"
            << "  kika --version                            версия\n\n"
            << "Пример задания: prototype/examples/bracket_side.job.json\n";
}

bool ends_with_json(const std::string& s) {
  if (s.size() < 5) return false;
  std::string t = s.substr(s.size() - 5);
  for (char& c : t) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return t == ".json";
}

// Файлы, перетащенные на kika.exe: G-code — сведения, задание .json — расчёт.
std::optional<int> run_dropped_files(const std::vector<std::string>& args) {
  if (args.size() < 2) return std::nullopt;
  std::vector<std::string> jobs, gcodes;
  for (std::size_t k = 1; k < args.size(); ++k) {
    const std::string& a = args[k];
    if (a.empty() || a.front() == '-') return std::nullopt;
    std::error_code ec;
    if (!fs::is_regular_file(path_from_utf8(a), ec)) return std::nullopt;
    (ends_with_json(a) ? jobs : gcodes).push_back(a);
  }
  int code = 0;
  if (!jobs.empty()) {
    for (std::size_t k = 0; k < jobs.size(); ++k) {
      if (k) std::cout << "\n";
      RunOptions o;
      o.job = jobs[k];
      if (gcodes.size() == 1) o.gcode = gcodes[0];
      try {
        cmd_run(o);
      } catch (const std::exception& e) {
        std::cout << std::flush;
        std::cerr << "Ошибка: " << e.what() << "\n";
        code = 1;
      }
    }
    return code;
  }
  for (std::size_t k = 0; k < gcodes.size(); ++k) {
    if (k) std::cout << "\n";
    try {
      cmd_info(gcodes[k], false);
    } catch (const std::exception& e) {
      std::cerr << "Ошибка: " << e.what() << "\n";
      code = 1;
    }
  }
  return code;
}

int run_main(const std::vector<std::string>& args) {
  if (args.size() <= 1) {
    print_usage();
    return finish(0);
  }
  const std::string& cmd = args[1];
  if (cmd == "--version" || cmd == "-V") {
    std::cout << kika::kVersion << "\n";
    return 0;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help" || cmd == "/?") {
    print_usage();
    return finish(0);
  }
  if (cmd != "info" && cmd != "segments" && cmd != "run" && cmd != "materials")
    if (auto code = run_dropped_files(args)) return finish(*code);

  try {
    if (cmd == "info") {
      const auto a = parse_args(args, 2, {{{"--json"}, false}});
      if (a.positional.size() != 1) throw UsageError("укажите один файл G-code: kika info деталь.gcode");
      return finish(cmd_info(a.positional[0], a.flag("--json")));
    }
    if (cmd == "segments") {
      const auto a = parse_args(args, 2, {{{"-o", "--out"}, true}});
      if (a.positional.size() != 1) throw UsageError("укажите один файл G-code: kika segments деталь.gcode");
      return finish(cmd_segments(a.positional[0], a.value("--out")));
    }
    if (cmd == "materials") {
      parse_args(args, 2, {});
      return finish(cmd_materials());
    }
    if (cmd == "run") {
      const auto a = parse_args(args, 2,
                                {{{"--json"}, true},
                                 {{"--dump"}, true},
                                 {{"--voxel"}, true},
                                 {{"--max-elems"}, true},
                                 {{"--threads"}, true},
                                 {{"--tol"}, true},
                                 {{"-q", "--quiet"}, false}});
      RunOptions o;
      if (a.positional.size() == 1) {
        o.job = a.positional[0];
      } else if (a.positional.size() == 2) {
        o.gcode = a.positional[0];
        o.job = a.positional[1];
      } else {
        throw UsageError("укажите задание: kika run [деталь.gcode] задание.json");
      }
      o.json_out = a.value("--json");
      o.dump_dir = a.value("--dump");
      if (auto v = a.value("--voxel")) o.voxel = parse_number(*v, "--voxel");
      if (auto v = a.value("--max-elems")) o.max_elems = static_cast<long long>(parse_number(*v, "--max-elems"));
      if (auto v = a.value("--tol")) o.tol = parse_number(*v, "--tol");
      if (auto v = a.value("--threads")) kika::set_thread_count(static_cast<int>(parse_number(*v, "--threads")));
      o.quiet = a.flag("--quiet");
      return finish(cmd_run(o));
    }
    throw UsageError("неизвестная команда «" + cmd + "» (или файл не найден)");
  } catch (const UsageError& e) {
    std::cout << std::flush;
    std::cerr << "Ошибка: " << e.what() << "\nСправка: kika --help\n";
    return finish(2);
  } catch (const std::exception& e) {
    std::cout << std::flush;
    std::cerr << "Ошибка: " << e.what() << "\n";
    return finish(1);
  }
}

}  // namespace

#ifdef _WIN32
namespace {
std::string utf8_from_wide(const wchar_t* w) {
  const int len = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
  if (len <= 1) return {};
  std::string s(static_cast<std::size_t>(len - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), len, nullptr, nullptr);
  return s;
}
}  // namespace

// На Windows параметры берём в UTF-16: так русские имена файлов приходят без потерь.
int wmain(int argc, wchar_t** argv) {
  SetConsoleOutputCP(CP_UTF8);
  std::vector<std::string> args;
  for (int i = 0; i < argc; ++i) args.push_back(utf8_from_wide(argv[i]));
  return run_main(args);
}
#else
int main(int argc, char** argv) { return run_main(std::vector<std::string>(argv, argv + argc)); }
#endif
