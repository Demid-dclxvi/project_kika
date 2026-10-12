#pragma once
// Принтер с Klipper и Moonraker (QIDI Q2, Voron, Creality K1 и другие) по сети: сведения,
// состояние, отправка G-code и запуск печати, пауза, продолжение, отмена.
//
// Moonraker слушает порт 7125; у многих принтеров тот же API доступен и через веб-интерфейс
// (Fluidd, Mainsail) на порту 80. В локальной сети ключ обычно не нужен (trusted_clients),
// иначе — ключ API (X-Api-Key) из настроек Moonraker.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "kika/printer/http.hpp"
#include "kika/util/json.hpp"

namespace kika::printer {

constexpr int kMoonrakerPort = 7125;

struct Connection {
  std::string host;
  int port = kMoonrakerPort;
  std::string api_key;
  int timeout_ms = 5000;
};

// Что за принтер: из /server/info и /printer/info.
struct PrinterInfo {
  std::string host;
  int port = kMoonrakerPort;
  std::string hostname;           // имя в сети
  std::string klippy_state;       // ready, startup, shutdown, error, disconnected
  std::string state_message;      // пояснение Klipper (почему не готов)
  std::string klipper_version, moonraker_version;
  bool ready() const { return klippy_state == "ready"; }
};

// Состояние печати и температуры.
struct PrinterStatus {
  std::string state;     // standby, printing, paused, complete, cancelled, error
  std::string filename;  // что печатается (или напечатано последним)
  std::string message;   // сообщение Klipper (ошибка печати и т. п.)
  double progress = 0;   // 0…1 по файлу
  double print_duration = 0, total_duration = 0;  // с
  int layer = 0, total_layers = 0;                // если G-code сообщает слои
  double nozzle = 0, nozzle_target = 0, bed = 0, bed_target = 0;
  std::optional<double> chamber, chamber_target;
  bool busy() const { return state == "printing" || state == "paused"; }
};

struct UploadResult {
  std::string path;     // имя файла на принтере (в папке gcodes)
  bool print_started = false;
  bool print_queued = false;
};

class Moonraker {
 public:
  explicit Moonraker(Connection c) : c_(std::move(c)) {}
  const Connection& connection() const { return c_; }

  PrinterInfo info() const;
  PrinterStatus status() const;
  // Отправить G-code в папку gcodes; start — сразу печатать. Имя файла на принтере — filename.
  UploadResult upload(const std::string& filename, const std::string& gcode, bool start, const SendProgress& progress = {}) const;
  void start(const std::string& filename) const;
  void pause() const;
  void resume() const;
  void cancel() const;

  // GET/POST к API: ответ "result" из JSON; ошибка Moonraker — NetError с её текстом.
  json::Value get(const std::string& target) const;
  json::Value post(const std::string& target, const std::string& body = {}, const std::string& content_type = {},
                   const SendProgress& progress = {}, int timeout_ms = 0) const;

 private:
  Connection c_;
};

// Есть ли по адресу Moonraker (порт не указан — пробуются 7125 и 80). Пусто — нет.
std::optional<PrinterInfo> probe(const std::string& host, std::optional<int> port, int timeout_ms = 1500);

// Найти принтеры в своей сети /24 (адреса с .1 по .254, параллельно). progress(доля) — ход.
std::vector<PrinterInfo> discover(int timeout_ms = 700, const std::function<void(double)>& progress = {});

// Имя файла для принтера: без символов, запрещённых в именах файлов, кириллица — латиницей
// (экраны принтеров часто её не показывают), окончание .gcode.
std::string printer_filename(std::string_view name);

}  // namespace kika::printer
