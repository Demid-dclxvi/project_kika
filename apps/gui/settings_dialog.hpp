#pragma once
// Все настройки нарезки: вкладки «Печать», «Пластик», «Принтер». Поля строятся по таблице
// slicer::fields(), свои профили — app::profile_store (папка настроек программы).

#include <QDialog>

#include <map>
#include <vector>

#include "kika/slicer/profiles.hpp"
#include "kika/slicer/slicer.hpp"

class QComboBox;
class QPushButton;
class QTabWidget;
class QWidget;

namespace kika::gui {

class SettingsDialog : public QDialog {
 public:
  SettingsDialog(const slicer::Settings& s, slicer::Section first, QWidget* parent = nullptr);
  const slicer::Settings& settings() const { return s_; }

 private:
  struct Editor {
    const slicer::Field* field;
    QWidget* widget;
  };
  QWidget* build_tab(slicer::Section section);
  void fill_profiles(slicer::Section section);
  void choose_profile(slicer::Section section);
  void save_as(slicer::Section section);
  void refresh(slicer::Section section);
  void read(const Editor& e);
  void update_ok();

  slicer::Settings s_;
  std::map<slicer::Section, std::vector<Editor>> editors_;
  std::map<slicer::Section, QComboBox*> profiles_;
  std::map<slicer::Section, QPushButton*> delete_;
  std::vector<QWidget*> bad_;
  QPushButton* ok_ = nullptr;
  QTabWidget* tabs_ = nullptr;
};

}  // namespace kika::gui
