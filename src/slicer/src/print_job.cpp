// Поле "print" задания ↔ настройки слайсера; поворот и постановка детали на стол.

#include "kika/slicer/print_job.hpp"

#include <algorithm>
#include <cmath>

#include "kika/analysis/job.hpp"

namespace kika::slicer {

namespace {

using analysis::JobError;
using json::Value;

double number(const Value& o, const char* key, double def, double lo, double hi) {
  const Value* v = o.find(key);
  if (!v || v->is_null()) return def;
  if (!v->is_number()) throw JobError(std::string("print.") + key + ": ожидается число");
  const double x = v->as_double();
  if (!(x >= lo && x <= hi))
    throw JobError(std::string("print.") + key + ": значение " + json::dump(*v) + " вне допустимого (" +
                   json::dump(Value(lo)) + "…" + json::dump(Value(hi)) + ")");
  return x;
}

std::string text(const Value& o, const char* key, const std::string& def) {
  const Value* v = o.find(key);
  if (!v || v->is_null()) return def;
  if (!v->is_string()) throw JobError(std::string("print.") + key + ": ожидается строка");
  return v->as_string();
}

}  // namespace

PrintJob print_from_json(const Value* print, const std::string& material) {
  PrintJob p;
  // тип пластика: из материала задания ("PETG" или {"base": "PETG", …})
  std::string filament = material.empty() ? std::string("PLA") : material;
  const Value empty = Value::object();
  const Value& o = print ? *print : empty;
  if (!o.is_object()) throw JobError("print: ожидается объект {…} с настройками нарезки");
  const std::string printer = text(o, "printer", default_printer().key);
  const Printer* pr = find_printer(printer);
  if (!pr) {
    std::string known;
    for (const auto& x : printers()) known += (known.empty() ? "" : ", ") + x.key;
    throw JobError("print.printer: неизвестный принтер «" + printer + "», есть: " + known);
  }
  Settings& s = p.settings;
  s.printer = *pr;
  s.filament = filament_preset(text(o, "filament", filament));
  s.layer_height = number(o, "layer_height", s.layer_height, 0.04, 0.6);
  s.first_layer_height = number(o, "first_layer_height", s.layer_height, 0.04, 0.6);
  s.line_width = number(o, "line_width", s.line_width, 0.2, 1.2);
  s.wall_loops = static_cast<int>(number(o, "walls", s.wall_loops, 1, 20));
  s.top_layers = static_cast<int>(number(o, "top_layers", s.top_layers, 0, 50));
  s.bottom_layers = static_cast<int>(number(o, "bottom_layers", s.bottom_layers, 0, 50));
  s.infill_density = number(o, "infill", s.infill_density * 100, 0, 100) / 100.0;
  const std::string pat = text(o, "pattern", std::string(pattern_key(s.pattern)));
  const auto pk = pattern_from_key(pat);
  if (!pk) throw JobError("print.pattern: неизвестный рисунок «" + pat + "», есть: grid, rectilinear, triangles, line");
  s.pattern = *pk;
  s.infill_angle = number(o, "infill_angle", s.infill_angle, -360, 360);
  if (const Value* sk = o.find("skirt"); sk && !sk->is_null()) {
    if (!sk->is_bool()) throw JobError("print.skirt: ожидается true или false");
    s.skirt = sk->as_bool();
  }
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
  o["printer"] = s.printer.key;
  o["filament"] = s.filament.type;
  o["layer_height"] = s.layer_height;
  o["first_layer_height"] = s.first_layer_height;
  o["line_width"] = s.line_width;
  o["walls"] = s.wall_loops;
  o["top_layers"] = s.top_layers;
  o["bottom_layers"] = s.bottom_layers;
  o["infill"] = std::round(s.infill_density * 1000) / 10;
  o["pattern"] = std::string(pattern_key(s.pattern));
  o["infill_angle"] = s.infill_angle;
  o["skirt"] = s.skirt;
  if (p.rotate != std::array<double, 3>{0, 0, 0}) o["rotate"] = Value::array_of(p.rotate);
  return o;
}

geometry::Transform rotation_of(const std::array<double, 3>& rotate) {
  using geometry::Transform;
  return Transform::rotation(2, rotate[2]) * Transform::rotation(1, rotate[1]) * Transform::rotation(0, rotate[0]);
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
