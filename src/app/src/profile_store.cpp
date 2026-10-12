// Свои профили нарезки в папке настроек.

#include "kika/app/profile_store.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

#include "kika/analysis/job.hpp"
#include "kika/util/json.hpp"

namespace kika::app {

namespace {

namespace fs = std::filesystem;
using json::Value;

fs::path from_utf8(const std::string& s) { return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size())); }
std::string to_utf8(const fs::path& p) {
  const std::u8string u = p.u8string();
  return std::string(u.begin(), u.end());
}

#ifndef _WIN32
std::string env(const char* name) {
  const char* v = std::getenv(name);
  return v ? std::string(v) : std::string();
}
#endif

// Имя файла из имени профиля: без символов, запрещённых в именах файлов.
std::string file_stem(const std::string& name) {
  std::string out;
  for (char c : name) {
    const auto u = static_cast<unsigned char>(c);
    out.push_back(u < 0x20 || std::string_view("\\/:*?\"<>|").find(c) != std::string_view::npos ? '_' : c);
  }
  while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
  return out.empty() ? std::string("профиль") : out;
}

fs::path section_dir(slicer::Section section) {
  switch (section) {
    case slicer::Section::Printer: return config_dir() / "printers";
    case slicer::Section::Filament: return config_dir() / "filaments";
    case slicer::Section::Process: return config_dir() / "process";
  }
  return config_dir();
}

// Встроенный профиль раздела, от которого считаются отличия.
slicer::Settings base_of(slicer::Section section, const std::string& base, const slicer::Settings& s) {
  slicer::Settings b = s;
  switch (section) {
    case slicer::Section::Printer: {
      const slicer::Printer* pr = slicer::find_printer(base);
      b.printer = pr ? *pr : slicer::default_printer();
      break;
    }
    case slicer::Section::Filament: b.filament = slicer::filament_preset(base); break;
    case slicer::Section::Process: {
      const slicer::Settings d;
      for (const auto& f : slicer::fields())
        if (f.section == slicer::Section::Process) f.set(b, f.get(d));
      break;
    }
  }
  return b;
}

std::string base_key(slicer::Section section, const slicer::Settings& s) {
  switch (section) {
    case slicer::Section::Printer: return slicer::find_printer(s.printer.key) ? s.printer.key : slicer::default_printer().key;
    case slicer::Section::Filament: return s.filament.type;
    case slicer::Section::Process: return "default";
  }
  return {};
}

}  // namespace

fs::path config_dir() {
#ifdef _WIN32
  if (const wchar_t* own = _wgetenv(L"KIKA_CONFIG_DIR"); own && *own) return fs::path(own);
  if (const wchar_t* appdata = _wgetenv(L"APPDATA"); appdata && *appdata) return fs::path(appdata) / "Kika";
#elif defined(__APPLE__)
  if (const auto e = env("KIKA_CONFIG_DIR"); !e.empty()) return from_utf8(e);
  if (const auto home = env("HOME"); !home.empty()) return from_utf8(home) / "Library" / "Application Support" / "Kika";
#else
  if (const auto e = env("KIKA_CONFIG_DIR"); !e.empty()) return from_utf8(e);
  if (const auto x = env("XDG_CONFIG_HOME"); !x.empty()) return from_utf8(x) / "kika";
  if (const auto home = env("HOME"); !home.empty()) return from_utf8(home) / ".config" / "kika";
#endif
  return fs::temp_directory_path() / "kika";
}

std::vector<ProfileRef> list_profiles(slicer::Section section) {
  std::vector<ProfileRef> out;
  std::error_code ec;
  const fs::path dir = section_dir(section);
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file(ec) || e.path().extension() != ".json") continue;
    std::string name = to_utf8(e.path().stem());
    try {
      std::ifstream f(e.path(), std::ios::binary);
      std::stringstream ss;
      ss << f.rdbuf();
      const Value j = json::parse(ss.str());
      if (const Value* n = j.find("name"); n && n->is_string() && !n->as_string().empty()) name = n->as_string();
    } catch (const std::exception&) {
      continue;  // испорченный файл — не показываем
    }
    out.push_back({name, e.path()});
  }
  std::sort(out.begin(), out.end(), [](const ProfileRef& a, const ProfileRef& b) { return a.name < b.name; });
  return out;
}

void load_profile(slicer::Section section, const fs::path& path, slicer::Settings& s) {
  std::ifstream f(path, std::ios::binary);
  if (!f) throw analysis::JobError("Не удалось открыть профиль " + to_utf8(path));
  std::stringstream ss;
  ss << f.rdbuf();
  Value j;
  try {
    j = json::parse(ss.str());
  } catch (const json::ParseError& e) {
    throw analysis::JobError("профиль " + to_utf8(path.filename()) + ": " + e.what());
  }
  const Value* kind = j.find("kika_profile");
  if (!kind || !kind->is_string() || kind->as_string() != slicer::section_key(section))
    throw analysis::JobError("Файл " + to_utf8(path.filename()) + " — не профиль раздела «" +
                             std::string(slicer::section_name(section)) + "».");
  const Value* base = j.find("base");
  slicer::Settings b = base_of(section, base && base->is_string() ? base->as_string() : base_key(section, s), s);
  if (const Value* v = j.find("values"); v && !v->is_null())
    slicer::section_from_json(b, section, *v, "профиль " + to_utf8(path.stem()));
  if (const Value* n = j.find("name"); n && n->is_string()) {
    if (section == slicer::Section::Printer) b.printer.name = n->as_string();
    if (section == slicer::Section::Filament) b.filament.name = n->as_string();
  }
  // раздел целиком из загруженного
  for (const auto& fd : slicer::fields())
    if (fd.section == section) fd.set(s, fd.get(b));
  if (section == slicer::Section::Printer) s.printer.key = b.printer.key;
}

fs::path save_profile(slicer::Section section, const std::string& name, const slicer::Settings& s) {
  const fs::path dir = section_dir(section);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) throw analysis::JobError("Не удалось создать папку профилей " + to_utf8(dir) + ": " + ec.message());
  const std::string base = base_key(section, s);
  slicer::Settings to_save = s;
  if (section == slicer::Section::Printer) to_save.printer.name = name;
  if (section == slicer::Section::Filament) to_save.filament.name = name;
  const slicer::Settings b = base_of(section, base, s);
  Value j = Value::object();
  j["kika_profile"] = std::string(slicer::section_key(section));
  j["name"] = name;
  j["base"] = base;
  j["values"] = slicer::section_to_json(to_save, section, &b);
  const fs::path path = dir / from_utf8(file_stem(name) + ".json");
  std::ofstream f(path, std::ios::binary);
  if (!f) throw analysis::JobError("Не удалось записать профиль " + to_utf8(path));
  const std::string text = json::dump_readable(j) + "\n";
  f.write(text.data(), static_cast<std::streamsize>(text.size()));
  return path;
}

void delete_profile(const fs::path& path) {
  std::error_code ec;
  fs::remove(path, ec);
}

}  // namespace kika::app
