// Окно принтера: Moonraker по сети. Без moc: сигналы — лямбдами, фоновая работа — std::thread,
// ответ в поток окна — QMetaObject::invokeMethod на приложение с проверкой, что окно живо.

#include "printer_dialog.hpp"

#include <QApplication>
#include <QComboBox>
#include <QGridLayout>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QSettings>
#include <QShowEvent>
#include <QTimer>

#include <cmath>
#include <thread>

#include "widgets.hpp"

namespace kika::gui {

namespace {

QString state_text(const std::string& st) {
  if (st == "standby") return "ожидает";
  if (st == "printing") return "печатает";
  if (st == "paused") return "пауза";
  if (st == "complete") return "печать закончена";
  if (st == "cancelled") return "печать отменена";
  if (st == "error") return "ошибка печати";
  return st.empty() ? QString("—") : qs(st);
}

QString minutes(double s) {
  const int m = static_cast<int>(std::lround(s / 60));
  return m >= 60 ? QString("%1 ч %2 мин").arg(m / 60).arg(m % 60) : QString("%1 мин").arg(std::max(0, m));
}

// Положить в очередь событий приложения (живёт дольше любого окна).
void post(std::function<void()> fn) { QMetaObject::invokeMethod(qApp, std::move(fn), Qt::QueuedConnection); }

}  // namespace

template <class R>
void PrinterDialog::background(std::function<R()> work, std::function<void(std::optional<R>, QString)> done) {
  auto alive = alive_;
  std::thread([alive, work = std::move(work), done = std::move(done)]() mutable {
    std::optional<R> r;
    QString err;
    try {
      r = work();
    } catch (const std::exception& e) {
      err = QString::fromUtf8(e.what());
    }
    post([alive, done = std::move(done), r = std::move(r), err]() mutable {
      if (*alive) done(std::move(r), err);
    });
  }).detach();
}

PrinterDialog::PrinterDialog(QWidget* parent) : QDialog(parent), alive_(std::make_shared<std::atomic<bool>>(true)) {
  setWindowTitle("Принтер");
  resize(560, 520);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(18, 16, 18, 16);
  v->setSpacing(10);

  v->addWidget(make_label("Подключение", "itemTitle", false));
  v->addWidget(make_hint("Принтеры с Klipper и Moonraker (QIDI Q2, Voron, Creality K1 и другие): адрес — на экране принтера "
                         "в настройках сети. Компьютер и принтер должны быть в одной сети."));
  QSettings st("Kika", "kika-gui");
  auto* row = hbox(8);
  address_ = new QLineEdit(st.value("printer/address").toString());
  address_->setPlaceholderText("192.168.1.50");
  btn_connect_ = make_button("Подключить", "sm");
  btn_find_ = make_button("Найти в сети", "sm");
  row->addWidget(address_, 1);
  row->addWidget(btn_connect_);
  row->addWidget(btn_find_);
  v->addLayout(row);
  found_ = new Combo;
  found_->hide();
  v->addWidget(found_);
  key_ = new QLineEdit(st.value("printer/key").toString());
  key_->setPlaceholderText("Ключ API Moonraker — только если принтер его требует");
  key_->setEchoMode(QLineEdit::Password);
  v->addWidget(key_);

  v->addSpacing(4);
  printer_label_ = make_label("Не подключено.", "itemTitle", false);
  v->addWidget(printer_label_);
  state_label_ = make_label("", nullptr, true);
  v->addWidget(state_label_);
  progress_ = new QProgressBar;
  progress_->setRange(0, 1000);
  progress_->setTextVisible(false);
  progress_->setFixedHeight(8);
  progress_->hide();
  v->addWidget(progress_);
  temps_label_ = make_hint("");
  v->addWidget(temps_label_);
  auto* ctl = hbox(8);
  btn_pause_ = make_button("Пауза", "sm");
  btn_cancel_ = make_button("Отменить печать", "sm");
  ctl->addWidget(btn_pause_);
  ctl->addWidget(btn_cancel_);
  ctl->addStretch();
  v->addLayout(ctl);

  v->addSpacing(6);
  v->addWidget(make_label("Печать", "itemTitle", false));
  source_label_ = make_hint("Нечего отправлять: откройте модель или G-code.");
  v->addWidget(source_label_);
  upload_ = new QProgressBar;
  upload_->setRange(0, 1000);
  upload_->setTextVisible(false);
  upload_->setFixedHeight(8);
  upload_->hide();
  v->addWidget(upload_);
  auto* act = hbox(8);
  btn_print_ = make_button("Отправить и печатать", "primary");
  btn_upload_ = make_button("Только отправить", "sm");
  act->addWidget(btn_print_);
  act->addWidget(btn_upload_);
  act->addStretch();
  v->addLayout(act);
  message_ = make_label("", "hint", true);
  v->addWidget(message_);
  v->addStretch();

  timer_ = new QTimer(this);
  timer_->setInterval(2500);
  connect(timer_, &QTimer::timeout, this, [this] { poll(); });
  connect(btn_connect_, &QPushButton::clicked, this, [this] { connect_printer(); });
  connect(address_, &QLineEdit::returnPressed, this, [this] { connect_printer(); });
  connect(btn_find_, &QPushButton::clicked, this, [this] { discover(); });
  connect(found_, &QComboBox::activated, this, [this](int) {
    const QString a = found_->currentData().toString();
    if (a.isEmpty()) return;
    address_->setText(a);
    connect_printer();
  });
  connect(btn_print_, &QPushButton::clicked, this, [this] { send(true, true); });
  connect(btn_upload_, &QPushButton::clicked, this, [this] { send(false, false); });
  connect(btn_pause_, &QPushButton::clicked, this, [this] {
    if (status_ && status_->state == "paused")
      command("Продолжаю печать", [](const printer::Moonraker& m) { m.resume(); });
    else
      command("Пауза", [](const printer::Moonraker& m) { m.pause(); });
  });
  connect(btn_cancel_, &QPushButton::clicked, this, [this] {
    if (QMessageBox::question(this, "Отменить печать", "Отменить печать на принтере? Продолжить её потом будет нельзя.") !=
        QMessageBox::Yes)
      return;
    command("Отменяю печать", [](const printer::Moonraker& m) { m.cancel(); });
  });
  update_buttons();
}

PrinterDialog::~PrinterDialog() { *alive_ = false; }

void PrinterDialog::showEvent(QShowEvent* e) {
  QDialog::showEvent(e);
  if (conn_) timer_->start();
}

void PrinterDialog::hideEvent(QHideEvent* e) {
  timer_->stop();
  QDialog::hideEvent(e);
}

void PrinterDialog::set_source(std::function<PrintPayload()> make, const QString& what) {
  source_ = std::move(make);
  source_label_->setText(source_ ? "Отправить: " + what : QString("Нечего отправлять: откройте модель или G-code."));
  update_buttons();
}

void PrinterDialog::set_message(const QString& text, bool error) {
  message_->setText(text);
  message_->setObjectName(error ? "warn" : "hint");
  repolish(message_);
}

void PrinterDialog::update_buttons() {
  const bool connected = conn_.has_value() && info_ && info_->ready();
  const bool printing = status_ && status_->busy();
  btn_connect_->setEnabled(busy_ == 0);
  btn_find_->setEnabled(busy_ == 0);
  btn_print_->setEnabled(connected && source_ && !sending_ && !printing && busy_ == 0);
  btn_upload_->setEnabled(conn_.has_value() && source_ && !sending_ && busy_ == 0);
  btn_pause_->setEnabled(connected && printing && busy_ == 0);
  btn_pause_->setText(status_ && status_->state == "paused" ? "Продолжить" : "Пауза");
  btn_cancel_->setEnabled(connected && printing && busy_ == 0);
}

void PrinterDialog::connect_printer(std::function<void(bool)> then) {
  const QString text = address_->text().trimmed();
  const auto addr = printer::parse_address(ss(text));
  if (!addr) {
    set_message(text.isEmpty() ? "Введите адрес принтера." : "Непонятный адрес: нужно 192.168.1.50 или имя:порт.", true);
    if (then) then(false);
    return;
  }
  const std::string key = ss(key_->text().trimmed());
  ++busy_;
  set_message("Подключаюсь…");
  update_buttons();
  background<printer::PrinterInfo>(
      [addr, key]() -> printer::PrinterInfo {
        const auto pi = printer::probe(addr->host, addr->port, 3000);
        if (!pi)
          throw printer::NetError("По этому адресу не отвечает Moonraker" +
                                  std::string(addr->port ? "" : " (пробовал порты 7125 и 80)") +
                                  ". Проверьте, что принтер включён и в той же сети.");
        return *pi;
      },
      [this, key, text, then](std::optional<printer::PrinterInfo> pi, QString err) {
        --busy_;
        if (!pi) {
          conn_.reset();
          info_.reset();
          printer_label_->setText("Не подключено.");
          set_message(err, true);
          update_buttons();
          if (then) then(false);
          return;
        }
        printer::Connection c;
        c.host = pi->host;
        c.port = pi->port;
        c.api_key = key;
        c.timeout_ms = 8000;
        conn_ = c;
        info_ = *pi;
        QSettings st("Kika", "kika-gui");
        st.setValue("printer/address", text);
        st.setValue("printer/key", key_->text().trimmed());
        printer_label_->setText(QString("%1 · %2:%3").arg(qs(pi->hostname.empty() ? pi->host : pi->hostname), qs(pi->host)).arg(pi->port));
        if (!pi->ready()) {
          state_label_->setText("Klipper не готов: " + qs(pi->klippy_state) +
                                (pi->state_message.empty() ? QString() : " — " + qs(pi->state_message)));
          set_message("Файл можно отправить, печать — когда принтер будет готов.");
        } else {
          set_message(QString("Подключено. Klipper %1.").arg(qs(pi->klipper_version)));
        }
        update_buttons();
        if (isVisible()) timer_->start();
        poll();
        if (then) then(true);
      });
}

void PrinterDialog::discover() {
  ++busy_;
  set_message("Ищу принтеры в сети — до полуминуты…");
  update_buttons();
  background<std::vector<printer::PrinterInfo>>(
      [] { return printer::discover(); },
      [this](std::optional<std::vector<printer::PrinterInfo>> found, QString err) {
        --busy_;
        found_->clear();
        if (!found || found->empty()) {
          found_->hide();
          set_message(err.isEmpty() ? "Принтеров с Moonraker в этой сети не нашёл. Введите адрес с экрана принтера." : err,
                      true);
        } else {
          found_->addItem(QString("Найдено: %1 — выберите").arg(found->size()), QString());
          for (const auto& pi : *found)
            found_->addItem(QString("%1 — %2:%3").arg(qs(pi.hostname.empty() ? pi.host : pi.hostname), qs(pi.host)).arg(pi.port),
                            QString("%1:%2").arg(qs(pi.host)).arg(pi.port));
          found_->show();
          set_message("");
          if (found->size() == 1) {
            address_->setText(found_->itemData(1).toString());
            connect_printer();
          }
        }
        update_buttons();
      });
}

void PrinterDialog::poll() {
  if (!conn_ || polling_ || !info_ || !info_->ready()) return;
  polling_ = true;
  const auto c = *conn_;
  background<printer::PrinterStatus>([c] { return printer::Moonraker(c).status(); },
                                     [this](std::optional<printer::PrinterStatus> st, QString err) {
                                       polling_ = false;
                                       if (!st) {
                                         state_label_->setText("Нет связи с принтером: " + err);
                                         return;
                                       }
                                       show_status(*st);
                                     });
}

void PrinterDialog::show_status(const printer::PrinterStatus& st) {
  status_ = st;
  QString s = state_text(st.state);
  if (st.busy() || st.state == "complete") {
    s += " · " + qs(st.filename);
    if (st.busy()) s += QString(" · %1 %").arg(fmtq(st.progress * 100, 0));
    if (st.total_layers > 0) s += QString(" · слой %1 из %2").arg(st.layer).arg(st.total_layers);
    if (st.busy() && st.progress > 0.01 && st.print_duration > 0)
      s += " · осталось ≈ " + minutes(st.print_duration / st.progress - st.print_duration);
  }
  if (!st.message.empty()) s += "\n" + qs(st.message);
  state_label_->setText(s);
  progress_->setVisible(st.busy());
  progress_->setValue(static_cast<int>(std::lround(st.progress * 1000)));
  QString t = QString("Сопло %1 / %2 °C · стол %3 / %4 °C").arg(fmtq(st.nozzle, 0), fmtq(st.nozzle_target, 0), fmtq(st.bed, 0),
                                                              fmtq(st.bed_target, 0));
  if (st.chamber) t += QString(" · камера %1 / %2 °C").arg(fmtq(*st.chamber, 0), fmtq(st.chamber_target.value_or(0), 0));
  temps_label_->setText(t);
  update_buttons();
}

void PrinterDialog::command(const QString& what, std::function<void(const printer::Moonraker&)> fn) {
  if (!conn_) return;
  ++busy_;
  set_message(what + "…");
  update_buttons();
  const auto c = *conn_;
  background<bool>(
      [c, fn] {
        fn(printer::Moonraker(c));
        return true;
      },
      [this, what](std::optional<bool> ok, QString err) {
        --busy_;
        set_message(ok ? what + " — принтер принял команду." : err, !ok);
        update_buttons();
        poll();
      });
}

void PrinterDialog::send(bool start, bool confirm, std::function<void(bool)> then) {
  if (!conn_ || !source_ || sending_) {
    if (then) then(false);
    return;
  }
  sending_ = true;
  update_buttons();
  set_message("Готовлю G-code…");
  upload_->setValue(0);
  upload_->show();
  // 1. G-code (может нарезать модель) — в фоне; 2. подтверждение; 3. отправка — в фоне
  auto make = source_;
  background<PrintPayload>(
      [make] { return make(); },
      [this, start, confirm, then](std::optional<PrintPayload> pl, QString err) {
        if (!pl) {
          sending_ = false;
          upload_->hide();
          set_message(err, true);
          update_buttons();
          if (then) then(false);
          return;
        }
        const std::string file = printer::printer_filename(pl->name);
        const QString who = qs(info_ && !info_->hostname.empty() ? info_->hostname : conn_->host);
        if (start && confirm) {
          QString text = QString("Отправить «%1» на %2 и начать печать?\n\n%3").arg(qs(file), who, pl->summary);
          if (!pl->for_klipper)
            text += "\n\n⚠ G-code нарезан для «" + pl->printer_name +
                    "» (не Klipper): стартовый код может не подойти этому принтеру. Выберите профиль своего принтера в "
                    "настройках печати.";
          text += "\n\nПеред печатью проверьте, что стол чист, а в принтер заправлен нужный пруток.";
          if (QMessageBox::question(this, "Начать печать", text) != QMessageBox::Yes) {
            sending_ = false;
            upload_->hide();
            set_message("Печать не начата.");
            update_buttons();
            if (then) then(false);
            return;
          }
        }
        set_message("Отправляю " + qs(file) + "…");
        const auto c = *conn_;
        auto alive = alive_;
        auto bar = QPointer<QProgressBar>(upload_);
        auto last = std::make_shared<int>(-1);
        const bool run = start && info_ && info_->ready() && !(status_ && status_->busy());
        background<printer::UploadResult>(
            [c, file, gcode = std::move(pl->gcode), run, alive, bar, last] {
              return printer::Moonraker(c).upload(file, gcode, run, [alive, bar, last](std::size_t sent, std::size_t total) {
                const int k = static_cast<int>(1000.0 * static_cast<double>(sent) / static_cast<double>(std::max<std::size_t>(total, 1)));
                if (k - *last >= 10 || k == 1000) {
                  *last = k;
                  post([alive, bar, k] {
                    if (*alive && bar) bar->setValue(k);
                  });
                }
                return static_cast<bool>(*alive);
              });
            },
            [this, start, run, then](std::optional<printer::UploadResult> r, QString err2) {
              sending_ = false;
              upload_->hide();
              if (!r) {
                set_message(err2, true);
              } else if (r->print_started) {
                set_message("Печать запущена: «" + qs(r->path) +
                            "». Принтер сначала выровняет стол, снимет карту и прогреется — это несколько минут.");
              } else if (start && !run) {
                set_message("Файл «" + qs(r->path) + "» на принтере; печать не запущена — принтер занят или не готов.", true);
              } else if (start) {
                set_message("Файл «" + qs(r->path) + "» на принтере, но печать не запустилась — запустите её на экране принтера.",
                            true);
              } else {
                set_message("Файл «" + qs(r->path) + "» на принтере — его можно запустить с экрана принтера.");
              }
              update_buttons();
              poll();
              if (then) then(r.has_value());
            });
      });
}

void PrinterDialog::run_script(const QString& address, bool do_send, bool start, std::function<void(bool)> done) {
  address_->setText(address);
  connect_printer([this, do_send, start, done](bool ok) {
    if (!ok || !do_send) {
      // дождаться первого опроса состояния, чтобы снимок был с ним
      QTimer::singleShot(600, this, [done, ok] { done(ok); });
      return;
    }
    send(start, false, [this, done](bool sent) { QTimer::singleShot(600, this, [done, sent] { done(sent); }); });
  });
}

}  // namespace kika::gui
