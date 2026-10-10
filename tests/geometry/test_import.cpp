// Модели: сетка, STL (двоичный и текстовый), 3MF (единицы, преобразования, составные объекты,
// объекты в отдельных файлах архива), G-code внутри 3MF.

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/geometry/import.hpp"
#include "kika/geometry/mesh.hpp"
#include "kika/geometry/zip.hpp"

using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;
using namespace kika::geometry;

namespace {

// Куб со стороной a, угол в начале координат, нормали наружу.
TriangleMesh cube(double a) {
  TriangleMesh m;
  for (int i = 0; i < 8; ++i) m.vertices.push_back({(i & 1) ? a : 0.0, (i & 2) ? a : 0.0, (i & 4) ? a : 0.0});
  m.triangles = {{0, 2, 1}, {1, 2, 3}, {4, 5, 6}, {5, 7, 6}, {0, 1, 4}, {1, 5, 4},
                 {2, 6, 3}, {3, 6, 7}, {0, 4, 2}, {2, 4, 6}, {1, 3, 5}, {3, 7, 5}};
  return m;
}

void put_u16(std::string& s, unsigned v) {
  s.push_back(static_cast<char>(v & 0xFF));
  s.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_u32(std::string& s, std::uint32_t v) {
  put_u16(s, v & 0xFFFF);
  put_u16(s, v >> 16);
}
void put_f32(std::string& s, float f) {
  std::uint32_t u;
  std::memcpy(&u, &f, 4);
  put_u32(s, u);
}

std::string binary_stl(const TriangleMesh& m, const std::string& header = "binary") {
  std::string s = header;
  s.resize(80, ' ');
  put_u32(s, static_cast<std::uint32_t>(m.triangles.size()));
  for (const auto& t : m.triangles) {
    for (int k = 0; k < 3; ++k) put_f32(s, 0.0f);
    for (auto v : t)
      for (double x : m.vertices[v]) put_f32(s, static_cast<float>(x));
    put_u16(s, 0);
  }
  return s;
}

std::string ascii_stl(const TriangleMesh& m) {
  std::string s = "solid куб\n";
  for (const auto& t : m.triangles) {
    s += "  facet normal 0 0 0\n    outer loop\n";
    for (auto v : t) {
      const auto& p = m.vertices[v];
      s += "      vertex " + std::to_string(p[0]) + " " + std::to_string(p[1]) + " " + std::to_string(p[2]) + "\n";
    }
    s += "    endloop\n  endfacet\n";
  }
  return s + "endsolid куб\n";
}

// ZIP из файлов: packed — сжимать deflate (иначе хранить как есть).
std::string make_zip(const std::vector<std::pair<std::string, std::string>>& files, bool packed) {
  std::string out, cd;
  for (const auto& [name, data] : files) {
    std::string body = data;
    if (packed) {
      z_stream zs{};
      REQUIRE(deflateInit2(&zs, 6, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) == Z_OK);
      body.resize(deflateBound(&zs, static_cast<uLong>(data.size())));
      zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
      zs.avail_in = static_cast<uInt>(data.size());
      zs.next_out = reinterpret_cast<Bytef*>(body.data());
      zs.avail_out = static_cast<uInt>(body.size());
      REQUIRE(deflate(&zs, Z_FINISH) == Z_STREAM_END);
      body.resize(zs.total_out);
      deflateEnd(&zs);
    }
    const auto crc = static_cast<std::uint32_t>(crc32(0, reinterpret_cast<const Bytef*>(data.data()), static_cast<uInt>(data.size())));
    const auto off = static_cast<std::uint32_t>(out.size());
    put_u32(out, 0x04034b50u);
    put_u16(out, 20);
    put_u16(out, 0);
    put_u16(out, packed ? 8 : 0);
    put_u32(out, 0);
    put_u32(out, crc);
    put_u32(out, static_cast<std::uint32_t>(body.size()));
    put_u32(out, static_cast<std::uint32_t>(data.size()));
    put_u16(out, static_cast<unsigned>(name.size()));
    put_u16(out, 0);
    out += name + body;
    put_u32(cd, 0x02014b50u);
    put_u16(cd, 20);
    put_u16(cd, 20);
    put_u16(cd, 0);
    put_u16(cd, packed ? 8 : 0);
    put_u32(cd, 0);
    put_u32(cd, crc);
    put_u32(cd, static_cast<std::uint32_t>(body.size()));
    put_u32(cd, static_cast<std::uint32_t>(data.size()));
    put_u16(cd, static_cast<unsigned>(name.size()));
    put_u16(cd, 0);
    put_u16(cd, 0);
    put_u16(cd, 0);
    put_u16(cd, 0);
    put_u32(cd, 0);
    put_u32(cd, off);
    cd += name;
  }
  const auto cd_off = static_cast<std::uint32_t>(out.size());
  out += cd;
  put_u32(out, 0x06054b50u);
  put_u16(out, 0);
  put_u16(out, 0);
  put_u16(out, static_cast<unsigned>(files.size()));
  put_u16(out, static_cast<unsigned>(files.size()));
  put_u32(out, static_cast<std::uint32_t>(cd.size()));
  put_u32(out, cd_off);
  put_u16(out, 0);
  return out;
}

std::string mesh_xml(const TriangleMesh& m) {
  std::string s = "<mesh><vertices>";
  for (const auto& v : m.vertices)
    s += "<vertex x=\"" + std::to_string(v[0]) + "\" y=\"" + std::to_string(v[1]) + "\" z=\"" + std::to_string(v[2]) + "\"/>";
  s += "</vertices><triangles>";
  for (const auto& t : m.triangles)
    s += "<triangle v1=\"" + std::to_string(t[0]) + "\" v2=\"" + std::to_string(t[1]) + "\" v3=\"" + std::to_string(t[2]) + "\"/>";
  return s + "</triangles></mesh>";
}

const char* kRels =
    R"(<?xml version="1.0" encoding="UTF-8"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
 <Relationship Target="/3D/3dmodel.model" Id="rel0" Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/>
</Relationships>)";

}  // namespace

TEST_CASE("Сетка: объём, площадь, замкнутость, преобразования", "[geometry]") {
  auto m = cube(10);
  CHECK_THAT(m.volume(), WithinRel(1000.0, 1e-12));
  CHECK_THAT(m.area(), WithinRel(600.0, 1e-12));
  const auto c = check(m);
  CHECK(c.closed());
  CHECK(c.shells == 1);
  // поворот на 90° точный и объём не меняет
  auto r = m;
  r.transform(Transform::rotation(0, 90));
  CHECK_THAT(r.volume(), WithinRel(1000.0, 1e-12));
  const auto b = r.bbox();
  CHECK(b.min[1] == -10.0);
  CHECK(b.max[2] == 10.0);
  // зеркало выворачивает обход — объём остаётся положительным
  Transform mirror;
  mirror.r = {-1, 0, 0, 0, 1, 0, 0, 0, 1};
  TriangleMesh mm;
  mm.append(m, mirror);
  CHECK_THAT(mm.volume(), WithinRel(1000.0, 1e-12));
  // постановка на стол
  const auto t = place_on_bed(r, 100, 50);
  r.transform(t);
  CHECK_THAT(r.bbox().center()[0], WithinAbs(100, 1e-12));
  CHECK_THAT(r.bbox().center()[1], WithinAbs(50, 1e-12));
  CHECK(r.bbox().min[2] == 0.0);
  // дыра
  auto open = m;
  open.triangles.pop_back();
  CHECK(check(open).open_edges == 3);
  // два куска
  TriangleMesh two;
  two.append(m);
  two.append(m, Transform::translation({20, 0, 0}));
  CHECK(check(two).shells == 2);
}

TEST_CASE("STL: двоичный и текстовый, слияние вершин, перевёрнутые нормали", "[geometry]") {
  const auto m = cube(20);
  for (const std::string& data : {binary_stl(m), binary_stl(m, "solid this header starts like ASCII"), ascii_stl(m)}) {
    auto im = parse_stl(data);
    CHECK(im.format == "STL");
    CHECK(im.mesh.triangles.size() == 12);
    check_and_fix(im);
    CHECK(im.mesh.vertices.size() == 8);
    CHECK(check(im.mesh).closed());
    CHECK_THAT(im.mesh.volume(), WithinRel(8000.0, 1e-6));
    CHECK(im.warnings.empty());
  }
  auto inv = m;
  inv.flip();
  auto im = parse_stl(binary_stl(inv));
  check_and_fix(im);
  CHECK_THAT(im.mesh.volume(), WithinRel(8000.0, 1e-6));
  REQUIRE(im.warnings.size() == 1);
  CHECK(im.warnings[0].find("внутрь") != std::string::npos);
  CHECK(parse_stl(ascii_stl(m)).name == "куб");
  CHECK_THROWS_AS(parse_stl("просто текст"), ImportError);
}

TEST_CASE("3MF: единицы, преобразование, сжатие", "[geometry]") {
  const std::string model = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="centimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <!-- куб 2 см -->
 <resources>
  <object id="1" name="Деталь &amp; крышка" type="model">)" + mesh_xml(cube(2)) + R"(</object>
 </resources>
 <build><item objectid="1" transform="1 0 0 0 1 0 0 0 1 5 6 7"/></build>
</model>)";
  for (bool packed : {false, true}) {
    const auto zip = make_zip({{"[Content_Types].xml", "<Types/>"}, {"_rels/.rels", kRels}, {"3D/3dmodel.model", model}}, packed);
    auto im = parse_3mf(zip);
    CHECK(im.format == "3MF");
    CHECK(im.name == "Деталь & крышка");
    CHECK(im.objects == 1);
    CHECK_THAT(im.mesh.volume(), WithinRel(8000.0, 1e-9));
    const auto b = im.mesh.bbox();
    CHECK_THAT(b.min[0], WithinAbs(50, 1e-9));
    CHECK_THAT(b.max[2], WithinAbs(70 + 20, 1e-9));
  }
}

TEST_CASE("3MF: составной объект в отдельных файлах (Orca, Bambu Studio)", "[geometry]") {
  const std::string root = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
       xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p">
 <resources>
  <object id="3" p:UUID="00000001-61cb-4c03-9d28-80fed5dfa1dc" type="model">
   <components>
    <component p:path="/3D/Objects/object_1.model" objectid="1" transform="1 0 0 0 1 0 0 0 1 0 0 0"/>
    <component p:path="/3D/Objects/object_1.model" objectid="2" transform="0 1 0 -1 0 0 0 0 1 30 0 0"/>
   </components>
  </object>
  <object id="9" type="support">)" + mesh_xml(cube(1)) + R"(</object>
 </resources>
 <build>
  <item objectid="3" transform="1 0 0 0 1 0 0 0 1 100 100 0" printable="1"/>
  <item objectid="9"/>
  <item objectid="3" printable="0"/>
 </build>
</model>)";
  const std::string objects = R"(<?xml version="1.0" encoding="UTF-8"?>
<model unit="millimeter" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">
 <resources>
  <object id="1" type="model">)" + mesh_xml(cube(10)) + R"(</object>
  <object id="2" type="model">)" + mesh_xml(cube(5)) + R"(</object>
 </resources>
</model>)";
  const auto zip = make_zip({{"_rels/.rels", kRels}, {"3D/3dmodel.model", root}, {"3D/Objects/object_1.model", objects}}, true);
  auto im = parse_3mf(zip);
  CHECK(im.objects == 1);  // составной объект; поддержка и непечатаемый элемент пропущены
  CHECK_THAT(im.mesh.volume(), WithinRel(1000.0 + 125.0, 1e-9));
  const auto b = im.mesh.bbox();
  CHECK_THAT(b.min[0], WithinAbs(100, 1e-9));
  // второй куб повёрнут на 90° вокруг Z и сдвинут на 30 по X: занимает x 125…130
  CHECK_THAT(b.max[0], WithinAbs(130, 1e-9));
  CHECK(std::find_if(im.warnings.begin(), im.warnings.end(),
                     [](const std::string& w) { return w.find("вспомогательных") != std::string::npos; }) != im.warnings.end());
}

TEST_CASE("G-code внутри 3MF", "[geometry]") {
  const std::string g = ";LAYER_CHANGE\nG1 X1 Y1 E1\n";
  const auto zip = make_zip({{"Metadata/plate_1.gcode", g}, {"Metadata/plate_1.gcode.md5", "x"}}, true);
  REQUIRE(gcode_from_3mf(zip));
  CHECK(*gcode_from_3mf(zip) == g);
  CHECK_FALSE(gcode_from_3mf("не архив"));
  ZipArchive a(zip);
  CHECK(a.entries().size() == 2);
  CHECK(a.find("/metadata/PLATE_1.gcode") != nullptr);
}
