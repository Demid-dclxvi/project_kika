// Moonraker: HTTP API (docs/external_api в репозитории Moonraker) — свой клиент.

#include "kika/printer/moonraker.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <format>
#include <mutex>
#include <thread>

namespace kika::printer {

namespace {

using json::Value;

std::string text_of(const Value* v) { return v && v->is_string() ? v->as_string() : std::string(); }
double num_of(const Value* v, double def = 0) { return v && v->is_number() ? v->as_double() : def; }

// Ответ Moonraker: {"result": …} или {"error": {"code": …, "message": …}}.
Value unwrap(const HttpResponse& r, const std::string& what) {
  Value j;
  try {
    j = json::parse(r.body);
  } catch (const json::ParseError&) {
    if (r.status >= 400)
      throw NetError(std::format("{}: принтер ответил ошибкой {} {}.", what, r.status, r.reason));
    throw NetError(what + ": принтер прислал не JSON — по этому адресу не Moonraker?");
  }
  if (const Value* e = j.find("error"); e && !e->is_null()) {
    std::string msg = e->is_object() ? text_of(e->find("message")) : text_of(e);
    if (msg.empty()) msg = std::format("код {}", r.status);
    if (r.status == 401 || r.status == 403)
      throw NetError(what + ": принтер требует ключ API (" + msg + "). Введите ключ из настроек Moonraker.");
    throw NetError(what + ": " + msg);
  }
  if (r.status >= 400) throw NetError(std::format("{}: принтер ответил ошибкой {} {}.", what, r.status, r.reason));
  if (const Value* res = j.find("result")) return *res;
  return j;
}

// Кириллица → латиница (как в загранпаспорте, упрощённо).
std::string translit(std::string_view s) {
  static const std::pair<const char*, const char*> table[] = {
      {"а", "a"},  {"б", "b"},  {"в", "v"},  {"г", "g"},  {"д", "d"},   {"е", "e"},  {"ё", "e"},   {"ж", "zh"},
      {"з", "z"},  {"и", "i"},  {"й", "i"},  {"к", "k"},  {"л", "l"},   {"м", "m"},  {"н", "n"},   {"о", "o"},
      {"п", "p"},  {"р", "r"},  {"с", "s"},  {"т", "t"},  {"у", "u"},   {"ф", "f"},  {"х", "kh"},  {"ц", "ts"},
      {"ч", "ch"}, {"ш", "sh"}, {"щ", "shch"}, {"ъ", ""}, {"ы", "y"},   {"ь", ""},   {"э", "e"},   {"ю", "iu"},
      {"я", "ia"}, {"А", "A"},  {"Б", "B"},  {"В", "V"},  {"Г", "G"},   {"Д", "D"},  {"Е", "E"},   {"Ё", "E"},
      {"Ж", "Zh"}, {"З", "Z"},  {"И", "I"},  {"Й", "I"},  {"К", "K"},   {"Л", "L"},  {"М", "M"},   {"Н", "N"},
      {"О", "O"},  {"П", "P"},  {"Р", "R"},  {"С", "S"},  {"Т", "T"},   {"У", "U"},  {"Ф", "F"},   {"Х", "Kh"},
      {"Ц", "Ts"}, {"Ч", "Ch"}, {"Ш", "Sh"}, {"Щ", "Shch"}, {"Ъ", ""}, {"Ы", "Y"},   {"Ь", ""},    {"Э", "E"},
      {"Ю", "Iu"}, {"Я", "Ia"},
  };
  std::string out;
  for (std::size_t i = 0; i < s.size();) {
    bool done = false;
    for (const auto& [ru, en] : table) {
      const std::string_view r(ru);
      if (s.substr(i, r.size()) == r) {
        out += en;
        i += r.size();
        done = true;
        break;
      }
    }
    if (done) continue;
    const auto c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
      out.push_back(static_cast<char>(c));
      ++i;
    } else {
      // прочие не-ASCII символы: пропускаем символ UTF-8 целиком
      ++i;
      while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) ++i;
      out.push_back('_');
    }
  }
  return out;
}

}  // namespace

Value Moonraker::get(const std::string& target) const {
  HttpRequest rq;
  rq.target = target;
  if (!c_.api_key.empty()) rq.headers.push_back({"X-Api-Key", c_.api_key});
  return unwrap(http_request(c_.host, c_.port, rq, c_.timeout_ms), "Запрос " + target.substr(0, target.find('?')));
}

Value Moonraker::post(const std::string& target, const std::string& body, const std::string& content_type,
                      const SendProgress& progress, int timeout_ms) const {
  HttpRequest rq;
  rq.method = "POST";
  rq.target = target;
  rq.body = body;
  if (!content_type.empty()) rq.headers.push_back({"Content-Type", content_type});
  if (!c_.api_key.empty()) rq.headers.push_back({"X-Api-Key", c_.api_key});
  return unwrap(http_request(c_.host, c_.port, rq, timeout_ms > 0 ? timeout_ms : c_.timeout_ms, progress),
                "Команда " + target.substr(0, target.find('?')));
}

PrinterInfo Moonraker::info() const {
  PrinterInfo pi;
  pi.host = c_.host;
  pi.port = c_.port;
  const Value server = get("/server/info");
  pi.klippy_state = text_of(server.find("klippy_state"));
  pi.moonraker_version = text_of(server.find("moonraker_version"));
  if (pi.klippy_state == "ready" || pi.klippy_state.empty()) {
    try {
      const Value p = get("/printer/info");
      pi.hostname = text_of(p.find("hostname"));
      pi.klipper_version = text_of(p.find("software_version"));
      if (const auto st = text_of(p.find("state")); !st.empty()) pi.klippy_state = st;
      pi.state_message = text_of(p.find("state_message"));
    } catch (const NetError&) {
      // Klipper не отвечает — достаточно сведений Moonraker
    }
  }
  return pi;
}

PrinterStatus Moonraker::status() const {
  const Value r = get(
      "/printer/objects/query?print_stats&virtual_sdcard&display_status&extruder=temperature,target"
      "&heater_bed=temperature,target&heater_generic%20chamber=temperature,target");
  PrinterStatus st;
  const Value* s = r.find("status");
  if (!s) return st;
  if (const Value* ps = s->find("print_stats")) {
    st.state = text_of(ps->find("state"));
    st.filename = text_of(ps->find("filename"));
    st.message = text_of(ps->find("message"));
    st.print_duration = num_of(ps->find("print_duration"));
    st.total_duration = num_of(ps->find("total_duration"));
    if (const Value* info = ps->find("info"); info && info->is_object()) {
      st.layer = static_cast<int>(num_of(info->find("current_layer")));
      st.total_layers = static_cast<int>(num_of(info->find("total_layer")));
    }
  }
  if (const Value* vs = s->find("virtual_sdcard")) st.progress = num_of(vs->find("progress"));
  if (const Value* ds = s->find("display_status"); ds && st.progress <= 0) st.progress = num_of(ds->find("progress"));
  if (const Value* e = s->find("extruder")) {
    st.nozzle = num_of(e->find("temperature"));
    st.nozzle_target = num_of(e->find("target"));
  }
  if (const Value* b = s->find("heater_bed")) {
    st.bed = num_of(b->find("temperature"));
    st.bed_target = num_of(b->find("target"));
  }
  if (const Value* c = s->find("heater_generic chamber"); c && c->is_object() && c->find("temperature")) {
    st.chamber = num_of(c->find("temperature"));
    st.chamber_target = num_of(c->find("target"));
  }
  return st;
}

UploadResult Moonraker::upload(const std::string& filename, const std::string& gcode, bool start,
                               const SendProgress& progress) const {
  const Multipart mp = multipart({{"root", "gcodes"}, {"print", start ? "true" : "false"}}, "file", filename, gcode);
  // большой файл по Wi-Fi идёт долго: тайм-аут — на каждое ожидание, а не на весь файл
  const Value r = post("/server/files/upload", mp.body, mp.content_type(), progress, std::max(c_.timeout_ms, 30000));
  UploadResult u;
  if (r.is_string()) {
    u.path = r.as_string();  // старые версии Moonraker: {"result": "имя"}
  } else if (const Value* item = r.find("item"); item && item->is_object()) {
    u.path = text_of(item->find("path"));
  }
  if (u.path.empty()) u.path = filename;
  u.print_started = r.find("print_started") && r.find("print_started")->is_bool() && r.find("print_started")->as_bool();
  u.print_queued = r.find("print_queued") && r.find("print_queued")->is_bool() && r.find("print_queued")->as_bool();
  return u;
}

void Moonraker::start(const std::string& filename) const { post("/printer/print/start?filename=" + url_encode(filename)); }
void Moonraker::pause() const { post("/printer/print/pause"); }
void Moonraker::resume() const { post("/printer/print/resume"); }
void Moonraker::cancel() const { post("/printer/print/cancel"); }

std::optional<PrinterInfo> probe(const std::string& host, std::optional<int> port, int timeout_ms) {
  std::vector<int> ports;
  if (port)
    ports.push_back(*port);
  else
    ports = {kMoonrakerPort, 80};
  for (int p : ports) {
    try {
      Connection c;
      c.host = host;
      c.port = p;
      c.timeout_ms = timeout_ms;
      const Moonraker m(c);
      const Value server = m.get("/server/info");
      if (!server.is_object() || !server.find("klippy_state")) continue;
      return m.info();
    } catch (const NetError&) {
    } catch (const json::ParseError&) {
    }
  }
  return std::nullopt;
}

std::vector<PrinterInfo> discover(int timeout_ms, const std::function<void(double)>& progress) {
  std::vector<PrinterInfo> found;
  const auto me = local_ipv4();
  if (!me) return found;
  const std::string prefix = me->substr(0, me->rfind('.') + 1);
  std::mutex mu;
  std::atomic<int> next{1}, done{0};
  const unsigned threads = 48;
  std::vector<std::thread> pool;
  for (unsigned t = 0; t < threads; ++t)
    pool.emplace_back([&] {
      for (int i = next++; i <= 254; i = next++) {
        if (auto pi = probe(prefix + std::to_string(i), kMoonrakerPort, timeout_ms)) {
          const std::lock_guard lock(mu);
          found.push_back(std::move(*pi));
        }
        const int d = ++done;
        if (progress) {
          const std::lock_guard lock(mu);
          progress(d / 254.0);
        }
      }
    });
  for (auto& th : pool) th.join();
  std::sort(found.begin(), found.end(), [](const PrinterInfo& a, const PrinterInfo& b) { return a.host < b.host; });
  return found;
}

std::string printer_filename(std::string_view name) {
  std::string s = translit(name);
  std::string out;
  for (char c : s) {
    const auto u = static_cast<unsigned char>(c);
    if (u < 0x20 || std::string_view("\\/:*?\"<>|#%&").find(c) != std::string_view::npos)
      out.push_back('_');
    else
      out.push_back(c);
  }
  while (!out.empty() && (out.front() == ' ' || out.front() == '.')) out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.empty()) out = "kika";
  std::string low = out;
  for (char& c : low) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (!low.ends_with(".gcode")) out += ".gcode";
  return out;
}

}  // namespace kika::printer
