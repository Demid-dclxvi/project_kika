#pragma once
// Простой HTTP/1.1-клиент поверх сокетов (Windows — Winsock, остальные — POSIX): принтер в
// локальной сети, без TLS. Одно соединение на запрос («Connection: close»), тайм-ауты на каждую
// операцию, ответ — с Content-Length, по частям (chunked) или до закрытия соединения.

#include <cstddef>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace kika::printer {

// Ошибка сети или протокола, текст — по-русски для человека.
class NetError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

using Headers = std::vector<std::pair<std::string, std::string>>;

struct HttpRequest {
  std::string method = "GET";
  std::string target = "/";  // путь со строкой запроса
  Headers headers;
  std::string body;
};

struct HttpResponse {
  int status = 0;
  std::string reason;
  Headers headers;
  std::string body;
  std::optional<std::string> header(std::string_view name) const;  // без учёта регистра
};

// Ход отправки тела: отправлено, всего. Вернуть false — прервать (NetError «отменено»).
using SendProgress = std::function<bool(std::size_t sent, std::size_t total)>;

// Запрос к host:port. timeout_ms — на соединение и на каждое ожидание данных.
HttpResponse http_request(const std::string& host, int port, const HttpRequest& rq, int timeout_ms,
                          const SendProgress& progress = {});

// Адрес принтера из строки: «192.168.1.50», «q2.local:7125», «http://192.168.1.50:7125/».
struct Address {
  std::string host;
  std::optional<int> port;  // не указан — пробовать обычные порты
};
std::optional<Address> parse_address(std::string_view text);

// Тело multipart/form-data: простые поля и один файл.
struct Multipart {
  std::string boundary;
  std::string body;
  std::string content_type() const { return "multipart/form-data; boundary=" + boundary; }
};
Multipart multipart(const Headers& fields, const std::string& file_field, const std::string& filename,
                    std::string_view data);

// Свой адрес IPv4 в локальной сети (через какой интерфейс идёт маршрут наружу); пусто — нет сети.
std::optional<std::string> local_ipv4();

// Кодирование для строки запроса (?filename=…).
std::string url_encode(std::string_view s);

}  // namespace kika::printer
