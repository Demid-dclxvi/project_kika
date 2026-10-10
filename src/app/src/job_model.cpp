// Перенос из fdmfea/web/app.js (toBackendJob, fromBackendJob, defaultsFor, loadVec)
// и fdmfea/server.py (_resolve_job).

#include "kika/app/job_model.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <stdexcept>

namespace kika::app {

namespace {

using json::Value;

const Value* get(const Value& o, std::string_view k) {
  const Value* v = o.find(k);
  return (v && !v->is_null()) ? v : nullptr;
}

double num_or(const Value* v, double def) { return (v && v->is_number()) ? v->as_double() : def; }

std::string str_or(const Value* v, const std::string& def) {
  return (v && v->is_string() && !v->as_string().empty()) ? v->as_string() : def;
}

double round_to(double v, int digits) {
  const double k = std::pow(10.0, digits);
  return std::round(v * k) / k;
}

struct DirInfo {
  std::string dir;
  Vec3 vec;
  double mag;
};

// Вектор → направление по оси, если он вдоль оси, иначе «свой вектор».
DirInfo vec_to_dir(const Vec3& v) {
  const double l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(l > 0)) return {"-z", {0, 0, -1}, 0};
  for (std::size_t a = 0; a < 3; ++a)
    if (std::abs(std::abs(v[a]) - l) < 1e-9 * l)
      return {std::string(v[a] < 0 ? "-" : "+") + "xyz"[a], {v[0] / l, v[1] / l, v[2] / l}, l};
  return {"custom", {round_to(v[0] / l, 4), round_to(v[1] / l, 4), round_to(v[2] / l, 4)}, l};
}

std::optional<Vec3> vec3_of(const Value& v) {
  if (!v.is_array() || v.size() != 3) return std::nullopt;
  Vec3 out{};
  for (std::size_t i = 0; i < 3; ++i) {
    if (!v.at(i).is_number()) return std::nullopt;
    out[i] = v.at(i).as_double();
  }
  return out;
}

Vec3 axis_vec(const std::string& dir) {
  Vec3 v{0, 0, 0};
  if (dir.size() == 2) {
    const int ax = dir[1] == 'x' ? 0 : (dir[1] == 'y' ? 1 : 2);
    v[static_cast<std::size_t>(ax)] = dir[0] == '-' ? -1.0 : 1.0;
  }
  return v;
}

// Направление нагрузки: ось, нормаль поверхности или свой вектор.
Vec3 dir_vec(const std::string& dir, const Vec3& custom, const std::vector<std::int64_t>& faces) {
  if (dir == "custom") return custom;
  if (dir == "normal_in" || dir == "normal_out") {
    Vec3 n{0, 0, 0};
    constexpr int dirs[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (auto k : faces)
      for (std::size_t a = 0; a < 3; ++a) n[a] += dirs[k % 6][a];
    double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (!(l > 0)) l = 1;
    const double s = dir == "normal_in" ? -1.0 : 1.0;
    return {s * n[0] / l, s * n[1] / l, s * n[2] / l};
  }
  return axis_vec(dir);
}

Value faces_json(const std::vector<std::int64_t>& faces) {
  Value w = Value::object();
  w["faces"] = Value::array_of(faces);
  return w;
}

Value dir_json(const UiLoad& l) {
  if (l.dir == "custom") return Value::array_of(l.vec);
  return Value(l.dir);
}

// Область задания → ключи граней текущей сетки.
std::vector<std::int64_t> resolve_faces(const Value& where, const analysis::Model& model, const analysis::Vec3& origin,
                                        const std::string& name) {
  if (where.is_object())
    if (const Value* f = where.find("faces")) {
      std::vector<std::int64_t> keys;
      if (f->is_array())
        for (const auto& k : f->as_array()) keys.push_back(k.as_int());
      std::sort(keys.begin(), keys.end());
      keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
      return keys;
    }
  const auto region = analysis::region_from_json(where, name);
  const auto idx = analysis::select_faces(model.mesh, region, origin, name);
  std::vector<std::int64_t> keys;
  keys.reserve(idx.size());
  for (auto i : idx) keys.push_back(model.mesh.face_key[static_cast<std::size_t>(i)]);
  std::sort(keys.begin(), keys.end());
  return keys;
}

}  // namespace

const std::vector<LoadTypeInfo>& load_types() {
  static const std::vector<LoadTypeInfo> t = {
      {"force", "Сила", "Н", true},
      {"mass", "Подвешенный груз", "кг", true},
      {"bearing", "Нагрузка на отверстие", "Н", true},
      {"pressure", "Давление", "МПа", true},
      {"moment", "Крутящий момент", "Н·мм", true},
      {"impact", "Удар падающим грузом", "кг", true},
      {"displacement", "Заданное перемещение", "мм", true},
      {"gravity", "Собственный вес / перегрузка", "g", false},
  };
  return t;
}

const LoadTypeInfo& load_type(const std::string& key) {
  for (const auto& t : load_types())
    if (key == t.key) return t;
  return load_types().front();
}

const std::vector<std::pair<const char*, const char*>>& directions() {
  static const std::vector<std::pair<const char*, const char*>> d = {
      {"-z", "−Z"}, {"+z", "+Z"}, {"-x", "−X"}, {"+x", "+X"}, {"-y", "−Y"}, {"+y", "+Y"},
      {"normal_in", "внутрь поверхности"}, {"normal_out", "от поверхности"}, {"custom", "свой вектор"}};
  return d;
}

const std::vector<std::pair<const char*, const char*>>& axis_directions() {
  static const std::vector<std::pair<const char*, const char*>> d = {
      {"-z", "−Z"}, {"+z", "+Z"}, {"-x", "−X"}, {"+x", "+X"}, {"-y", "−Y"}, {"+y", "+Y"}, {"custom", "свой вектор"}};
  return d;
}

const std::vector<std::pair<const char*, const char*>>& durations() {
  static const std::vector<std::pair<const char*, const char*>> d = {
      {"short", "Кратковременная"}, {"long", "Длительная (ползучесть)"}, {"cyclic", "Циклическая (усталость)"}};
  return d;
}

UiJob new_job() {
  UiJob j;
  UiCase c;
  c.id = j.new_id();
  c.name = "Основная нагрузка";
  j.cases.push_back(std::move(c));
  return j;
}

UiLoad default_load(const std::string& type, int id) {
  UiLoad d;
  d.id = id;
  d.type = type;
  if (type == "mass") d.value = 1;
  if (type == "pressure") d.value = 0.1;
  if (type == "moment") d.value = 1000;
  if (type == "impact") d.value = 0.5;
  if (type == "gravity") d.value = 1;
  if (type == "displacement") d.disp = {std::nullopt, std::nullopt, -1.0};
  if (type == "pressure") d.dir = "normal_in";
  return d;
}

bool can_run(const UiJob& job) {
  bool fix = false, load = false;
  for (const auto& f : job.fixtures) fix = fix || !f.faces.empty();
  for (const auto& c : job.cases) load = load || !c.loads.empty();
  return fix && load;
}

std::optional<Vec3> load_arrow(const UiLoad& l, const Scene& /*scene*/) {
  if (l.type == "pressure") return dir_vec("normal_in", {}, l.faces);
  if (l.type == "moment") return std::nullopt;
  if (l.type == "displacement") {
    const Vec3 v{l.disp[0].value_or(0.0), l.disp[1].value_or(0.0), l.disp[2].value_or(0.0)};
    if (std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]) > 0) return v;
    return std::nullopt;
  }
  return dir_vec(l.dir, l.vec, l.faces);
}

material::Material job_material(const UiJob& job) {
  Value spec;
  if (job.overrides.empty()) {
    spec = Value(job.material);
  } else {
    spec = Value::object();
    spec["base"] = job.material;
    for (const auto& [k, v] : job.overrides) spec[k] = v;
  }
  try {
    return analysis::material_from_json(spec);
  } catch (const std::exception&) {
    return *material::find_material("PLA");
  }
}

json::Value to_job_json(const UiJob& job, const std::string& title, const std::string& gcode_name,
                        long long max_elems) {
  Value j = Value::object();
  j["title"] = title;
  if (!gcode_name.empty()) j["gcode"] = gcode_name;
  if (max_elems > 0) j["max_elems"] = max_elems;
  if (job.overrides.empty()) {
    j["material"] = job.material;
  } else {
    Value m = Value::object();
    m["base"] = job.material;
    for (const auto& [k, v] : job.overrides) m[k] = v;
    j["material"] = std::move(m);
  }
  j["target_sf"] = job.target_sf > 0 ? job.target_sf : 2.0;
  Value fixtures = Value::array();
  for (const auto& f : job.fixtures) {
    if (f.faces.empty()) continue;
    Value o = Value::object();
    o["name"] = f.name;
    o["where"] = faces_json(f.faces);
    o["components"] = f.components;
    fixtures.push_back(std::move(o));
  }
  j["fixtures"] = std::move(fixtures);
  Value cases = Value::array();
  for (const auto& c : job.cases) {
    if (c.loads.empty()) continue;
    Value o = Value::object();
    o["name"] = c.name;
    o["duration"] = c.duration;
    o["cycles"] = c.cycles;
    o["temperature"] = c.temperature ? Value(*c.temperature) : Value(nullptr);
    o["thermal_expansion"] = c.thermal;
    Value loads = Value::array();
    for (const auto& l : c.loads) {
      Value d = Value::object();
      const auto& t = l.type;
      if (t == "force" || t == "bearing") {
        d["type"] = t;
        d["where"] = faces_json(l.faces);
        d["value"] = l.value;
        d["direction"] = dir_json(l);
      } else if (t == "mass") {
        d["type"] = "mass";
        d["where"] = faces_json(l.faces);
        d["kg"] = l.value;
        d["direction"] = dir_json(l);
      } else if (t == "pressure") {
        d["type"] = "pressure";
        d["where"] = faces_json(l.faces);
        d["value"] = l.value;
      } else if (t == "moment") {
        d["type"] = "moment";
        d["where"] = faces_json(l.faces);
        d["value"] = l.value;
        d["axis"] = l.axis == "custom" ? Value::array_of(l.vec) : Value(l.axis);
      } else if (t == "impact") {
        d["type"] = "impact";
        d["where"] = faces_json(l.faces);
        d["kg"] = l.value;
        d["height"] = l.height;
        d["direction"] = dir_json(l);
      } else if (t == "displacement") {
        d["type"] = "displacement";
        d["where"] = faces_json(l.faces);
        Value v = Value::array();
        for (const auto& x : l.disp) v.push_back(x ? Value(*x) : Value(nullptr));
        d["vector"] = std::move(v);
      } else if (t == "gravity") {
        const Vec3 v = dir_vec(l.dir, l.vec, {});
        d["type"] = "gravity";
        d["g"] = Value::array_of(Vec3{v[0] * l.value, v[1] * l.value, v[2] * l.value});
      } else {
        continue;
      }
      loads.push_back(std::move(d));
    }
    o["loads"] = std::move(loads);
    cases.push_back(std::move(o));
  }
  j["cases"] = std::move(cases);
  return j;
}

UiJob from_job_json(const json::Value& job, const analysis::Model& model, std::vector<std::string>* warnings) {
  if (!job.is_object()) throw analysis::JobError("Задание должно быть объектом JSON {…}");
  const Value* coords = job.find("coords");
  const bool part = !coords || (coords->is_string() && coords->as_string() == "part");
  const analysis::Vec3 origin = part ? model.origin : analysis::Vec3{0, 0, 0};
  UiJob out;
  if (const Value* m = get(job, "material")) {
    if (m->is_string()) {
      out.material = m->as_string();
    } else if (m->is_object()) {
      out.material = str_or(get(*m, "base"), "PLA");
      for (const auto& [k, v] : m->as_object())
        if (k != "base" && k != "key" && v.is_number()) out.overrides.emplace_back(k, v.as_double());
    }
  }
  if (!material::find_material(out.material)) {
    if (warnings) warnings->push_back("Материала «" + out.material + "» нет в базе — взят PLA.");
    out.material = "PLA";
  }
  out.target_sf = num_or(get(job, "target_sf"), 2.0);
  if (const Value* fx = get(job, "fixtures"); fx && fx->is_array()) {
    for (std::size_t i = 0; i < fx->size(); ++i) {
      const Value& f = fx->at(i);
      UiFixture u;
      u.id = out.new_id();
      u.name = str_or(get(f, "name"), "Закрепление");
      const std::string label = "Закрепление " + std::to_string(i + 1);
      const Value* w = get(f, "where");
      if (!w) throw analysis::SelectionError(label + ": не задана область");
      u.faces = resolve_faces(*w, model, origin, label);
      if (const Value* c = get(f, "components"); c && c->is_string()) u.components = c->as_string();
      out.fixtures.push_back(std::move(u));
    }
  }
  if (const Value* cs = get(job, "cases"); cs && cs->is_array()) {
    for (std::size_t ci = 0; ci < cs->size(); ++ci) {
      const Value& c = cs->at(ci);
      UiCase uc;
      uc.id = out.new_id();
      uc.name = str_or(get(c, "name"), "Случай " + std::to_string(ci + 1));
      uc.duration = str_or(get(c, "duration"), "short");
      uc.cycles = num_or(get(c, "cycles"), 1e5);
      if (const Value* t = c.find("temperature")) {
        uc.temperature = t->is_number() ? std::optional<double>(t->as_double()) : std::nullopt;
      }
      if (const Value* th = c.find("thermal_expansion")) uc.thermal = th->truthy();
      if (c.find("fixtures") && warnings)
        warnings->push_back("«" + uc.name + "»: у случая свои закрепления — в окне используются общие.");
      if (const Value* ls = get(c, "loads"); ls && ls->is_array()) {
        for (std::size_t li = 0; li < ls->size(); ++li) {
          const Value& l = ls->at(li);
          std::string t = str_or(get(l, "type"), "force");
          for (char& ch : t) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          if (t == "torque") t = "moment";
          if (t == "acceleration" || t == "weight" || t == "self_weight") t = "gravity";
          if (t == "bolt" || t == "pin") t = "bearing";
          if (t == "weight_kg" || t == "hanging") t = "mass";
          if (t == "enforced" || t == "displace") t = "displacement";
          bool known = false;
          for (const auto& lt : load_types()) known = known || t == lt.key;
          if (!known) {
            if (warnings) warnings->push_back("«" + uc.name + "»: неизвестный тип нагрузки «" + t + "» пропущен.");
            continue;
          }
          UiLoad d = default_load(t, out.new_id());
          const std::string label = uc.name + ": нагрузка " + std::to_string(li + 1);
          if (const Value* w = get(l, "where"); w && load_type(t).faces) d.faces = resolve_faces(*w, model, origin, label);
          if (const Value* v = get(l, "vector"); v && (t == "force" || t == "bearing"))
            if (auto vv = vec3_of(*v)) {
              const auto r = vec_to_dir(*vv);
              d.value = round_to(r.mag, 4);
              d.dir = r.dir;
              d.vec = r.vec;
            }
          if (const Value* v = get(l, "value"); v && v->is_number() && t != "moment") d.value = v->as_double();
          if (const Value* dir = get(l, "direction")) {
            if (auto vv = vec3_of(*dir)) {
              const auto r = vec_to_dir(*vv);
              d.dir = r.dir;
              d.vec = r.vec;
            } else if (dir->is_string()) {
              d.dir = dir->as_string();
            }
          }
          if (t == "mass" || t == "impact") d.value = num_or(get(l, "kg"), num_or(get(l, "mass"), d.value));
          if (t == "impact") d.height = num_or(get(l, "height"), 0.0);
          if (t == "moment") {
            if (const Value* ax = get(l, "axis")) {
              if (auto vv = vec3_of(*ax)) {
                const auto r = vec_to_dir(*vv);
                d.axis = r.dir;
                d.vec = r.vec;
              } else if (ax->is_string()) {
                d.axis = ax->as_string();
              }
              d.value = num_or(get(l, "value"), d.value);
            } else if (const Value* v = get(l, "vector")) {
              if (auto vv = vec3_of(*v)) {
                const auto r = vec_to_dir(*vv);
                d.axis = r.dir;
                d.vec = r.vec;
                d.value = r.mag;
              }
            }
          }
          if (t == "displacement") {
            d.disp = {};
            if (const Value* v = get(l, "vector"); v && v->is_array() && v->size() == 3)
              for (std::size_t a = 0; a < 3; ++a)
                if (v->at(a).is_number()) d.disp[a] = v->at(a).as_double();
          }
          if (t == "gravity") {
            Vec3 g{0, 0, -1};
            const Value* gv = get(l, "g");
            if (!gv) gv = get(l, "vector");
            if (gv && gv->is_number()) g = {0, 0, -gv->as_double()};
            else if (gv)
              if (auto vv = vec3_of(*gv)) g = *vv;
            const auto r = vec_to_dir(g);
            d.value = round_to(r.mag, 3);
            d.dir = r.dir;
            d.vec = r.vec;
          }
          uc.loads.push_back(std::move(d));
        }
      }
      out.cases.push_back(std::move(uc));
    }
  }
  if (out.cases.empty()) {
    UiCase c;
    c.id = out.new_id();
    c.name = "Основная нагрузка";
    out.cases.push_back(std::move(c));
  }
  return out;
}

void remap_faces(UiJob& job, const Scene& from, const Scene& to) {
  for (auto& f : job.fixtures) f.faces = to.from_points(from.to_points(f.faces));
  for (auto& c : job.cases)
    for (auto& l : c.loads) l.faces = to.from_points(from.to_points(l.faces));
}

void rotate_job(UiJob& job, const Scene& from, const Scene& to, const std::array<double, 9>& r) {
  auto mul = [&](const Vec3& v) {
    return Vec3{r[0] * v[0] + r[1] * v[1] + r[2] * v[2], r[3] * v[0] + r[4] * v[1] + r[5] * v[2],
                r[6] * v[0] + r[7] * v[1] + r[8] * v[2]};
  };
  // ось и знак, куда переходит ось a; пусто — поворот не кратен 90°
  auto axis_of = [](const Vec3& v) -> std::optional<std::pair<int, double>> {
    for (int a = 0; a < 3; ++a)
      if (std::abs(v[static_cast<std::size_t>(a)]) > 0.999) return std::pair{a, v[static_cast<std::size_t>(a)] > 0 ? 1.0 : -1.0};
    return std::nullopt;
  };
  std::array<std::optional<std::pair<int, double>>, 3> image;
  for (int a = 0; a < 3; ++a) {
    Vec3 e{0, 0, 0};
    e[static_cast<std::size_t>(a)] = 1;
    image[static_cast<std::size_t>(a)] = axis_of(mul(e));
  }
  // система детали — от минимального угла габарита: после поворота угол другой
  const Vec3 s = from.size();
  Vec3 lo{1e300, 1e300, 1e300};
  for (int c = 0; c < 8; ++c) {
    const Vec3 q = mul({(c & 1) ? s[0] : 0.0, (c & 2) ? s[1] : 0.0, (c & 4) ? s[2] : 0.0});
    for (std::size_t a = 0; a < 3; ++a) lo[a] = std::min(lo[a], q[a]);
  }
  auto move = [&](const std::vector<std::int64_t>& keys) {
    std::vector<FacePoint> pts;
    for (const auto& fp : from.to_points(keys)) {
      Vec3 n{0, 0, 0};
      n[static_cast<std::size_t>(fp.dir >> 1)] = (fp.dir & 1) ? 1.0 : -1.0;
      const auto ax = axis_of(mul(n));
      if (!ax) continue;
      const Vec3 q = mul(fp.p);
      pts.push_back({{q[0] - lo[0], q[1] - lo[1], q[2] - lo[2]}, ax->first * 2 + (ax->second > 0 ? 1 : 0)});
    }
    return to.from_points(pts);
  };
  // направление "+x"… поворачивается в другую ось; не по оси — в свой вектор
  auto turn_key = [&](std::string& key, Vec3& vec) {
    if (key.size() != 2 || (key[0] != '+' && key[0] != '-') || key[1] < 'x' || key[1] > 'z') return;  // normal_in…
    Vec3 v{0, 0, 0};
    v[static_cast<std::size_t>(key[1] - 'x')] = key[0] == '+' ? 1.0 : -1.0;
    const Vec3 w = mul(v);
    if (const auto ax = axis_of(w)) {
      key = std::string(ax->second > 0 ? "+" : "-") + static_cast<char>('x' + ax->first);
    } else {
      key = "custom";
      vec = w;
    }
  };
  for (auto& f : job.fixtures) {
    f.faces = move(f.faces);
    std::string comps;
    for (char c : f.components)
      if (c >= 'x' && c <= 'z')
        if (const auto& im = image[static_cast<std::size_t>(c - 'x')]) comps += static_cast<char>('x' + im->first);
    std::sort(comps.begin(), comps.end());
    if (!comps.empty()) f.components = comps;
  }
  for (auto& c : job.cases)
    for (auto& l : c.loads) {
      l.faces = move(l.faces);
      l.vec = mul(l.vec);  // свой вектор — и направления силы, и оси момента
      turn_key(l.dir, l.vec);
      turn_key(l.axis, l.vec);
      std::array<std::optional<double>, 3> disp{};
      for (std::size_t a = 0; a < 3; ++a)
        if (l.disp[a]) {
          if (const auto& im = image[a]) disp[static_cast<std::size_t>(im->first)] = *l.disp[a] * im->second;
        }
      l.disp = disp;
    }
}

}  // namespace kika::app
