// Чтение 3MF: ZIP с XML-моделью (3D/3dmodel.model). Поддерживаются единицы модели, составные
// объекты (components) с преобразованиями и расширение Production — объекты в отдельных файлах
// архива, как в проектах Orca Slicer и Bambu Studio.

#include <charconv>
#include <map>
#include <memory>

#include "kika/geometry/import.hpp"
#include "kika/geometry/zip.hpp"
#include "xml.hpp"

namespace kika::geometry {

namespace {

struct Component {
  std::string path;  // пусто — тот же файл
  int object = 0;
  Transform tr;
};

struct Object {
  std::string name;
  std::string type = "model";
  TriangleMesh mesh;
  std::vector<Component> components;
};

struct ModelFile {
  double unit = 1.0;  // мм в единице файла
  std::map<int, Object> objects;
  struct Item {
    int object = 0;
    Transform tr;
  };
  std::vector<Item> items;
};

double parse_double(std::string_view s, const char* what) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  if (!s.empty() && s.front() == '+') s.remove_prefix(1);
  double v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc()) throw ImportError(std::string("3MF: не число в ") + what + ": «" + std::string(s.substr(0, 30)) + "»");
  return v;
}

int parse_int(std::string_view s, const char* what) {
  int v = 0;
  const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
  if (r.ec != std::errc()) throw ImportError(std::string("3MF: не целое число в ") + what);
  return v;
}

// Матрица 3MF «m00 m01 m02 m10 m11 m12 m20 m21 m22 m30 m31 m32»: точка — строка, x' = x·m00 + y·m10 + z·m20 + m30.
Transform parse_transform(std::string_view s, double unit) {
  double m[12];
  int n = 0;
  std::size_t p = 0;
  while (n < 12 && p < s.size()) {
    while (p < s.size() && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) ++p;
    const std::size_t b = p;
    while (p < s.size() && s[p] != ' ' && s[p] != '\t' && s[p] != '\n' && s[p] != '\r') ++p;
    if (p > b) m[n++] = parse_double(s.substr(b, p - b), "transform");
  }
  if (n != 12) throw ImportError("3MF: преобразование должно содержать 12 чисел");
  Transform t;
  t.r = {m[0], m[3], m[6], m[1], m[4], m[7], m[2], m[5], m[8]};
  t.t = {m[9] * unit, m[10] * unit, m[11] * unit};
  return t;
}

double unit_scale(const std::string& u) {
  if (u.empty() || u == "millimeter") return 1.0;
  if (u == "micron") return 0.001;
  if (u == "centimeter") return 10.0;
  if (u == "inch") return 25.4;
  if (u == "foot") return 304.8;
  if (u == "meter") return 1000.0;
  throw ImportError("3MF: неизвестные единицы «" + u + "»");
}

ModelFile parse_model(std::string_view text) {
  ModelFile f;
  xml::Reader r(text);
  Object* obj = nullptr;
  int cur_id = 0;
  std::string a, b, c;
  bool in_build = false;
  try {
    for (auto ev = r.next(); ev != xml::Reader::Event::Eof; ev = r.next()) {
      const std::string_view name = r.name();
      if (ev == xml::Reader::Event::End) {
        if (name == "object") obj = nullptr;
        if (name == "build") in_build = false;
        continue;
      }
      if (name == "model") {
        if (r.attr("unit", a)) f.unit = unit_scale(a);
      } else if (name == "object") {
        if (!r.attr("id", a)) throw ImportError("3MF: объект без id");
        cur_id = parse_int(a, "id объекта");
        obj = &f.objects[cur_id];
        if (r.attr("name", a)) obj->name = a;
        if (r.attr("type", a)) obj->type = a;
      } else if (name == "vertex" && obj) {
        // самое частое место: значения атрибутов разбираем без копирования
        double v[3] = {0, 0, 0};
        int got = 0;
        for (const auto& at : r.attributes()) {
          const auto ln = xml::Reader::local(at.name);
          if (ln.size() != 1) continue;
          const int k = ln[0] - 'x';
          if (k < 0 || k > 2) continue;
          v[k] = parse_double(at.value, "координатах вершины") * f.unit;
          ++got;
        }
        if (got != 3) throw ImportError("3MF: у вершины нет одной из координат x, y, z");
        obj->mesh.vertices.push_back({v[0], v[1], v[2]});
      } else if (name == "triangle" && obj) {
        std::uint32_t v[3] = {0, 0, 0};
        int got = 0;
        for (const auto& at : r.attributes()) {
          const auto ln = xml::Reader::local(at.name);
          if (ln.size() != 2 || ln[0] != 'v' || ln[1] < '1' || ln[1] > '3') continue;
          v[ln[1] - '1'] = static_cast<std::uint32_t>(parse_int(at.value, "номерах вершин треугольника"));
          ++got;
        }
        if (got != 3) throw ImportError("3MF: у треугольника нет одной из вершин v1, v2, v3");
        obj->mesh.triangles.push_back({v[0], v[1], v[2]});
      } else if (name == "component" && obj) {
        Component comp;
        if (!r.attr("objectid", a)) throw ImportError("3MF: компонент без objectid");
        comp.object = parse_int(a, "objectid");
        if (r.attr("transform", b)) comp.tr = parse_transform(b, f.unit);
        if (r.attr("path", c)) comp.path = c;
        obj->components.push_back(std::move(comp));
      } else if (name == "build") {
        in_build = true;
      } else if (name == "item" && in_build) {
        if (r.attr("printable", a) && a == "0") continue;
        ModelFile::Item it;
        if (!r.attr("objectid", a)) throw ImportError("3MF: элемент сборки без objectid");
        it.object = parse_int(a, "objectid");
        if (r.attr("transform", b)) it.tr = parse_transform(b, f.unit);
        f.items.push_back(it);
      }
    }
  } catch (const xml::Error& e) {
    throw ImportError(std::string("3MF: ") + e.what());
  }
  return f;
}

class Assembler {
 public:
  explicit Assembler(const ZipArchive& zip) : zip_(zip) {}

  const ModelFile& file(const std::string& path) {
    auto it = files_.find(path);
    if (it != files_.end()) return *it->second;
    const auto text = zip_.read(path);
    if (!text) throw ImportError("3MF: в архиве нет файла модели «" + path + "»");
    auto f = std::make_unique<ModelFile>(parse_model(*text));
    return *files_.emplace(path, std::move(f)).first->second;
  }

  void add(const std::string& path, int id, const Transform& tr, TriangleMesh& out, int depth, std::string* name) {
    if (depth > 32) throw ImportError("3MF: объекты ссылаются друг на друга по кругу");
    const ModelFile& f = file(path);
    const auto it = f.objects.find(id);
    if (it == f.objects.end()) throw ImportError("3MF: нет объекта с id " + std::to_string(id) + " в «" + path + "»");
    const Object& o = it->second;
    if (o.type != "model") {
      ++skipped_;
      return;
    }
    if (name && name->empty()) *name = o.name;
    if (!o.mesh.triangles.empty()) {
      const auto nv = static_cast<std::uint32_t>(o.mesh.vertices.size());
      for (const auto& t : o.mesh.triangles)
        if (t[0] >= nv || t[1] >= nv || t[2] >= nv)
          throw ImportError("3MF: треугольник ссылается на несуществующую вершину (объект «" + o.name + "»)");
      out.append(o.mesh, tr);
    }
    for (const auto& c : o.components) add(c.path.empty() ? path : c.path, c.object, tr * c.tr, out, depth + 1, name);
  }

  int skipped() const { return skipped_; }

 private:
  const ZipArchive& zip_;
  std::map<std::string, std::unique_ptr<ModelFile>> files_;
  int skipped_ = 0;
};

std::string root_model_path(const ZipArchive& zip) {
  if (const auto rels = zip.read("_rels/.rels")) {
    xml::Reader r(*rels);
    std::string type, target;
    try {
      for (auto ev = r.next(); ev != xml::Reader::Event::Eof; ev = r.next())
        if (ev == xml::Reader::Event::Start && r.name() == "Relationship" && r.attr("Type", type) &&
            type.size() >= 8 && type.compare(type.size() - 8, 8, "3dmodel") == 0 && r.attr("Target", target))
          return target;
    } catch (const xml::Error&) {
    }
  }
  return "3D/3dmodel.model";
}

}  // namespace

ImportedModel parse_3mf(std::string_view data) {
  if (!looks_like_zip(data)) throw ImportError("Файл не похож на 3MF (это должен быть ZIP-архив).");
  ImportedModel m;
  m.format = "3MF";
  try {
    const ZipArchive zip(data);
    Assembler as(zip);
    const std::string root = root_model_path(zip);
    const ModelFile& f = as.file(root);
    if (f.items.empty()) throw ImportError("В 3MF нет объектов для печати (раздел build пуст).");
    for (const auto& it : f.items) {
      const std::size_t before = m.mesh.triangles.size();
      as.add(root, it.object, it.tr, m.mesh, 0, &m.name);
      if (m.mesh.triangles.size() > before) ++m.objects;
    }
    if (as.skipped() > 0)
      m.warnings.push_back(std::to_string(as.skipped()) + " вспомогательных объектов (поддержки, модификаторы) пропущено.");
  } catch (const ZipError& e) {
    throw ImportError(std::string("3MF: ") + e.what());
  }
  if (m.mesh.empty()) throw ImportError("В 3MF нет треугольников.");
  if (m.objects > 1)
    m.warnings.push_back("В файле " + std::to_string(m.objects) +
                         " объектов — они нарезаются вместе, как стоят на столе в файле.");
  return m;
}

std::optional<std::string> gcode_from_3mf(std::string_view data, int plate) {
  if (!looks_like_zip(data)) return std::nullopt;
  try {
    const ZipArchive zip(data);
    if (auto g = zip.read("Metadata/plate_" + std::to_string(plate) + ".gcode")) return g;
    for (const auto& e : zip.entries())
      if (e.name.size() > 6 && e.name.compare(e.name.size() - 6, 6, ".gcode") == 0) return zip.read(e);
  } catch (const ZipError&) {
  }
  return std::nullopt;
}

}  // namespace kika::geometry
