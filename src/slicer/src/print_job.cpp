// Поле "print" задания ↔ настройки слайсера; поворот и постановка детали на стол.

#include "kika/slicer/print_job.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

#include "kika/analysis/job.hpp"
#include "kika/slicer/profiles.hpp"

namespace kika::slicer {

namespace {

using analysis::JobError;
using json::Value;

std::string text(const Value& o, const char* key, const std::string& def) {
  const Value* v = o.find(key);
  if (!v || v->is_null()) return def;
  if (!v->is_string()) throw JobError(std::string("print.") + key + ": ожидается строка");
  return v->as_string();
}

std::string known_printers() {
  std::string known;
  for (const auto& x : printers()) known += (known.empty() ? "" : ", ") + x.key;
  return known;
}

const Printer& printer_by_key(const std::string& key) {
  const Printer* pr = find_printer(key);
  if (!pr) throw JobError("print.printer: неизвестный принтер «" + key + "», есть: " + known_printers());
  return *pr;
}

// Основные настройки печати пишутся всегда, остальные — если отличаются от обычных.
constexpr const char* kMainKeys[] = {"layer_height", "first_layer_height", "line_width", "walls", "top_layers",
                                     "bottom_layers", "infill", "pattern", "infill_angle", "skirt", "support"};

}  // namespace

PrintJob print_from_json(const Value* print, const std::string& material) {
  PrintJob p;
  // тип пластика: из материала задания ("PETG" или {"base": "PETG", …})
  const std::string filament = material.empty() ? std::string("PLA") : material;
  const Value empty = Value::object();
  const Value& o = print ? *print : empty;
  if (!o.is_object()) throw JobError("print: ожидается объект {…} с настройками нарезки");
  Settings& s = p.settings;
  // принтер: ключ встроенного или {"base": ключ, …свои значения}
  if (const Value* pv = o.find("printer"); pv && !pv->is_null()) {
    if (pv->is_string()) {
      s.printer = printer_by_key(pv->as_string());
    } else if (pv->is_object()) {
      s.printer = printer_by_key(text(*pv, "base", default_printer().key));
      section_from_json(s, Section::Printer, *pv, "print.printer", {"base"});
    } else {
      throw JobError("print.printer: ожидается ключ принтера (" + known_printers() + ") или {\"base\": …, …}");
    }
  } else {
    s.printer = default_printer();
  }
  // пластик: тип из базы или {"base": тип, …свои значения}
  if (const Value* fv = o.find("filament"); fv && !fv->is_null()) {
    if (fv->is_string()) {
      s.filament = filament_preset(fv->as_string());
    } else if (fv->is_object()) {
      s.filament = filament_preset(text(*fv, "base", filament));
      section_from_json(s, Section::Filament, *fv, "print.filament", {"base"});
    } else {
      throw JobError("print.filament: ожидается тип пластика (PLA, PETG…) или {\"base\": …, …}");
    }
  } else {
    s.filament = filament_preset(filament);
  }
  section_from_json(s, Section::Process, o, "print", {"printer", "filament", "rotate"});
  // первый слой не указан — как остальные
  if (o.find("layer_height") && !o.find("first_layer_height")) s.first_layer_height = s.layer_height;
  if (const Value* r = o.find("rotate"); r && !r->is_null()) {
    if (!r->is_array() || r->size() != 3) throw JobError("print.rotate: ожидается [x, y, z] — углы в градусах");
    for (std::size_t a = 0; a < 3; ++a) {
      if (!r->at(a).is_number()) throw JobError("print.rotate: ожидаются числа");
      p.rotate[a] = r->at(a).as_double();
    }
  }
  return p;
}

Value print_to_json(const PrintJob& p) {
  const Settings& s = p.settings;
  Value o = Value::object();
  // принтер и пластик: ключ встроенного, а если что-то изменено — с отличиями
  {
    Settings base = s;
    const Printer* pr = find_printer(s.printer.key);
    base.printer = pr ? *pr : default_printer();
    Value diff = section_to_json(s, Section::Printer, &base);
    if (diff.as_object().empty() && pr) {
      o["printer"] = s.printer.key;
    } else {
      Value po = Value::object();
      po["base"] = base.printer.key;
      for (auto& [k, v] : diff.as_object()) po[k] = v;
      o["printer"] = std::move(po);
    }
  }
  {
    Settings base = s;
    base.filament = filament_preset(s.filament.type);
    Value diff = section_to_json(s, Section::Filament, &base);
    if (diff.as_object().empty()) {
      o["filament"] = s.filament.type;
    } else {
      Value fo = Value::object();
      fo["base"] = s.filament.type;
      for (auto& [k, v] : diff.as_object()) fo[k] = v;
      o["filament"] = std::move(fo);
    }
  }
  const Settings defaults;
  for (const auto& f : fields()) {
    if (f.section != Section::Process) continue;
    bool main = false;
    for (const char* k : kMainKeys) main = main || f.key == k;
    if (s.support != Support::None && (f.key == "support_angle" || f.key == "support_on_bed_only")) main = true;
    const Value v = f.get(s);
    if (main || json::dump(v) != json::dump(f.get(defaults))) o[f.key] = v;
  }
  if (p.rotate != std::array<double, 3>{0, 0, 0}) o["rotate"] = Value::array_of(p.rotate);
  return o;
}

geometry::Transform rotation_of(const std::array<double, 3>& rotate) {
  using geometry::Transform;
  return Transform::rotation(2, rotate[2]) * Transform::rotation(1, rotate[1]) * Transform::rotation(0, rotate[0]);
}

geometry::Transform turn_between(const std::array<double, 3>& from, const std::array<double, 3>& to) {
  // обратный к повороту — транспонированный
  geometry::Transform back = rotation_of(from);
  std::swap(back.r[1], back.r[3]);
  std::swap(back.r[2], back.r[6]);
  std::swap(back.r[5], back.r[7]);
  return rotation_of(to) * back;
}

std::array<double, 3> euler_xyz(const geometry::Transform& t) {
  // R = Rz(c)·Ry(b)·Rx(a); элементы по строкам
  const auto& r = t.r;
  auto deg = [](double rad) {
    double d = rad * 180.0 / 3.14159265358979323846;
    if (std::abs(d - std::round(d)) < 1e-9) d = std::round(d);
    if (d == -0.0) d = 0.0;
    return d;
  };
  const double sb = std::clamp(-r[6], -1.0, 1.0);
  const double b = std::asin(sb);
  double a, c;
  if (std::abs(std::cos(b)) > 1e-9) {
    a = std::atan2(r[7], r[8]);
    c = std::atan2(r[3], r[0]);
  } else {  // b = ±90°: поворот вокруг X и Z не различить — весь в X
    c = 0;
    a = sb > 0 ? std::atan2(r[1], r[4]) : std::atan2(-r[1], r[4]);
  }
  return {deg(a), deg(b), deg(c)};
}

geometry::Transform placement(const geometry::TriangleMesh& mesh, const PrintJob& p) {
  using geometry::Transform;
  const Transform r = rotation_of(p.rotate);
  geometry::TriangleMesh tmp = mesh;
  tmp.transform(r);
  return geometry::place_on_bed(tmp, p.settings.printer.bed_x / 2, p.settings.printer.bed_y / 2) * r;
}

geometry::TriangleMesh placed(const geometry::TriangleMesh& mesh, const PrintJob& p) {
  geometry::TriangleMesh m = mesh;
  m.transform(placement(mesh, p));
  return m;
}

}  // namespace kika::slicer
