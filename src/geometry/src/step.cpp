// Чтение STEP через OpenCascade: тела из файла → триангуляция граней (BRepMesh) → одна сетка.
// Собирается только с KIKA_WITH_OCCT. OpenCascade (LGPL-2.1 с исключением) подключается
// динамически — библиотеки TK*.dll/.so лежат рядом с программой отдельными файлами.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>

#include <BRepBndLib.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRep_Tool.hxx>
#include <Bnd_Box.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_PrinterOStream.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <Standard_Failure.hxx>
#include <TopAbs_Orientation.hxx>
#include <TopAbs_ShapeEnum.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Face.hxx>
#include <TopoDS_Shape.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>

#include "kika/geometry/import.hpp"

namespace kika::geometry {

namespace {

// Текст ошибки OpenCascade: в 8.0 — what() (как у std::exception), в 7.x — GetMessageString().
template <class Failure>
const char* failure_text(const Failure& e) {
  if constexpr (requires { e.what(); })
    return e.what();
  else
    return e.GetMessageString();
}

std::size_t count(const TopoDS_Shape& s, TopAbs_ShapeEnum type) {
  std::size_t n = 0;
  for (TopExp_Explorer ex(s, type); ex.More(); ex.Next()) ++n;
  return n;
}

}  // namespace

// Точность сетки — как у экспорта STL «высокого качества» в CAD: отклонение хорды от поверхности
// не больше 0,01 мм и угол между соседними треугольниками не больше 0,2 рад (≈ 11°). Для печати
// соплом 0,4 мм этого с запасом хватает; отверстие ⌀5 получается 32-угольником.
constexpr double kChordal = 0.01;  // мм
constexpr double kAngular = 0.2;   // рад

ImportedModel load_step(const std::filesystem::path& path) {
  // OpenCascade пишет сообщения разбора прямо в stdout (цветом) — в консоли kika и в окне они
  // лишние: ошибки чтения мы сообщаем сами
  static std::once_flag quiet;
  std::call_once(quiet, [] { Message::DefaultMessenger()->RemovePrinters(STANDARD_TYPE(Message_PrinterOStream)); });
  ImportedModel m;
  m.format = "STEP";
  try {
    STEPControl_Reader reader;
    // OpenCascade принимает путь в UTF-8 (на Windows сам переводит в UTF-16)
    const auto u8 = path.u8string();
    const std::string p(u8.begin(), u8.end());
    if (reader.ReadFile(p.c_str()) != IFSelect_RetDone)
      throw ImportError("Не удалось прочитать STEP: файл повреждён или это не STEP.");
    reader.TransferRoots();
    const TopoDS_Shape shape = reader.OneShape();
    if (shape.IsNull() || count(shape, TopAbs_FACE) == 0) throw ImportError("В файле STEP нет тел и поверхностей.");

    // единицы: OpenCascade переводит длины файла в мм (единица по умолчанию)
    BRepMesh_IncrementalMesh mesher(shape, kChordal, false, kAngular, true);
    std::size_t faces = 0, failed = 0;
    for (TopExp_Explorer ex(shape, TopAbs_FACE); ex.More(); ex.Next()) {
      const TopoDS_Face& face = TopoDS::Face(ex.Current());
      TopLoc_Location loc;
      const auto tri = BRep_Tool::Triangulation(face, loc);
      ++faces;
      if (tri.IsNull() || tri->NbTriangles() == 0) {
        ++failed;
        continue;
      }
      const gp_Trsf tr = loc.Transformation();
      // грань, повёрнутая к телу обратной стороной: обход треугольников меняется
      const bool reversed = face.Orientation() == TopAbs_REVERSED;
      const auto base = static_cast<std::uint32_t>(m.mesh.vertices.size());
      for (int i = 1; i <= tri->NbNodes(); ++i) {
        const gp_Pnt q = tri->Node(i).Transformed(tr);
        m.mesh.vertices.push_back({q.X(), q.Y(), q.Z()});
      }
      for (int i = 1; i <= tri->NbTriangles(); ++i) {
        int a = 0, b = 0, c = 0;
        tri->Triangle(i).Get(a, b, c);
        if (reversed) std::swap(b, c);
        m.mesh.triangles.push_back({base + static_cast<std::uint32_t>(a - 1), base + static_cast<std::uint32_t>(b - 1),
                                    base + static_cast<std::uint32_t>(c - 1)});
      }
    }
    const std::size_t solids = count(shape, TopAbs_SOLID);
    m.objects = solids > 0 ? solids : count(shape, TopAbs_SHELL);
    if (failed > 0)
      m.warnings.push_back(std::format("{} из {} граней не удалось разбить на треугольники — на их месте будут дыры.",
                                       failed, faces));
    if (solids == 0)
      m.warnings.push_back("В файле STEP нет твёрдых тел, только поверхности: проверьте, что модель замкнута.");
  } catch (const Standard_Failure& e) {
    const char* msg = failure_text(e);
    throw ImportError(std::string("Ошибка OpenCascade при чтении STEP") + (msg && *msg ? std::string(": ") + msg : "") +
                      ".");
  }
  if (m.mesh.triangles.empty()) throw ImportError("В файле STEP нет поверхностей, которые можно разбить на треугольники.");
  return m;
}

}  // namespace kika::geometry
