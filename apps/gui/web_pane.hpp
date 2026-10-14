#pragma once
// Веб-интерфейс принтера (Fluidd, Mainsail) во вкладке «Принтер». С Qt WebEngine — страница прямо
// в окне, над ней — адрес, «Назад», «Обновить», «В браузере»; без модуля (сборка без него) —
// адрес и кнопка «Открыть в браузере». Страница не открылась — пояснение и «Повторить».

#include <QString>
#include <QUrl>
#include <QWidget>

class QFrame;
class QLabel;
class QPushButton;
class QStackedLayout;

namespace kika::gui {

class WebPane : public QWidget {
 public:
  explicit WebPane(QWidget* parent = nullptr);
  // Страница принтера; пусто — принтер не подключён.
  void set_url(const QString& url);
  QString url() const { return url_; }
  // Есть ли в сборке встроенный браузер.
  static bool embedded();
  // Для снимка без человека: виджет со страницей (или заглушка).
  QWidget* content() const;
  // Страница загружена (для сценария проверки): ok — без ошибки.
  bool loaded() const { return loaded_; }

 private:
  void show_placeholder(const QString& error = {});
  void load();

  QString url_;
  bool loaded_ = false;
  QFrame* bar_ = nullptr;
  QLabel* address_ = nullptr;
  QPushButton* back_ = nullptr;
  QPushButton* reload_ = nullptr;
  QPushButton* external_ = nullptr;
  QStackedLayout* stack_ = nullptr;
  QWidget* placeholder_ = nullptr;
  QLabel* text_ = nullptr;
  QPushButton* open_ = nullptr;
  QPushButton* retry_ = nullptr;
  QWidget* view_ = nullptr;  // QWebEngineView, создаётся при первом адресе
};

}  // namespace kika::gui
