// Диалог всех настроек нарезки. Поля — по таблице настроек слайсера, без moc.

#include "settings_dialog.hpp"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QIcon>
#include <QInputDialog>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QTabWidget>

#include <algorithm>

#include "kika/analysis/job.hpp"
#include "kika/app/profile_store.hpp"
#include "field_editors.hpp"
#include "widgets.hpp"

namespace kika::gui {

namespace {

using slicer::Section;

constexpr const char* kBuiltin = "b:";
constexpr const char* kUser = "u:";
constexpr const char* kCurrent = "current";

}  // namespace

SettingsDialog::SettingsDialog(const slicer::Settings& s, Section first, QWidget* parent, bool only)
    : QDialog(parent), s_(s) {
  setWindowTitle(only ? (first == Section::Printer ? "Принтер" : first == Section::Filament ? "Пластик" : "Печать")
                      : "Настройки печати");
  resize(720, 780);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(10);
  if (only) {
    v->addWidget(build_tab(first), 1);
  } else {
    tabs_ = new QTabWidget;
    for (const Section sec : {Section::Process, Section::Filament, Section::Printer})
      tabs_->addTab(build_tab(sec), qs(std::string(slicer::section_name(sec))));
    tabs_->setCurrentIndex(first == Section::Process ? 0 : first == Section::Filament ? 1 : 2);
    v->addWidget(tabs_, 1);
  }
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
  ok_ = buttons->button(QDialogButtonBox::Ok);
  ok_->setText("Применить");
  ok_->setIcon(QIcon());
  buttons->button(QDialogButtonBox::Cancel)->setText("Отмена");
  buttons->button(QDialogButtonBox::Cancel)->setIcon(QIcon());
  connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  v->addWidget(buttons);
}

QWidget* SettingsDialog::build_tab(Section section) {
  auto* page = new QWidget;
  auto* pv = new QVBoxLayout(page);
  pv->setContentsMargins(0, 10, 0, 0);
  pv->setSpacing(8);
  // профили
  auto* row = hbox(8);
  row->addWidget(make_label("Профиль", "fieldLabel", false));
  auto* combo = new Combo;
  profiles_[section] = combo;
  row->addWidget(combo, 1);
  auto* save = make_button("Сохранить как…", "sm");
  auto* del = make_button("Удалить", "sm");
  delete_[section] = del;
  row->addWidget(save);
  row->addWidget(del);
  pv->addLayout(row);
  pv->addWidget(make_hint(section == Section::Process
                              ? "Как печатать: слои, стенки, заполнение, скорости. Скорости ограничены пределами принтера "
                                "и расходом сопла для пластика."
                          : section == Section::Filament
                              ? "Пластик: температуры, обдув, расход. Тип пластика — тот же, что материал для расчёта "
                                "прочности."
                              : "Принтер: стол, пределы движения, откат, стартовый и финальный G-code."));
  fill_profiles(section);
  connect(combo, &QComboBox::activated, this, [this, section](int) { choose_profile(section); });
  connect(save, &QPushButton::clicked, this, [this, section] { save_as(section); });
  connect(del, &QPushButton::clicked, this, [this, section] {
    const QString d = profiles_[section]->currentData().toString();
    if (!d.startsWith(kUser)) return;
    if (QMessageBox::question(this, "Удалить профиль", "Удалить профиль «" + profiles_[section]->currentText() + "»?") !=
        QMessageBox::Yes)
      return;
    app::delete_profile(to_path(d.mid(2)));
    fill_profiles(section);
  });

  // страницы раздела — вкладки, как в OrcaSlicer
  auto* pages = new QTabWidget;
  for (const auto& name : slicer::pages(section)) {
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* body = new QWidget;
    auto* bv = new QVBoxLayout(body);
    bv->setContentsMargins(4, 8, 8, 4);
    bv->setSpacing(6);
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(10);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(0, 1);
    bv->addLayout(grid);
    std::string group;
    int r = 0;
    for (const auto& f : slicer::fields()) {
      if (f.section != section || f.page != name) continue;
      if (f.group != group) {
        if (!group.empty()) grid->setRowMinimumHeight(r++, 10);
        group = f.group;
        grid->addWidget(make_label(qs(group), "itemTitle", false), r++, 0, 1, 3);
      }
      Editor ed{&f, nullptr};
      auto* w = make_field_editor(f, this, [this, &f, section] {
        for (const auto& e : editors_[section])
          if (e.field == &f) read(e);
      });
      ed.widget = w;
      r = add_field_row(grid, r, f, w);
      editors_[section].push_back(ed);
    }
    bv->addStretch();
    scroll->setWidget(body);
    pages->addTab(scroll, qs(name));
  }
  pv->addWidget(pages, 1);
  refresh(section);
  return page;
}

void SettingsDialog::fill_profiles(Section section) {
  auto* c = profiles_[section];
  const QSignalBlocker block(c);
  c->clear();
  c->addItem("Текущие настройки", kCurrent);
  switch (section) {
    case Section::Printer:
      for (const auto& p : slicer::printers()) c->addItem(qs(p.name), QString(kBuiltin) + qs(p.key));
      break;
    case Section::Filament:
      for (const auto& f : slicer::filaments()) c->addItem(qs(f.name) + " (обычный)", QString(kBuiltin) + qs(f.type));
      break;
    case Section::Process: c->addItem("Обычные", QString(kBuiltin) + "default"); break;
  }
  for (const auto& p : app::list_profiles(section)) c->addItem(qs(p.name) + " (свой)", QString(kUser) + to_qpath(p.path));
  c->setCurrentIndex(0);
  delete_[section]->setEnabled(false);
}

void SettingsDialog::choose_profile(Section section) {
  auto* c = profiles_[section];
  const QString d = c->currentData().toString();
  delete_[section]->setEnabled(d.startsWith(kUser));
  if (d == kCurrent) return;
  try {
    if (d.startsWith(kUser)) {
      app::load_profile(section, to_path(d.mid(2)), s_);
    } else {
      const std::string key = ss(d.mid(2));
      switch (section) {
        case Section::Printer:
          if (const auto* p = slicer::find_printer(key)) s_.printer = *p;
          break;
        case Section::Filament: {
          // тип — из шага «Материал»: обычный профиль другого типа меняет и тип
          s_.filament = slicer::filament_preset(key);
          break;
        }
        case Section::Process: {
          const slicer::Settings d0;
          for (const auto& f : slicer::fields())
            if (f.section == Section::Process) f.set(s_, f.get(d0));
          break;
        }
      }
    }
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "Профиль", QString::fromUtf8(e.what()));
  }
  refresh(section);
}

void SettingsDialog::save_as(Section section) {
  QString def;
  if (section == Section::Printer) def = qs(s_.printer.name);
  if (section == Section::Filament) def = qs(s_.filament.name);
  bool ok = false;
  const QString name = QInputDialog::getText(this, "Сохранить профиль", "Название профиля:", QLineEdit::Normal, def, &ok).trimmed();
  if (!ok || name.isEmpty()) return;
  try {
    app::save_profile(section, ss(name), s_);
    if (section == Section::Printer) s_.printer.name = ss(name);
    if (section == Section::Filament) s_.filament.name = ss(name);
    fill_profiles(section);
    auto* c = profiles_[section];
    for (int i = 0; i < c->count(); ++i)
      if (c->itemText(i) == name + " (свой)") {
        c->setCurrentIndex(i);
        delete_[section]->setEnabled(true);
      }
    refresh(section);
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "Профиль", QString::fromUtf8(e.what()));
  }
}

void SettingsDialog::refresh(Section section) {
  for (const auto& e : editors_[section]) {
    show_field(*e.field, e.widget, s_);
    std::erase(bad_, e.widget);
  }
  update_ok();
}

void SettingsDialog::read(const Editor& e) {
  const auto v = read_field(*e.field, e.widget);
  std::erase(bad_, e.widget);
  if (v)
    e.field->set(s_, *v);
  else
    bad_.push_back(e.widget);
  // правка — уже не тот профиль, что выбран
  if (auto* c = profiles_[e.field->section]; c && c->currentIndex() != 0) {
    const QSignalBlocker block(c);
    c->setCurrentIndex(0);
    delete_[e.field->section]->setEnabled(false);
  }
  update_ok();
}

void SettingsDialog::update_ok() {
  if (ok_) ok_->setEnabled(bad_.empty());
}

}  // namespace kika::gui
