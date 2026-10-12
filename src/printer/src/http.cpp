// HTTP/1.1 поверх сокетов: Winsock на Windows, POSIX на остальных.

#include "kika/printer/http.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <format>
#include <mutex>
#include <random>

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
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace kika::printer {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
const socket_t kBad = INVALID_SOCKET;
void close_socket(socket_t s) { closesocket(s); }
int last_error() { return WSAGetLastError(); }
bool would_block(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
void ensure_started() {
  static std::once_flag once;
  std::call_once(once, [] {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  });
}
void set_nonblocking(socket_t s) {
  u_long mode = 1;
  ioctlsocket(s, FIONBIO, &mode);
}
constexpr int kSendFlags = 0;
#else
using socket_t = int;
const socket_t kBad = -1;
void close_socket(socket_t s) { ::close(s); }
int last_error() { return errno; }
bool would_block(int e) { return e == EWOULDBLOCK || e == EAGAIN || e == EINPROGRESS; }
void ensure_started() {}
void set_nonblocking(socket_t s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

// Ждать готовности сокета: 1 — готов, 0 — тайм-аут, −1 — ошибка.
int wait_socket(socket_t s, bool write, int timeout_ms) {
#ifdef _WIN32
  // наборы заполняем сами: макросы FD_SET дают предупреждения /W4
  fd_set rd{}, wr{}, ex{};
  fd_set& want = write ? wr : rd;
  want.fd_count = 1;
  want.fd_array[0] = s;
  ex.fd_count = 1;
  ex.fd_array[0] = s;
  timeval tv{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  const int r = select(0, &rd, &wr, &ex, &tv);
  if (r < 0) return -1;
  if (r == 0) return 0;
  if (ex.fd_count > 0) return -1;  // select оставляет в наборах только сработавшие сокеты
  return 1;
#else
  pollfd p{};
  p.fd = s;
  p.events = write ? POLLOUT : POLLIN;
  for (;;) {
    const int r = ::poll(&p, 1, timeout_ms);
    if (r < 0 && errno == EINTR) continue;
    if (r < 0) return -1;
    if (r == 0) return 0;
    if (p.revents & (POLLERR | POLLNVAL)) return -1;
    return 1;
  }
#endif
}

class Socket {
 public:
  Socket(const std::string& host, int port, int timeout_ms) : timeout_(timeout_ms) {
    ensure_started();
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* res = nullptr;
    const std::string service = std::to_string(port);
    if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res)
      throw NetError("Не найден адрес «" + host + "». Проверьте, что принтер включён и в той же сети, или введите его IP-адрес.");
    std::string why = "не отвечает";
    for (addrinfo* a = res; a; a = a->ai_next) {
      socket_t s = ::socket(a->ai_family, a->ai_socktype, a->ai_protocol);
      if (s == kBad) continue;
      set_nonblocking(s);
#if defined(__APPLE__) && defined(SO_NOSIGPIPE)
      int one = 1;
      setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
      const int rc = ::connect(s, a->ai_addr, static_cast<socklen_t>(a->ai_addrlen));
      bool ok = rc == 0;
      if (!ok && would_block(last_error())) {
        const int w = wait_socket(s, true, timeout_);
        if (w > 0) {
          int err = 0;
          socklen_t len = static_cast<socklen_t>(sizeof err);
          getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
          ok = err == 0;
          if (!ok) why = "отказал в соединении";
        } else if (w == 0) {
          why = "не ответил вовремя";
        } else {
          why = "отказал в соединении";
        }
      }
      if (ok) {
        s_ = s;
        break;
      }
      close_socket(s);
    }
    freeaddrinfo(res);
    if (s_ == kBad) throw NetError(std::format("{}:{} {}.", host, port, why));
  }
  ~Socket() {
    if (s_ != kBad) close_socket(s_);
  }
  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  void send_all(std::string_view data, const SendProgress& progress, std::size_t& sent, std::size_t total) {
    std::size_t off = 0;
    while (off < data.size()) {
      const std::size_t chunk = std::min<std::size_t>(data.size() - off, 64 * 1024);
      const auto n = ::send(s_, data.data() + off, static_cast<int>(chunk), kSendFlags);
      if (n < 0) {
        if (!would_block(last_error())) throw NetError("Соединение с принтером оборвалось при отправке.");
        const int w = wait_socket(s_, true, timeout_);
        if (w == 0) throw NetError("Принтер перестал принимать данные (тайм-аут).");
        if (w < 0) throw NetError("Соединение с принтером оборвалось при отправке.");
        continue;
      }
      off += static_cast<std::size_t>(n);
      sent += static_cast<std::size_t>(n);
      if (progress && !progress(sent, total)) throw NetError("Отправка отменена.");
    }
  }

  // Прочитать сколько есть; 0 — соединение закрыто.
  std::size_t read_some(char* buf, std::size_t cap) {
    for (;;) {
      const auto n = ::recv(s_, buf, static_cast<int>(cap), 0);
      if (n >= 0) return static_cast<std::size_t>(n);
      if (!would_block(last_error())) throw NetError("Соединение с принтером оборвалось.");
      const int w = wait_socket(s_, false, timeout_);
      if (w == 0) throw NetError("Принтер не ответил вовремя.");
      if (w < 0) throw NetError("Соединение с принтером оборвалось.");
    }
  }

 private:
  socket_t s_ = kBad;
  int timeout_;
};

std::string lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

std::string trim(std::string_view s) {
  std::size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\r')) --b;
  return std::string(s.substr(a, b - a));
}

// Тело в кодировке chunked.
std::string dechunk(const std::string& raw) {
  std::string out;
  std::size_t p = 0;
  for (;;) {
    const std::size_t eol = raw.find("\r\n", p);
    if (eol == std::string::npos) throw NetError("Принтер прислал неполный ответ.");
    const std::size_t len = std::stoul(raw.substr(p, eol - p), nullptr, 16);
    p = eol + 2;
    if (len == 0) break;
    if (p + len > raw.size()) throw NetError("Принтер прислал неполный ответ.");
    out.append(raw, p, len);
    p += len + 2;
  }
  return out;
}

}  // namespace

std::optional<std::string> HttpResponse::header(std::string_view name) const {
  const std::string n = lower(name);
  for (const auto& [k, v] : headers)
    if (lower(k) == n) return v;
  return std::nullopt;
}

HttpResponse http_request(const std::string& host, int port, const HttpRequest& rq, int timeout_ms,
                          const SendProgress& progress) {
  Socket sock(host, port, timeout_ms);
  const bool v6 = host.find(':') != std::string::npos;
  std::string head = std::format("{} {} HTTP/1.1\r\nHost: {}{}{}:{}\r\nConnection: close\r\nUser-Agent: Kika\r\n", rq.method,
                                 rq.target, v6 ? "[" : "", host, v6 ? "]" : "", port);
  bool has_len = false;
  for (const auto& [k, v] : rq.headers) {
    head += k + ": " + v + "\r\n";
    has_len = has_len || lower(k) == "content-length";
  }
  if (!has_len && (!rq.body.empty() || rq.method == "POST")) head += std::format("Content-Length: {}\r\n", rq.body.size());
  head += "\r\n";
  std::size_t sent = 0;
  const std::size_t total = head.size() + rq.body.size();
  sock.send_all(head, {}, sent, total);
  sock.send_all(rq.body, progress, sent, total);

  std::string raw;
  char buf[16 * 1024];
  std::size_t header_end = std::string::npos;
  std::optional<std::size_t> content_length;
  HttpResponse resp;
  bool chunked = false;
  for (;;) {
    const std::size_t n = sock.read_some(buf, sizeof buf);
    if (n == 0) break;
    raw.append(buf, n);
    if (header_end == std::string::npos) {
      header_end = raw.find("\r\n\r\n");
      if (header_end != std::string::npos) {
        // строка состояния и заголовки
        std::size_t p = raw.find("\r\n");
        const std::string status = raw.substr(0, p);
        if (!status.starts_with("HTTP/")) throw NetError("Это не HTTP-ответ: по этому адресу не принтер с Moonraker?");
        const std::size_t sp1 = status.find(' ');
        if (sp1 == std::string::npos) throw NetError("Принтер прислал непонятный ответ.");
        resp.status = std::atoi(status.c_str() + sp1 + 1);
        const std::size_t sp2 = status.find(' ', sp1 + 1);
        resp.reason = sp2 == std::string::npos ? std::string() : status.substr(sp2 + 1);
        p += 2;
        while (p < header_end) {
          const std::size_t e = raw.find("\r\n", p);
          const std::string line = raw.substr(p, e - p);
          const std::size_t c = line.find(':');
          if (c != std::string::npos) resp.headers.push_back({trim(line.substr(0, c)), trim(line.substr(c + 1))});
          p = e + 2;
        }
        if (const auto cl = resp.header("Content-Length")) content_length = std::stoul(*cl);
        if (const auto te = resp.header("Transfer-Encoding")) chunked = lower(*te).find("chunked") != std::string::npos;
      }
    }
    if (header_end != std::string::npos) {
      const std::size_t have = raw.size() - (header_end + 4);
      if (content_length && have >= *content_length) break;
      if (chunked && raw.ends_with("0\r\n\r\n")) break;
    }
  }
  if (header_end == std::string::npos) throw NetError("Принтер закрыл соединение, не ответив.");
  std::string body = raw.substr(header_end + 4);
  if (chunked)
    resp.body = dechunk(body);
  else if (content_length)
    resp.body = body.substr(0, *content_length);
  else
    resp.body = std::move(body);
  return resp;
}

std::optional<Address> parse_address(std::string_view text) {
  std::string t = trim(text);
  if (t.starts_with("http://")) t = t.substr(7);
  if (t.starts_with("https://")) return std::nullopt;  // TLS не поддерживаем
  if (const auto slash = t.find('/'); slash != std::string::npos) t = t.substr(0, slash);
  if (t.empty()) return std::nullopt;
  Address a;
  if (t.front() == '[') {  // [::1]:7125
    const auto close = t.find(']');
    if (close == std::string::npos) return std::nullopt;
    a.host = t.substr(1, close - 1);
    if (close + 1 < t.size()) {
      if (t[close + 1] != ':') return std::nullopt;
      t = t.substr(close + 2);
    } else {
      t.clear();
    }
  } else if (std::count(t.begin(), t.end(), ':') == 1) {
    const auto c = t.find(':');
    a.host = t.substr(0, c);
    t = t.substr(c + 1);
  } else {
    a.host = t;
    t.clear();
  }
  if (!t.empty()) {
    for (char c : t)
      if (!std::isdigit(static_cast<unsigned char>(c))) return std::nullopt;
    const int port = std::atoi(t.c_str());
    if (port <= 0 || port > 65535) return std::nullopt;
    a.port = port;
  }
  for (char c : a.host)
    if (std::isspace(static_cast<unsigned char>(c))) return std::nullopt;
  if (a.host.empty()) return std::nullopt;
  return a;
}

Multipart multipart(const Headers& fields, const std::string& file_field, const std::string& filename, std::string_view data) {
  Multipart m;
  std::mt19937_64 rng(static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count()));
  m.boundary = std::format("----KikaBoundary{:016x}", rng());
  for (const auto& [k, v] : fields)
    m.body += std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"\r\n\r\n{}\r\n", m.boundary, k, v);
  m.body += std::format("--{}\r\nContent-Disposition: form-data; name=\"{}\"; filename=\"{}\"\r\n"
                        "Content-Type: application/octet-stream\r\n\r\n",
                        m.boundary, file_field, filename);
  m.body.append(data);
  m.body += std::format("\r\n--{}--\r\n", m.boundary);
  return m;
}

std::optional<std::string> local_ipv4() {
  ensure_started();
  // UDP-«соединение» ничего не отправляет: система только выбирает маршрут и свой адрес
  socket_t s = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (s == kBad) return std::nullopt;
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(53);
  inet_pton(AF_INET, "192.0.2.1", &to.sin_addr);  // адрес для документации: пакеты не уходят
  std::optional<std::string> out;
  if (::connect(s, reinterpret_cast<const sockaddr*>(&to), static_cast<socklen_t>(sizeof to)) == 0) {
    sockaddr_in me{};
    socklen_t len = static_cast<socklen_t>(sizeof me);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&me), &len) == 0) {
      char buf[INET_ADDRSTRLEN] = {};
      if (inet_ntop(AF_INET, &me.sin_addr, buf, sizeof buf) && std::strcmp(buf, "0.0.0.0") != 0) out = buf;
    }
  }
  close_socket(s);
  return out;
}

std::string url_encode(std::string_view s) {
  std::string out;
  for (unsigned char c : s) {
    if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
      out.push_back(static_cast<char>(c));
    else
      out += std::format("%{:02X}", c);
  }
  return out;
}

}  // namespace kika::printer
