#pragma once
// Панель настроек печати слева (как в OrcaSlicer): принтер, пластик и профиль печати сверху,
// под ними — страницы «Качество», «Прочность», «Скорость», «Поддержки», «Прочее» с полями
// по таблице slicer::fields(). Внизу — «Нарезать». Принтер и пластик правятся в отдельном окне (⚙).

#include <QFrame>
#include <QString>

#include <functional>
#include <string>
#include <vector>

#include "kika/slicer/profiles.hpp"
#include "kika/slicer/slicer.hpp"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QScrollArea;
class QStackedWidget;
class QWidget;

namespace kika::gui {

class SettingsPanel : public QFrame {
 public:
  explicit SettingsPanel(QWidget* parent = nullptr);

  // Показать настройки (без on_changed).
  void set_settings(const slicer::Settings& s);
  const slicer::Settings& settings() const { return s_; }
  // Человек что-то поменял: settings() уже новые.
  std::function<void()> on_changed;
  std::function<void()> on_slice;

  void set_slice_button(bool enabled, const QString& text, bool primary);
  void set_status(const QString& text);
  // Настройки не применяются (открыт готовый G-code): why — пояснение; пусто — снять.
  void set_locked(const QString& why);
  void select_page(const std::string& name);
  void set_collapsed(bool on);
  bool collapsed() const { return collapsed_; }
  // Окно настроек принтера или пластика (⚙).
  void edit(slicer::Section section);

 private:
  struct Editor {
    const slicer::Field* field;
    QWidget* widget;
    std::vector<QWidget*> parts;  // вся строка — чтобы прятать тонкие настройки
  };
  QWidget* build_page(const std::string& name);
  void fill_printers();
  void fill_filaments();
  void fill_processes();
  void refresh();
  void apply_advanced();
  void changed();
  void choose(slicer::Section section, QComboBox* combo);
  void save_process();

  slicer::Settings s_;
  std::vector<Editor> editors_;
  bool collapsed_ = false;
  bool advanced_ = false;
  QWidget* body_ = nullptr;
  QWidget* strip_ = nullptr;  // свёрнутая панель
  QLabel* lock_note_ = nullptr;
  QWidget* controls_ = nullptr;
  QComboBox* printer_ = nullptr;
  QComboBox* filament_ = nullptr;
  QComboBox* process_ = nullptr;
  std::vector<QPushButton*> page_buttons_;
  QStackedWidget* pages_ = nullptr;
  QCheckBox* advanced_box_ = nullptr;
  QPushButton* slice_ = nullptr;
  QLabel* status_ = nullptr;
};

}  // namespace kika::gui
