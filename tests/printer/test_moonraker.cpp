// Принтер по сети: свой HTTP-клиент и Moonraker против имитации сервера на 127.0.0.1.

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "kika/printer/http.hpp"
#include "kika/printer/moonraker.hpp"
#include "kika/util/json.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using Catch::Matchers::WithinAbs;

namespace {

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t kNone = INVALID_SOCKET;
void sclose(sock_t s) { closesocket(s); }
#else
using sock_t = int;
const sock_t kNone = -1;
void sclose(sock_t s) { ::close(s); }
#endif

struct Request {
  std::string method, target, body;
  std::map<std::string, std::string> headers;  // имена — строчными
};
struct Reply {
  int status = 200;
  std::string body;
  bool chunked = false;
};

// Имитация HTTP-сервера: каждый запрос — handler, ответ и закрытие соединения.
class MockServer {
 public:
  explicit MockServer(std::function<Reply(const Request&)> handler) : handler_(std::move(handler)) {
#ifdef _WIN32
    WSADATA d;
    WSAStartup(MAKEWORD(2, 2), &d);
#endif
    s_ = ::socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), static_cast<socklen_t>(sizeof one));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = 0;
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    REQUIRE(::bind(s_, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof a)) == 0);
    REQUIRE(::listen(s_, 16) == 0);
    socklen_t len = static_cast<socklen_t>(sizeof a);
    getsockname(s_, reinterpret_cast<sockaddr*>(&a), &len);
    port_ = ntohs(a.sin_port);
    th_ = std::thread([this] { loop(); });
  }
  ~MockServer() {
    stop_ = true;
    // разбудить accept
    sock_t c = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(static_cast<unsigned short>(port_));
    inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
    ::connect(c, reinterpret_cast<const sockaddr*>(&a), static_cast<socklen_t>(sizeof a));
    sclose(c);
    th_.join();
    sclose(s_);
  }
  int port() const { return port_; }
  int requests() const { return count_; }

 private:
  void loop() {
    while (!stop_) {
      sock_t c = ::accept(s_, nullptr, nullptr);
      if (c == kNone) continue;
      if (stop_) {
        sclose(c);
        break;
      }
      serve(c);
      sclose(c);
    }
  }
  void serve(sock_t c) {
    std::string raw;
    char buf[8192];
    std::size_t head_end = std::string::npos, need = 0;
    Request rq;
    for (;;) {
      const auto n = ::recv(c, buf, static_cast<int>(sizeof buf), 0);
      if (n <= 0) return;
      raw.append(buf, static_cast<std::size_t>(n));
      if (head_end == std::string::npos && (head_end = raw.find("\r\n\r\n")) != std::string::npos) {
        const std::string first = raw.substr(0, raw.find("\r\n"));
        const auto sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
        rq.method = first.substr(0, sp1);
        rq.target = first.substr(sp1 + 1, sp2 - sp1 - 1);
        std::size_t p = raw.find("\r\n") + 2;
        while (p < head_end) {
          const auto e = raw.find("\r\n", p);
          const std::string line = raw.substr(p, e - p);
          const auto colon = line.find(':');
          std::string k = line.substr(0, colon);
          for (char& ch : k) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
          rq.headers[k] = line.substr(line.find_first_not_of(' ', colon + 1));
          p = e + 2;
        }
        need = rq.headers.count("content-length") ? std::stoul(rq.headers["content-length"]) : 0;
      }
      if (head_end != std::string::npos && raw.size() >= head_end + 4 + need) break;
    }
    rq.body = raw.substr(head_end + 4, need);
    ++count_;
    const Reply r = handler_(rq);
    std::string out = "HTTP/1.1 " + std::to_string(r.status) + (r.status < 400 ? " OK" : " Error") +
                      "\r\nContent-Type: application/json\r\nConnection: close\r\n";
    if (r.chunked) {
      out += "Transfer-Encoding: chunked\r\n\r\n";
      // по 7 байт: проверить склейку частей
      for (std::size_t i = 0; i < r.body.size(); i += 7) {
        const std::string part = r.body.substr(i, 7);
        char hex[16];
        std::snprintf(hex, sizeof hex, "%zx", part.size());
        out += std::string(hex) + "\r\n" + part + "\r\n";
      }
      out += "0\r\n\r\n";
    } else {
      out += "Content-Length: " + std::to_string(r.body.size()) + "\r\n\r\n" + r.body;
    }
    std::size_t off = 0;
    while (off < out.size()) {
      const auto n = ::send(c, out.data() + off, static_cast<int>(out.size() - off), 0);
      if (n <= 0) break;
      off += static_cast<std::size_t>(n);
    }
  }

  std::function<Reply(const Request&)> handler_;
  sock_t s_ = kNone;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::atomic<int> count_{0};
  std::thread th_;
};

// Поле multipart по имени.
std::string form_field(const std::string& body, const std::string& name) {
  const std::string key = "name=\"" + name + "\"";
  const auto p = body.find(key);
  if (p == std::string::npos) return {};
  const auto start = body.find("\r\n\r\n", p) + 4;
  const auto boundary_start = body.substr(0, body.find("\r\n"));
  const auto end = body.find("\r\n" + boundary_start, start);
  return body.substr(start, end - start);
}

}  // namespace

TEST_CASE("Сеть: адрес принтера, кодирование, multipart", "[printer]") {
  using kika::printer::parse_address;
  auto a = parse_address("  192.168.1.50 ");
  REQUIRE(a);
  CHECK(a->host == "192.168.1.50");
  CHECK(!a->port);
  a = parse_address("http://qidi-q2.local:7125/fluidd");
  REQUIRE(a);
  CHECK(a->host == "qidi-q2.local");
  CHECK(*a->port == 7125);
  a = parse_address("[fe80::1]:80");
  REQUIRE(a);
  CHECK(a->host == "fe80::1");
  CHECK(*a->port == 80);
  CHECK(!parse_address(""));
  CHECK(!parse_address("192.168.1.50:99999"));
  CHECK(!parse_address("https://printer"));
  CHECK(kika::printer::url_encode("Уголок 1.gcode") == "%D0%A3%D0%B3%D0%BE%D0%BB%D0%BE%D0%BA%201.gcode");
  CHECK(kika::printer::printer_filename("Уголок: упор/1") == "Ugolok_ upor_1.gcode");
  CHECK(kika::printer::printer_filename("part.GCODE") == "part.GCODE");
  const auto mp = kika::printer::multipart({{"root", "gcodes"}}, "file", "a.gcode", "G1 X1\n");
  CHECK(mp.content_type().starts_with("multipart/form-data; boundary="));
  CHECK(form_field(mp.body, "root") == "gcodes");
  CHECK(form_field(mp.body, "file") == "G1 X1\n");
}

TEST_CASE("Сеть: HTTP-клиент — длина, части, ошибки соединения", "[printer]") {
  MockServer srv([](const Request& rq) {
    Reply r;
    if (rq.target == "/chunked") {
      r.chunked = true;
      r.body = R"({"result": {"text": "длинный ответ по частям"}})";
    } else {
      r.body = R"({"result": ")" + rq.method + " " + rq.target + R"("})";
    }
    return r;
  });
  const auto r1 = kika::printer::http_request("127.0.0.1", srv.port(), {"GET", "/server/info", {}, {}}, 2000);
  CHECK(r1.status == 200);
  CHECK(kika::json::parse(r1.body).find("result")->as_string() == "GET /server/info");
  CHECK(r1.header("content-type") == "application/json");
  const auto r2 = kika::printer::http_request("127.0.0.1", srv.port(), {"GET", "/chunked", {}, {}}, 2000);
  CHECK(kika::json::parse(r2.body).find("result")->find("text")->as_string() == "длинный ответ по частям");
  // порт, где никто не слушает, — понятная ошибка, а не зависание
  CHECK_THROWS_AS(kika::printer::http_request("127.0.0.1", 1, {"GET", "/", {}, {}}, 1000), kika::printer::NetError);
  CHECK_THROWS_AS(kika::printer::http_request("нет-такого-принтера.invalid", 7125, {"GET", "/", {}, {}}, 1000),
                  kika::printer::NetError);
}

TEST_CASE("Сеть: Moonraker — сведения, состояние, отправка с запуском, команды", "[printer]") {
  std::mutex mu;
  std::string uploaded, upload_name, upload_print, upload_root, last_command, api_key;
  MockServer srv([&](const Request& rq) {
    const std::lock_guard lock(mu);
    Reply r;
    if (rq.headers.count("x-api-key")) api_key = rq.headers.at("x-api-key");
    if (rq.target == "/server/info") {
      r.body = R"({"result": {"klippy_connected": true, "klippy_state": "ready", "moonraker_version": "v0.9.3"}})";
    } else if (rq.target == "/printer/info") {
      r.body = R"({"result": {"state": "ready", "state_message": "Printer is ready", "hostname": "qidi-q2",
                   "software_version": "v0.12.0"}})";
    } else if (rq.target.starts_with("/printer/objects/query?")) {
      r.body = R"({"result": {"eventtime": 1.0, "status": {
        "print_stats": {"state": "printing", "filename": "Ugolok.gcode", "print_duration": 600.5, "total_duration": 700,
                        "message": "", "info": {"current_layer": 12, "total_layer": 50}},
        "virtual_sdcard": {"progress": 0.42},
        "extruder": {"temperature": 244.6, "target": 245.0},
        "heater_bed": {"temperature": 69.9, "target": 70.0},
        "heater_generic chamber": {"temperature": 35.2, "target": 0.0}}}})";
    } else if (rq.target == "/server/files/upload" && rq.method == "POST") {
      uploaded = form_field(rq.body, "file");
      upload_print = form_field(rq.body, "print");
      upload_root = form_field(rq.body, "root");
      const auto fn = rq.body.find("filename=\"");
      upload_name = rq.body.substr(fn + 10, rq.body.find('"', fn + 10) - fn - 10);
      r.status = 201;  // ответ загрузки у Moonraker — без обёртки "result"
      r.body = R"({"item": {"path": ")" + upload_name + R"(", "root": "gcodes", "size": 10},
                   "print_started": )" + (upload_print == "true" ? "true" : "false") +
               R"(, "print_queued": false, "action": "create_file"})";
    } else if (rq.target.starts_with("/printer/print/")) {
      last_command = rq.method + " " + rq.target;
      r.body = R"({"result": "ok"})";
    } else {
      r.status = 404;
      r.body = R"({"error": {"code": 404, "message": "Not Found"}})";
    }
    return r;
  });

  kika::printer::Connection c;
  c.host = "127.0.0.1";
  c.port = srv.port();
  c.api_key = "секрет";
  const kika::printer::Moonraker m(c);
  const auto info = m.info();
  CHECK(info.ready());
  CHECK(info.hostname == "qidi-q2");
  CHECK(info.klipper_version == "v0.12.0");
  CHECK(info.moonraker_version == "v0.9.3");
  CHECK(api_key == "секрет");

  const auto st = m.status();
  CHECK(st.state == "printing");
  CHECK(st.busy());
  CHECK(st.filename == "Ugolok.gcode");
  CHECK_THAT(st.progress, WithinAbs(0.42, 1e-12));
  CHECK(st.layer == 12);
  CHECK(st.total_layers == 50);
  CHECK_THAT(st.nozzle, WithinAbs(244.6, 1e-9));
  CHECK_THAT(st.bed_target, WithinAbs(70, 1e-9));
  REQUIRE(st.chamber);
  CHECK_THAT(*st.chamber, WithinAbs(35.2, 1e-9));

  // большой G-code — с ходом отправки
  std::string gcode;
  for (int i = 0; i < 20000; ++i) gcode += "G1 X" + std::to_string(i % 200) + " Y10 E0.1\n";
  std::size_t last_sent = 0, total = 0;
  const auto up = m.upload("Ugolok.gcode", gcode, true, [&](std::size_t sent, std::size_t all) {
    last_sent = sent;
    total = all;
    return true;
  });
  CHECK(up.print_started);
  CHECK(up.path == "Ugolok.gcode");
  {
    const std::lock_guard lock(mu);
    CHECK(uploaded == gcode);
    CHECK(upload_name == "Ugolok.gcode");
    CHECK(upload_print == "true");
    CHECK(upload_root == "gcodes");
  }
  CHECK(last_sent == total);
  CHECK(total > gcode.size());
  // отправку можно прервать
  CHECK_THROWS_AS(m.upload("x.gcode", gcode, false, [](std::size_t, std::size_t) { return false; }), kika::printer::NetError);

  m.pause();
  CHECK(last_command == "POST /printer/print/pause");
  m.resume();
  CHECK(last_command == "POST /printer/print/resume");
  m.cancel();
  CHECK(last_command == "POST /printer/print/cancel");
  m.start("Уголок 1.gcode");
  CHECK(last_command == "POST /printer/print/start?filename=%D0%A3%D0%B3%D0%BE%D0%BB%D0%BE%D0%BA%201.gcode");
  // ошибка Moonraker — её текст
  std::string msg;
  try {
    (void)m.get("/server/nothing");
  } catch (const kika::printer::NetError& e) {
    msg = e.what();
  }
  CHECK(msg.find("Not Found") != std::string::npos);

  // проба адреса: Moonraker есть — на нашем порту; где никого нет — пусто
  const auto pr = kika::printer::probe("127.0.0.1", srv.port(), 1000);
  REQUIRE(pr);
  CHECK(pr->port == srv.port());
  CHECK(pr->hostname == "qidi-q2");
  CHECK(!kika::printer::probe("127.0.0.1", 1, 500));
}
