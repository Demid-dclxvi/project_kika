// Перенос fdmfea/export.py: данные для 3D-просмотра и самодостаточный HTML-отчёт.

#include "kika/report/report.hpp"

#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <format>
#include <stdexcept>
#include <vector>

#include "kika/material/material.hpp"
#include "kika/version.hpp"

namespace kika::report {

namespace assets {
std::string_view get(std::string_view name);  // встроенные файлы (cmake/EmbedFiles.cmake)
}

namespace {

using analysis::py_round;
using json::Value;

std::string_view asset(std::string_view name) {
  const auto a = assets::get(name);
  if (a.empty()) throw std::logic_error("нет встроенного файла отчёта: " + std::string(name));
  return a;
}

template <class T>
std::string bytes_of(const std::vector<T>& v) {
  return std::string(reinterpret_cast<const char*>(v.data()), v.size() * sizeof(T));
}

template <class T>
Value b64_array(const std::vector<T>& v) {
  return Value(base64_encode(bytes_of(v)));
}

// Как _round в export.py: дробные числа — до n знаков, остальное как есть.
Value round_json(const Value& v, int n) {
  switch (v.type()) {
    case Value::Type::kDouble:
      return Value(py_round(v.as_double(), n));
    case Value::Type::kArray: {
      Value a = Value::array();
      for (const auto& x : v.as_array()) a.push_back(round_json(x, n));
      return a;
    }
    case Value::Type::kObject: {
      Value o = Value::object();
      for (const auto& [k, x] : v.as_object()) o[k] = round_json(x, n);
      return o;
    }
    default:
      return v;
  }
}

std::string replace_all(std::string s, std::string_view from, std::string_view to) {
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

std::string esc(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

// Число для отчёта с десятичной запятой; без d — знаков тем меньше, чем больше число.
std::string fnum(double v, int d = -1) {
  if (d < 0) {
    const double a = std::abs(v);
    d = a >= 100 ? 0 : (a >= 10 ? 1 : 2);
  }
  std::string s = std::format("{:.{}f}", v, d);
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

// Целое с узкими неразрывными пробелами между разрядами (как в прототипе).
std::string fint(long long v) {
  std::string s = std::to_string(v);
  const std::size_t start = (!s.empty() && s[0] == '-') ? 1 : 0;
  for (std::size_t i = s.size(); i > start + 3; i -= 3) s.insert(i - 3, "\u202F");
  return s;
}

std::string now_text() {
  const std::time_t t = std::time(nullptr);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &t);
#else
  localtime_r(&t, &tm);
#endif
  char buf[32];
  std::strftime(buf, sizeof buf, "%d.%m.%Y %H:%M", &tm);
  return buf;
}

const char* verdict_name(const std::string& v) {
  if (v == "ok") return "Выдержит";
  if (v == "risk") return "Мало запаса";
  return "Разрушится";
}

Value material_json(const material::Material& m) {
  Value o = Value::object();
  o["name"] = m.name;
  o["density"] = m.density;
  const std::pair<const char*, double> num[] = {
      {"E1", m.E1},   {"E2", m.E2},   {"E3", m.E3},   {"G12", m.G12},   {"G13", m.G13},   {"G23", m.G23},
      {"nu12", m.nu12}, {"nu13", m.nu13}, {"nu23", m.nu23}, {"Xt", m.Xt}, {"Xc", m.Xc}, {"Yt", m.Yt},
      {"Yc", m.Yc},   {"Zt", m.Zt},   {"Zc", m.Zc},   {"S12", m.S12},   {"S13", m.S13},   {"S23", m.S23},
      {"cte", m.cte}, {"hdt", m.hdt}, {"tg", m.tg},   {"creep_strength", m.creep_strength},
      {"creep_modulus", m.creep_modulus}, {"fatigue_k", m.fatigue_k}, {"elongation", m.elongation}};
  for (const auto& [k, v] : num) o[k] = v;
  o["note"] = m.note;
  o["key"] = m.key;
  return o;
}

Value model_payload(const analysis::Model& model) {
  const auto& vm = model.vm;
  const auto& mesh = model.mesh;
  const std::size_t n = vm.size();
  std::vector<std::int16_t> elems(3 * n);
  std::vector<std::uint8_t> role(n, 2), rho(n);
  for (std::size_t e = 0; e < n; ++e) {
    elems[3 * e] = static_cast<std::int16_t>(vm.ix[e]);
    elems[3 * e + 1] = static_cast<std::int16_t>(vm.iy[e]);
    elems[3 * e + 2] = static_cast<std::int16_t>(vm.iz[e]);
    switch (vm.role[e]) {
      case gcode::Role::OuterWall:
      case gcode::Role::InnerWall:
        role[e] = 0;
        break;
      case gcode::Role::Solid:
        role[e] = 1;
        break;
      default:
        role[e] = 2;
    }
    rho[e] = static_cast<std::uint8_t>(std::clamp(std::nearbyint(vm.rho(e) * 255), 0.0, 255.0));
  }
  std::vector<std::int32_t> node_flat(mesh.node_flat.size());
  for (std::size_t i = 0; i < node_flat.size(); ++i) node_flat[i] = static_cast<std::int32_t>(mesh.node_flat[i]);

  Value grid = Value::object();
  grid["nx"] = vm.nx;
  grid["ny"] = vm.ny;
  grid["nz"] = vm.nz;
  grid["sx"] = vm.sx;
  grid["sy"] = vm.sy;
  grid["x0"] = vm.x0;
  grid["y0"] = vm.y0;
  grid["z_edges"] = Value::array_of(vm.z_edges);
  Value m = Value::object();
  m["grid"] = std::move(grid);
  m["origin"] = Value::array_of(model.origin);
  m["size"] = Value::array_of(model.size);
  m["n_elems"] = n;
  m["n_nodes"] = mesh.n_nodes;
  m["elems"] = b64_array(elems);
  m["role"] = b64_array(role);
  m["rho"] = b64_array(rho);
  m["node_flat"] = b64_array(node_flat);
  m["summary"] = round_json(analysis::model_summary(model), 4);
  return m;
}

Value results_payload(const analysis::Model& model, const analysis::AnalysisResult& result) {
  Value out = Value::array();
  for (const auto& r : result.cases) {
    std::vector<float> sf(r.sf.size()), vmis(r.von_mises.size()), u(r.u.size());
    std::vector<std::uint8_t> mode(r.mode.size());
    for (std::size_t i = 0; i < sf.size(); ++i) sf[i] = static_cast<float>(std::min(r.sf[i], 1e4));
    for (std::size_t i = 0; i < vmis.size(); ++i) vmis[i] = static_cast<float>(r.von_mises[i]);
    for (std::size_t i = 0; i < u.size(); ++i) u[i] = static_cast<float>(r.u[i]);
    for (std::size_t i = 0; i < mode.size(); ++i) mode[i] = static_cast<std::uint8_t>(r.mode[i]);
    Value sel = Value::object();
    for (const auto& [name, faces] : r.sel_faces) {
      Value keys = Value::array();
      for (auto f : faces) keys.push_back(model.mesh.face_key[static_cast<std::size_t>(f)]);
      sel[name] = std::move(keys);
    }
    Value c = Value::object();
    c["summary"] = round_json(analysis::to_json(r.summary), 4);
    c["sf"] = b64_array(sf);
    c["vm"] = b64_array(vmis);
    c["mode"] = b64_array(mode);
    c["u"] = b64_array(u);
    c["sel"] = std::move(sel);
    out.push_back(std::move(c));
  }
  Value res = Value::object();
  res["results"] = std::move(out);
  res["material"] = round_json(material_json(result.material), 4);
  res["target_sf"] = result.target_sf;
  res["total_time"] = py_round(result.total_time, 2);
  res["kz"] = material::infill_kz(model.vm.infill_pattern);
  return res;
}

std::string title_of(const json::Value& job, const ReportInfo& info) {
  if (!info.title.empty()) return info.title;
  if (const Value* t = job.find("title"); t && t->is_string() && !t->as_string().empty()) return t->as_string();
  return "Расчёт детали";
}

std::string header_html(const analysis::Model& model, const material::Material& m, const std::string& title,
                        const std::string& gcode_name, const std::string& date, const Value* subtitle) {
  const Value sm = analysis::model_summary(model);
  const double mass = sm.find("volume_mm3")->as_double() * m.density / 1000.0;
  const auto& sz = sm.find("size")->as_array();
  const std::string pat = sm.find("infill_pattern")->as_string();
  std::vector<std::pair<std::string, std::string>> chips = {
      {"Материал", m.name},
      {"Слой", fnum(sm.find("layer_height")->as_double(), 2) + " мм"},
      {"Заполнение", std::to_string(static_cast<long long>(py_round(sm.find("infill_density")->as_double() * 100, 0))) +
                         "%" + (pat.empty() ? "" : " " + pat)},
  };
  if (const Value* w = sm.find("wall_loops"); w && w->truthy()) chips.emplace_back("Стенки", std::to_string(w->as_int()));
  chips.emplace_back("Габарит", fnum(sz[0].as_double(), 1) + " × " + fnum(sz[1].as_double(), 1) + " × " +
                                    fnum(sz[2].as_double(), 1) + " мм");
  chips.emplace_back("Масса", fnum(mass, 1) + " г");
  chips.emplace_back("Сетка", fnum(sm.find("voxel")->as_double(), 2) + " мм · " +
                                  fint(sm.find("elements")->as_int()) + " эл.");
  const std::string slicer = sm.find("slicer")->as_string();
  if (!slicer.empty() && slicer != "unknown") chips.emplace_back("Слайсер", slicer);
  std::string spec;
  for (const auto& [k, v] : chips) spec += "<span class=\"spec\"><span>" + esc(k) + "</span><b>" + esc(v) + "</b></span>";
  const std::string src = gcode_name.empty() ? "" : " · " + esc(gcode_name);
  std::string h = "<header class=\"hdr\"><div class=\"eyebrow\">Расчёт прочности FDM-детали · " + date + src +
                  "</div><h1>" + esc(title) + "</h1>";
  if (subtitle && subtitle->is_string() && !subtitle->as_string().empty())
    h += "<p class=\"sub\">" + esc(subtitle->as_string()) + "</p>";
  h += "<div class=\"specs\">" + spec + "</div></header>";
  return h;
}

std::string cycles_text(double n) {
  if (!(n > 0)) return "";
  const int p = static_cast<int>(std::floor(std::log10(n)));
  const double mant = n / std::pow(10.0, p);
  return (std::abs(mant - 1) > 1e-6 ? fnum(mant, 1) + "·" : std::string()) + "10<sup>" + std::to_string(p) +
         "</sup> циклов";
}

std::string cases_html(const analysis::AnalysisResult& result) {
  std::string out;
  for (std::size_t i = 0; i < result.cases.size(); ++i) {
    const auto& s = result.cases[i].summary;
    const std::string& v = s.verdict;
    std::string kind = esc(s.duration_name);
    if (s.cycles && *s.cycles != 0.0) kind += " · " + cycles_text(*s.cycles);
    if (s.temperature) kind += " · " + fnum(*s.temperature, 0) + " °C";
    std::vector<std::pair<std::string, std::string>> rows = {
        {"Разрушение", esc(s.mode)},
        {"Где", "<span class=\"num\">" + fnum(s.sf_xyz[0], 1) + " · " + fnum(s.sf_xyz[1], 1) + " · " +
                    fnum(s.sf_xyz[2], 1) + " мм</span>"},
        {"Прогиб", "<span class=\"num\">" + fnum(s.max_disp, 2) + " мм</span>"},
    };
    if (s.limit) rows.emplace_back("Выдержит до", esc(s.limit->short_text.empty() ? s.limit->text : s.limit->short_text));
    if (s.impact_factor && *s.impact_factor != 0.0)
      rows.emplace_back("Удар", "коэффициент <span class=\"num\">" + fnum(*s.impact_factor, 2) + "</span>");
    if (s.disp_force) {
      const auto& d = *s.disp_force;
      rows.emplace_back("Усилие", "<span class=\"num\">" + fnum(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1) +
                                      " Н</span>");
    }
    std::string kv;
    for (const auto& [k, val] : rows) kv += "<dt>" + k + "</dt><dd>" + val + "</dd>";
    std::vector<std::string> warns = s.warnings;
    if (s.sf_min_at_bc)
      warns.push_back("У места закрепления или приложения нагрузки есть локальный пик (запас " + fnum(s.sf_min, 2) +
                      "). Обычно это особенность расчётной модели; проверьте конструкцию узла.");
    std::string w;
    for (const auto& x : warns) w += "<div class=\"warn\">" + esc(x) + "</div>";
    const bool on = i == 0;
    if (i) out += "\n";
    out += std::string("<button type=\"button\" class=\"case") + (on ? " on" : "") + "\" aria-pressed=\"" +
           (on ? "true" : "false") + "\"><div class=\"case-top\"><div><h3>" + esc(s.name) +
           "</h3><div class=\"kind\">" + kind + "</div></div><span class=\"verdict " + v + "\">" + verdict_name(v) +
           "</span></div><div class=\"sfrow\"><span class=\"sfbig " + v + "\">" + fnum(std::min(s.sf, 999.0), 2) +
           "</span><span class=\"sflabel\">запас прочности<br>нужно не меньше " + fnum(s.target_sf, 1) +
           "</span></div><dl class=\"kv\">" + kv + "</dl>" + w + "</button>";
  }
  return out;
}

std::string details_html(const analysis::Model& model, const analysis::AnalysisResult& result) {
  const auto& m = result.material;
  struct Row {
    const char* name;
    double a, b, c;
  };
  const Row rows[] = {{"Модуль упругости, МПа", m.E1, m.E2, m.E3},
                      {"Прочность на растяжение, МПа", m.Xt, m.Yt, m.Zt},
                      {"Прочность на сжатие, МПа", m.Xc, m.Yc, m.Zc}};
  std::string tb;
  for (const auto& r : rows)
    tb += std::string("<tr><td>") + r.name + "</td><td>" + fnum(r.a, 0) + "</td><td>" + fnum(r.b, 0) + "</td><td>" +
          fnum(r.c, 0) + "</td></tr>";
  tb += "<tr><td>Прочность на сдвиг, МПа</td><td>—</td><td>" + fnum(m.S12, 0) + "</td><td>" + fnum(m.S13, 0) +
        "</td></tr>";
  return std::string(R"(<section class="details">
<div>
<h2>Как читать результат</h2>
<ul>
<li><b>Запас прочности</b> показывает, во сколько раз можно увеличить нагрузку до разрушения. Меньше 1 — деталь сломается. Для обычных деталей нужно 2 и больше, для ответственных — 3.</li>
<li><b>Что разрушится первым</b>: разрыв нити, отрыв соседних нитей в слое или расслоение между слоями. Расслоение — типичная слабость печати: по Z пластик обычно в 1,5–2 раза слабее.</li>
<li>Длительная нагрузка учитывает ползучесть пластика, циклическая — усталость, температура — размягчение.</li>
<li>Острые внутренние углы на воксельной сетке дают завышенные пики. Если минимум запаса в таком углу, добавьте скругление и пересчитайте.</li>
</ul>
<h2 style="margin-top:18px">Допущения</h2>
<ul>
<li>Линейная упругость и малые перемещения. Контакт, трение и потеря устойчивости не считаются.</li>
<li>Свойства материала типовые для хорошей печати сухим пластиком. Реальная прочность между слоями зависит от температуры, обдува и скорости — для ответственных деталей замените значения своими испытаниями.</li>
<li>Стенки, верх и низ учитываются по реальным траекториям из G-code; разреженное заполнение усредняется по ячейке рисунка.</li>
</ul>
</div>
<div>
<h2>Материал: )") +
         esc(m.name) +
         R"(</h2>
<div class="tbl-wrap"><table class="props"><thead><tr><th></th><th>вдоль нити</th><th>поперёк нити</th><th>между слоями</th></tr></thead><tbody>)" +
         tb + "</tbody></table></div>\n<p class=\"foot\">" + esc(m.note) + " Плотность " + fnum(m.density, 2) +
         " г/см³ · теплостойкость (HDT) " + fnum(m.hdt, 0) + " °C · длительная прочность " +
         std::to_string(static_cast<long long>(py_round(m.creep_strength * 100, 0))) +
         "% от кратковременной.<br>\nРасчётная сетка: " + fint(static_cast<long long>(model.vm.size())) +
         " элементов, " + fint(model.mesh.n_nodes * 3) + " неизвестных · время расчёта " +
         fnum(result.total_time, 1) + " с · kika " + kVersion + "</p>\n</div>\n</section>";
}

}  // namespace

std::string gzip_compress(std::string_view data, int level) {
  z_stream zs{};
  if (deflateInit2(&zs, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
    throw std::runtime_error("gzip: не удалось начать сжатие");
  std::string out;
  out.resize(deflateBound(&zs, static_cast<uLong>(data.size())));
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  zs.next_out = reinterpret_cast<Bytef*>(out.data());
  zs.avail_out = static_cast<uInt>(out.size());
  const int rc = deflate(&zs, Z_FINISH);
  const auto written = zs.total_out;
  deflateEnd(&zs);
  if (rc != Z_STREAM_END) throw std::runtime_error("gzip: ошибка сжатия");
  out.resize(static_cast<std::size_t>(written));
  return out;
}

std::string gzip_decompress(std::string_view data) {
  z_stream zs{};
  if (inflateInit2(&zs, 15 + 32) != Z_OK) throw std::runtime_error("gzip: не удалось начать распаковку");
  zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
  zs.avail_in = static_cast<uInt>(data.size());
  std::string out;
  char buf[1 << 16];
  int rc = Z_OK;
  while (rc != Z_STREAM_END) {
    zs.next_out = reinterpret_cast<Bytef*>(buf);
    zs.avail_out = static_cast<uInt>(sizeof buf);
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) {
      inflateEnd(&zs);
      throw std::runtime_error("gzip: данные повреждены");
    }
    out.append(buf, sizeof buf - zs.avail_out);
    if (rc == Z_OK && zs.avail_in == 0 && zs.avail_out != 0) {
      inflateEnd(&zs);
      throw std::runtime_error("gzip: данные оборваны");
    }
  }
  inflateEnd(&zs);
  return out;
}

std::string base64_encode(std::string_view data) {
  static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve((data.size() + 2) / 3 * 4);
  std::size_t i = 0;
  for (; i + 3 <= data.size(); i += 3) {
    const std::uint32_t v = (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16) |
                            (static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8) |
                            static_cast<unsigned char>(data[i + 2]);
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += tbl[(v >> 6) & 63];
    out += tbl[v & 63];
  }
  const std::size_t rest = data.size() - i;
  if (rest) {
    std::uint32_t v = static_cast<std::uint32_t>(static_cast<unsigned char>(data[i])) << 16;
    if (rest == 2) v |= static_cast<std::uint32_t>(static_cast<unsigned char>(data[i + 1])) << 8;
    out += tbl[(v >> 18) & 63];
    out += tbl[(v >> 12) & 63];
    out += rest == 2 ? tbl[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

std::string base64_decode(std::string_view text) {
  auto val = [](char c) -> int {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
  };
  std::string out;
  out.reserve(text.size() / 4 * 3);
  std::uint32_t acc = 0;
  int bits = 0;
  for (char c : text) {
    if (c == '=') break;
    const int v = val(c);
    if (v < 0) throw std::invalid_argument("base64: недопустимый символ");
    acc = (acc << 6) | static_cast<std::uint32_t>(v);
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out += static_cast<char>((acc >> bits) & 0xFF);
    }
  }
  return out;
}

json::Value report_payload(const analysis::Model& model, const analysis::AnalysisResult& result,
                           const json::Value& job_json, const ReportInfo& info) {
  Value meta = Value::object();
  meta["title"] = title_of(job_json, info);
  meta["gcode"] = info.gcode_name;
  meta["date"] = info.date.empty() ? now_text() : info.date;
  meta["version"] = kVersion;
  Value p = Value::object();
  p["model"] = model_payload(model);
  p["results"] = results_payload(model, result);
  p["job"] = job_json;
  p["meta"] = std::move(meta);
  return p;
}

std::string report_html(const analysis::Model& model, const analysis::AnalysisResult& result,
                        const json::Value& job_json, ReportInfo info) {
  if (info.date.empty()) info.date = now_text();
  const std::string title = title_of(job_json, info);
  const Value payload = report_payload(model, result, job_json, info);
  const std::string packed = base64_encode(gzip_compress(json::dump(payload, json::kCompact), 6));
  std::string html(asset("report.html"));
  html = replace_all(std::move(html), "__TITLE__", esc(title));
  html = replace_all(std::move(html), "__HEADER__",
                     header_html(model, result.material, title, info.gcode_name, info.date, job_json.find("subtitle")));
  html = replace_all(std::move(html), "__CASES__", cases_html(result));
  html = replace_all(std::move(html), "__DETAILS__", details_html(model, result));
  html = replace_all(std::move(html), "__PAYLOAD__", packed);
  html = replace_all(std::move(html), "/*__VIEWER_CSS__*/", asset("viewer.css"));
  html = replace_all(std::move(html), "/*__VIEWER_JS__*/", asset("viewer.js"));
  html = replace_all(std::move(html), "/*__REPORT_JS__*/", asset("report.js"));
  html = replace_all(std::move(html), "/*__THREE__*/", asset("vendor/three.min.js"));
  // шапка документа: всё до <div class="page"> — в <head>
  const std::string page = "<div class=\"page\">";
  const auto pos = html.find(page);
  if (pos == std::string::npos) throw std::logic_error("шаблон отчёта без <div class=\"page\">");
  return "<!doctype html>\n<html lang=\"ru\">\n<head>\n<meta charset=\"utf-8\">\n"
         "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1, viewport-fit=cover\">\n" +
         html.substr(0, pos) + "</head>\n<body>\n" + html.substr(pos) + "\n</body>\n</html>\n";
}

}  // namespace kika::report
