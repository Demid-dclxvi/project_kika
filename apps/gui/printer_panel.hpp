#pragma once
// Принтер по сети (Klipper, Moonraker) — панель вкладки «Принтер»: адрес и поиск в сети,
// состояние и температуры, отправка G-code с запуском печати, пауза, продолжение, отмена; адрес
// веб-интерфейса принтера (Fluidd, Mainsail) для вида справа. Сеть — в фоновых потоках, результат
// возвращается в окно через очередь событий.

#include <QString>
#include <QWidget>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "kika/printer/moonraker.hpp"

class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QTimer;
class QComboBox;

namespace kika::gui {

// Что отправлять: имя файла на принтере, G-code и пояснение для подтверждения.
struct PrintPayload {
  std::string name;
  std::string gcode;
  QString summary;          // «Уголок · PETG, сопло 245 °C, стол 70 °C · ≈ 33 мин, 8 г»
  bool for_klipper = true;  // нарезан для Klipper (иначе — предупредить)
  QString printer_name;     // для какого принтера нарезан
};

class PrinterPanel : public QWidget {
 public:
  explicit PrinterPanel(QWidget* parent = nullptr);
  ~PrinterPanel() override;

  // Источник G-code: вызывается в фоновом потоке. what — для подписи.
  void set_source(std::function<PrintPayload()> make, const QString& what);
  // Отправить (start — и начать печать, с подтверждением). Не подключено — сначала подключиться
  // по сохранённому адресу. then(успех).
  void send_now(bool start, std::function<void(bool)> then = {});
  bool connected() const { return conn_.has_value(); }
  bool has_address() const;
  // Подключиться по адресу из поля (если ещё нет).
  void connect_saved();
  // Веб-интерфейс принтера: адрес из поля или по адресу Moonraker; пусто — не подключено.
  QString web_url() const;
  std::function<void(const QString& url)> on_web;
  // Для сценария проверки: подключиться, отправить (start — с запуском), затем done(успех).
  void run_script(const QString& address, bool send, bool start, std::function<void(bool)> done);

 protected:
  void showEvent(QShowEvent* e) override;
  void hideEvent(QHideEvent* e) override;

 private:
  void connect_printer(std::function<void(bool)> then = {});
  void discover();
  void poll();
  void send(bool start, bool confirm, std::function<void(bool)> then = {});
  void command(const QString& what, std::function<void(const printer::Moonraker&)> fn);
  void show_status(const printer::PrinterStatus& st);
  void set_message(const QString& text, bool error = false);
  void update_buttons();
  // Выполнить work в фоновом потоке, затем done в потоке окна (если окно ещё живо).
  template <class R>
  void background(std::function<R()> work, std::function<void(std::optional<R>, QString)> done);

  QLineEdit* address_ = nullptr;
  QLineEdit* key_ = nullptr;
  QLineEdit* web_ = nullptr;
  QComboBox* found_ = nullptr;
  QPushButton* btn_connect_ = nullptr;
  QPushButton* btn_find_ = nullptr;
  QLabel* printer_label_ = nullptr;
  QLabel* state_label_ = nullptr;
  QLabel* temps_label_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* source_label_ = nullptr;
  QProgressBar* upload_ = nullptr;
  QPushButton* btn_print_ = nullptr;
  QPushButton* btn_upload_ = nullptr;
  QPushButton* btn_pause_ = nullptr;
  QPushButton* btn_cancel_ = nullptr;
  QLabel* message_ = nullptr;
  QTimer* timer_ = nullptr;

  std::shared_ptr<std::atomic<bool>> alive_;
  std::optional<printer::Connection> conn_;
  std::optional<printer::PrinterInfo> info_;
  std::optional<printer::PrinterStatus> status_;
  std::function<PrintPayload()> source_;
  int busy_ = 0;          // операций в работе (подключение, отправка, команды)
  bool polling_ = false;  // опрос состояния идёт
  bool sending_ = false;
};

}  // namespace kika::gui
