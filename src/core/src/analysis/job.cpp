// Разбор задания на расчёт (как Analysis в fdmfea/analysis.py читает словарь задания).

#include "kika/analysis/job.hpp"

#include <cmath>
#include <limits>

#include "util/text.hpp"

namespace kika::analysis {

namespace {

using json::Value;

const Value* get(const Value& obj, std::string_view key) { return obj.find(key); }

// Значение по ключу, если оно есть и не null.
const Value* get_set(const Value& obj, std::string_view key) {
  const Value* v = obj.find(key);
  return (v && !v->is_null()) ? v : nullptr;
}

double num(const Value& v, const std::string& what) {
  if (v.is_number() || v.is_bool()) return v.as_double();
  if (v.is_string())
    if (auto d = util::parse_double(v.as_string())) return *d;
  throw JobError(what + ": ожидается число, а записано " + json::dump(v));
}

std::string str_of(const Value& v, const std::string& what) {
  if (v.is_string()) return v.as_string();
  if (v.is_number()) return json::dump(v);
  throw JobError(what + ": ожидается строка, а записано " + json::dump(v));
}

void flatten_numbers(const Value& v, std::vector<double>& out, const std::string& what) {
  if (v.is_array()) {
    for (const auto& x : v.as_array()) flatten_numbers(x, out, what);
  } else {
    out.push_back(num(v, what));
  }
}

Vec3 vec3(const Value& v, const std::string& name) {
  std::vector<double> a;
  flatten_numbers(v, a, name);
  if (a.size() != 3) throw SelectionError(name + " должен содержать 3 числа [x, y, z]");
  return {a[0], a[1], a[2]};
}

int axis_index(const std::string& s, const std::string& what) {
  const std::string t = util::to_lower(s);
  if (t == "x") return 0;
  if (t == "y") return 1;
  if (t == "z") return 2;
  throw SelectionError(what + ": неизвестная ось '" + s + "', допустимо x, y, z");
}

Box parse_box(const Value& v, const std::string& name) {
  if (v.is_object()) {
    const Value* lo = get(v, "min");
    const Value* hi = get(v, "max");
    if (!lo || !hi) throw SelectionError(name + ": у рамки нужны min и max");
    return {vec3(*lo, "min"), vec3(*hi, "max")};
  }
  std::vector<double> b;
  flatten_numbers(v, b, name);
  if (b.size() != 6) throw SelectionError(name + ": рамка задаётся как {\"min\": […], \"max\": […]} или шестью числами");
  return {{b[0], b[1], b[2]}, {b[3], b[4], b[5]}};
}

struct SideInfo {
  int axis;
  int sign;
};

std::optional<SideInfo> side_info(const std::string& side) {
  static const std::pair<const char*, SideInfo> sides[] = {{"xmin", {0, -1}}, {"xmax", {0, 1}},
                                                           {"ymin", {1, -1}}, {"ymax", {1, 1}},
                                                           {"zmin", {2, -1}}, {"zmax", {2, 1}}};
  for (const auto& [k, v] : sides)
    if (side == k) return v;
  return std::nullopt;
}

Vec3 parse_normal(const Value& v, const std::string& name) {
  if (v.is_string()) {
    std::string t = v.as_string();
    if (t == "-x") t = "xmin";
    if (t == "+x") t = "xmax";
    if (t == "-y") t = "ymin";
    if (t == "+y") t = "ymax";
    if (t == "-z") t = "zmin";
    if (t == "+z") t = "zmax";
    const auto si = side_info(t);
    if (!si) throw SelectionError(name + ": неизвестная нормаль '" + v.as_string() + "', допустимо +x, -x, +y, -y, +z, -z");
    Vec3 n{0, 0, 0};
    n[static_cast<std::size_t>(si->axis)] = si->sign;
    return n;
  }
  return vec3(v, "нормаль");
}

Selection parse_selection(const Value& sel, const std::string& name) {
  Selection s;
  if (sel.is_string()) {
    // строка — название стороны
    Value obj = Value::object();
    obj["side"] = sel;
    return parse_selection(obj, name);
  }
  if (!sel.is_object()) throw SelectionError(name + ": не понимаю описание области " + json::dump(sel));
  if (const Value* f = get(sel, "faces")) {
    s.kind = Selection::Kind::Faces;
    std::vector<double> keys;
    flatten_numbers(*f, keys, name + ": faces");
    for (double k : keys) s.faces.push_back(static_cast<std::int64_t>(k));
  } else if (const Value* sd = get(sel, "side")) {
    s.kind = Selection::Kind::Side;
    const std::string side = util::to_lower(str_of(*sd, name + ": side"));
    const auto si = side_info(side);
    if (!si)
      throw SelectionError(name + ": неизвестная сторона '" + side +
                           "', допустимо: xmin, xmax, ymin, ymax, zmin, zmax");
    s.axis = si->axis;
    s.sign = si->sign;
    if (const Value* d = get(sel, "depth")) s.depth = num(*d, name + ": depth");
    if (const Value* r = get(sel, "range"); r && r->truthy()) s.range_box = parse_box(*r, name);
  } else if (const Value* bx = get(sel, "box")) {
    s.kind = Selection::Kind::Box;
    s.box = parse_box(*bx, name);
    if (const Value* n = get(sel, "normal")) s.normal = parse_normal(*n, name);
  } else if (get(sel, "sphere") || get(sel, "brush")) {
    s.kind = Selection::Kind::Sphere;
    const Value* sp = get(sel, "sphere");
    const Value& d = (sp && sp->truthy()) ? *sp : *(get(sel, "brush") ? get(sel, "brush") : sp);
    const Value* c = get(d, "center");
    const Value* r = get(d, "radius");
    if (!c || !r) throw SelectionError(name + ": у шара нужны center и radius");
    s.center = vec3(*c, "центр");
    s.radius = num(*r, name + ": radius");
  } else if (get(sel, "cylinder") || get(sel, "hole")) {
    s.kind = Selection::Kind::Cylinder;
    const Value* cy = get(sel, "cylinder");
    const Value& d = (cy && cy->truthy()) ? *cy : *(get(sel, "hole") ? get(sel, "hole") : cy);
    const Value* ax = get(d, "axis");
    s.axis = ax ? axis_index(str_of(*ax, name + ": axis"), name) : 2;
    const Value* c = get(d, "center");
    const Value* r = get(d, "radius");
    if (!c || !r) throw SelectionError(name + ": у отверстия нужны center и radius");
    std::vector<double> cc;
    flatten_numbers(*c, cc, name + ": center");
    if (cc.size() == 2) {
      s.center2 = {cc[0], cc[1]};
    } else if (cc.size() == 3) {
      const std::size_t o0 = s.axis == 0 ? 1 : 0;
      const std::size_t o1 = s.axis == 2 ? 1 : 2;
      s.center2 = {cc[o0], cc[o1]};
    } else {
      throw SelectionError(name + ": центр отверстия — два числа (в плоскости поперёк оси) или три");
    }
    s.radius = num(*r, name + ": radius");
    if (const Value* o = get(d, "outer")) s.outer = o->truthy();
    if (const Value* rg = get(d, "range")) {
      std::vector<double> lr;
      flatten_numbers(*rg, lr, name + ": range");
      if (lr.size() != 2) throw SelectionError(name + ": range отверстия — два числа [от, до]");
      s.range = std::array<double, 2>{lr[0], lr[1]};
    }
  } else {
    throw SelectionError(name + ": не понимаю описание области " + json::dump(sel));
  }
  return s;
}

void parse_region_into(const Value& v, const std::string& name, Region& out) {
  if (v.is_array()) {
    for (const auto& x : v.as_array()) parse_region_into(x, name, out);
    return;
  }
  out.push_back(parse_selection(v, name));
}

Region parse_region(const Value* v, const std::string& name) {
  if (!v || v->is_null()) throw SelectionError(name + ": не задана область");
  Region r;
  parse_region_into(*v, name, r);
  if (r.empty()) throw SelectionError(name + ": не задана область");
  return r;
}

Direction parse_direction(const Value* v) {
  Direction d;
  if (!v || v->is_null()) return d;  // по умолчанию −z
  if (v->is_string()) {
    d.name = v->as_string();
    // проверяем сразу, чтобы ошибка была видна до расчёта
    const std::string t = util::to_lower_utf8(util::trim(d.name));
    if (t != "normal_in" && t != "inward" && t != "внутрь" && t != "normal_out" && t != "outward" &&
        t != "наружу")
      (void)resolve_direction(d);
    return d;
  }
  d.vec = vec3(*v, "направление");
  (void)resolve_direction(d);  // нулевое направление — ошибка сразу
  return d;
}

Fixture parse_fixture(const Value& fx, std::size_t k) {
  Fixture f;
  f.name = "Закрепление " + std::to_string(k + 1);
  if (!fx.is_object()) throw JobError(f.name + ": ожидается объект {\"where\": …}");
  if (const Value* n = get_set(fx, "name")) f.name = str_of(*n, f.name);
  f.where = parse_region(get_set(fx, "where"), "Закрепление " + std::to_string(k + 1));
  std::string comps = "xyz";
  if (const Value* c = get(fx, "components")) comps = util::to_lower(str_of(*c, f.name + ": components"));
  for (std::size_t a = 0; a < 3; ++a) f.components[a] = comps.find("xyz"[a]) != std::string::npos;
  return f;
}

Load parse_load(const Value& ld, std::size_t k) {
  Load l;
  if (!ld.is_object()) throw JobError("Нагрузка " + std::to_string(k + 1) + ": ожидается объект {\"type\": …}");
  if (const Value* t = get(ld, "type")) l.type_name = util::to_lower(str_of(*t, "Нагрузка " + std::to_string(k + 1)));
  l.label = "Нагрузка " + std::to_string(k + 1) + " (" + l.type_name + ")";
  const std::string& t = l.type_name;
  auto number = [&](std::initializer_list<const char*> keys, double def) {
    for (const char* key : keys)
      if (const Value* v = get(ld, key)) return num(*v, l.label + ": " + key);
    return def;
  };
  if (t == "gravity" || t == "acceleration" || t == "weight" || t == "self_weight") {
    l.type = LoadType::Gravity;
    const Value* gv = get(ld, "g");
    if (!gv) gv = get(ld, "vector");
    if (gv) {
      if (gv->is_number()) {
        l.g = {0.0, 0.0, -gv->as_double()};
      } else if (gv->is_string()) {
        Direction d;
        d.name = gv->as_string();
        l.g = resolve_direction(d);
      } else {
        l.g = vec3(*gv, l.label + ": g");
      }
    }
    return l;
  }
  // у остальных нагрузок есть область приложения
  l.where = parse_region(get_set(ld, "where"), l.label);
  if (t == "force" || t == "bearing" || t == "bolt" || t == "pin") {
    l.type = t == "force" ? LoadType::Force : LoadType::Bearing;
    if (const Value* v = get_set(ld, "vector")) l.vector = vec3(*v, "вектор");
    l.value = number({"value", "magnitude"}, 0.0);
    l.direction = parse_direction(get(ld, "direction"));
    if (l.type == LoadType::Force)
      if (const Value* p = get_set(ld, "point")) l.point = vec3(*p, "точка приложения");
  } else if (t == "mass" || t == "weight_kg" || t == "hanging") {
    l.type = LoadType::Mass;
    l.kg = number({"kg", "mass"}, 0.0);
    l.direction = parse_direction(get(ld, "direction"));
    if (const Value* p = get_set(ld, "point")) l.point = vec3(*p, "точка приложения");
  } else if (t == "pressure") {
    l.type = LoadType::Pressure;
    l.value = number({"value", "mpa"}, 0.0);
  } else if (t == "moment" || t == "torque") {
    l.type = LoadType::Moment;
    const Value* val = get_set(ld, "value");
    const Value* ax = get(ld, "axis");
    if (val && ax && ax->truthy()) {
      l.value = num(*val, l.label + ": value");
      Direction d = parse_direction(ax);
      if (!d.vec) {
        const std::string n = util::to_lower_utf8(util::trim(d.name));
        if (n == "normal_in" || n == "inward" || n == "внутрь" || n == "normal_out" || n == "outward" ||
            n == "наружу")
          throw SelectionError(l.label + ": ось момента — +x, -x, … или вектор [x, y, z]");
      }
      l.axis = d;
    } else {
      const Value* v = get(ld, "vector");
      l.vector = (v && v->truthy()) ? vec3(*v, "вектор") : Vec3{0, 0, 0};
    }
    if (const Value* c = get_set(ld, "center")) l.center = vec3(*c, "центр");
  } else if (t == "impact") {
    l.type = LoadType::Impact;
    l.kg = number({"kg", "mass"}, 0.0);
    l.height = number({"height"}, 0.0);
    l.direction = parse_direction(get(ld, "direction"));
  } else if (t == "displacement" || t == "enforced" || t == "displace") {
    l.type = LoadType::Displacement;
    const Value* v = get(ld, "vector");
    if (v) {
      if (!v->is_array() || v->size() != 3)
        throw JobError(l.label + ": перемещение задаётся тремя числами [x, y, z] (null — свободно)");
      for (std::size_t c = 0; c < 3; ++c) {
        const Value& x = v->at(c);
        if (!x.is_null()) l.displacement[c] = num(x, l.label);
      }
    }
  } else {
    throw JobError(l.label + ": неизвестный тип нагрузки '" + t + "'");
  }
  return l;
}

std::vector<Fixture> parse_fixtures(const Value* v) {
  std::vector<Fixture> out;
  if (!v || !v->truthy()) return out;
  if (!v->is_array()) throw JobError("fixtures: ожидается список закреплений [..]");
  for (std::size_t k = 0; k < v->size(); ++k) out.push_back(parse_fixture(v->at(k), k));
  return out;
}

}  // namespace

material::Material material_from_json(const json::Value& spec) {
  if (spec.is_string()) {
    const auto* m = material::find_material(spec.as_string());
    if (!m) {
      std::string names;
      for (const auto& x : material::builtin_materials()) names += (names.empty() ? "" : ", ") + x.key;
      throw JobError("Материал '" + spec.as_string() + "' не найден. Есть: " + names);
    }
    return *m;
  }
  if (!spec.is_object()) throw JobError("material: ожидается название из базы или объект со свойствами");
  constexpr double kUnset = std::numeric_limits<double>::quiet_NaN();
  material::Material m;
  std::string base;
  if (const Value* b = get_set(spec, "base"); b && b->truthy()) {
    base = str_of(*b, "material.base");
  } else if (const Value* k = get_set(spec, "key"); k && k->truthy()) {
    base = str_of(*k, "material.key");
  }
  if (const auto* bm = base.empty() ? nullptr : material::find_material(base)) {
    m = *bm;
  } else {
    // без базы: обязательные свойства должны быть заданы
    m.density = m.E1 = m.E2 = m.E3 = m.G12 = m.G13 = m.G23 = m.nu12 = m.nu13 = m.nu23 = kUnset;
    m.Xt = m.Xc = m.Yt = m.Yc = m.Zt = m.Zc = m.S12 = m.S13 = m.S23 = kUnset;
    m.key = base;
  }
  struct Field {
    const char* name;
    double material::Material::* ptr;
    bool required;
  };
  static const Field fields[] = {
      {"E1", &material::Material::E1, true},       {"E2", &material::Material::E2, true},
      {"E3", &material::Material::E3, true},       {"G12", &material::Material::G12, true},
      {"G13", &material::Material::G13, true},     {"G23", &material::Material::G23, true},
      {"nu12", &material::Material::nu12, true},   {"nu13", &material::Material::nu13, true},
      {"nu23", &material::Material::nu23, true},   {"Xt", &material::Material::Xt, true},
      {"Xc", &material::Material::Xc, true},       {"Yt", &material::Material::Yt, true},
      {"Yc", &material::Material::Yc, true},       {"Zt", &material::Material::Zt, true},
      {"Zc", &material::Material::Zc, true},       {"S12", &material::Material::S12, true},
      {"S13", &material::Material::S13, true},     {"S23", &material::Material::S23, true},
      {"density", &material::Material::density, true},
      {"cte", &material::Material::cte, false},    {"hdt", &material::Material::hdt, false},
      {"tg", &material::Material::tg, false},      {"creep_strength", &material::Material::creep_strength, false},
      {"creep_modulus", &material::Material::creep_modulus, false},
      {"fatigue_k", &material::Material::fatigue_k, false},
      {"elongation", &material::Material::elongation, false},
  };
  for (const auto& f : fields)
    if (const Value* v = get(spec, f.name)) m.*f.ptr = num(*v, std::string("material.") + f.name);
  if (const Value* v = get_set(spec, "key")) m.key = str_of(*v, "material.key");
  if (const Value* v = get_set(spec, "name")) m.name = str_of(*v, "material.name");
  if (const Value* v = get_set(spec, "note")) m.note = str_of(*v, "material.note");
  std::string missing;
  for (const auto& f : fields)
    if (f.required && std::isnan(m.*f.ptr)) missing += (missing.empty() ? "" : ", ") + std::string(f.name);
  if (!missing.empty()) throw JobError("В описании материала не хватает: " + missing);
  if (m.name.empty()) m.name = m.key.empty() ? "Свой материал" : m.key;
  return m;
}

Job parse_job(const json::Value& v) {
  if (!v.is_object()) throw JobError("Задание должно быть объектом JSON {…}");
  Job job;
  if (const Value* m = get(v, "material"); m && m->truthy()) job.material = material_from_json(*m);
  if (const Value* t = get_set(v, "target_sf")) job.target_sf = num(*t, "target_sf");
  if (const Value* c = get(v, "coords")) job.part_coords = c->is_string() && c->as_string() == "part";
  if (const Value* g = get_set(v, "gcode")) job.gcode = str_of(*g, "gcode");
  if (const Value* t = get_set(v, "title")) job.title = str_of(*t, "title");
  if (const Value* t = get_set(v, "subtitle")) job.subtitle = str_of(*t, "subtitle");
  if (const Value* x = get_set(v, "voxel"); x && x->truthy()) job.voxel = num(*x, "voxel");
  if (const Value* x = get_set(v, "max_elems"); x && x->truthy())
    job.max_elems = static_cast<long long>(num(*x, "max_elems"));

  const Value* cases = get(v, "cases");
  if (!cases || !cases->truthy()) throw JobError("В задании нет ни одного расчётного случая (cases).");
  if (!cases->is_array()) throw JobError("cases: ожидается список расчётных случаев [..]");
  const Value* job_fixtures = get(v, "fixtures");
  for (std::size_t ci = 0; ci < cases->size(); ++ci) {
    const Value& cv = cases->at(ci);
    if (!cv.is_object()) throw JobError("Случай " + std::to_string(ci + 1) + ": ожидается объект {…}");
    Case c;
    if (const Value* n = get(cv, "name")) {
      if (n->truthy()) c.name = str_of(*n, "name");
      if (!n->is_null()) c.name_in_job = str_of(*n, "name");
    }
    if (c.name.empty()) c.name = "Случай " + std::to_string(ci + 1);
    // свои закрепления случая заменяют общие
    const Value* fx = cv.contains("fixtures") ? get(cv, "fixtures") : job_fixtures;
    c.fixtures = parse_fixtures(fx);
    if (c.fixtures.empty()) throw JobError("«" + c.name_in_job + "»: не задано ни одного закрепления.");
    const Value* loads = get(cv, "loads");
    if (!loads || !loads->truthy()) throw JobError("«" + c.name_in_job + "»: не задано ни одной нагрузки.");
    if (!loads->is_array()) throw JobError("«" + c.name + "»: loads — ожидается список нагрузок [..]");
    for (std::size_t k = 0; k < loads->size(); ++k) c.loads.push_back(parse_load(loads->at(k), k));
    bool impact = false;
    for (const auto& l : c.loads) impact = impact || l.type == LoadType::Impact;
    if (impact && c.loads.size() > 1)
      throw JobError("Удар (impact) считается отдельным случаем: уберите из него другие нагрузки.");
    if (const Value* d = get_set(cv, "duration")) c.duration = util::to_lower(str_of(*d, "duration"));
    if (const Value* t = get_set(cv, "temperature")) c.temperature = num(*t, "temperature");
    if (const Value* n = get_set(cv, "cycles")) c.cycles = num(*n, "cycles");
    if (const Value* th = get(cv, "thermal_expansion")) c.thermal_expansion = th->truthy();
    job.cases.push_back(std::move(c));
  }
  return job;
}

}  // namespace kika::analysis
