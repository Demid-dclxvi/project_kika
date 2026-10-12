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
#include "widgets.hpp"

namespace kika::gui {

namespace {

using slicer::FieldKind;
using slicer::Section;

constexpr const char* kBuiltin = "b:";
constexpr const char* kUser = "u:";
constexpr const char* kCurrent = "current";

}  // namespace

SettingsDialog::SettingsDialog(const slicer::Settings& s, Section first, QWidget* parent) : QDialog(parent), s_(s) {
  setWindowTitle("Настройки печати");
  resize(700, 760);
  auto* v = new QVBoxLayout(this);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(10);
  tabs_ = new QTabWidget;
  for (const Section sec : {Section::Process, Section::Filament, Section::Printer})
    tabs_->addTab(build_tab(sec), qs(std::string(slicer::section_name(sec))));
  tabs_->setCurrentIndex(first == Section::Process ? 0 : first == Section::Filament ? 1 : 2);
  v->addWidget(tabs_, 1);
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
                              ? "Пластик: температуры, обдув, расход. Тип пластика — из шага «Материал»; здесь — как его "
                                "печатать."
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

  auto* scroll = new QScrollArea;
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  auto* body = new QWidget;
  auto* bv = new QVBoxLayout(body);
  bv->setContentsMargins(0, 4, 8, 4);
  bv->setSpacing(6);
  // одна сетка на вкладку: поля всех разделов — в одном столбце
  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(10);
  grid->setVerticalSpacing(6);
  grid->setColumnStretch(3, 1);
  bv->addLayout(grid);
  std::string group;
  int r = 0;
  for (const auto& f : slicer::fields()) {
    if (f.section != section) continue;
    if (f.group != group) {
      if (!group.empty()) grid->setRowMinimumHeight(r++, 10);
      group = f.group;
      grid->addWidget(make_label(qs(group), "itemTitle", false), r++, 0, 1, 4);
    }
    QWidget* w = nullptr;
    const QString tip = qs(f.tip);
    switch (f.kind) {
      case FieldKind::Number:
      case FieldKind::Integer: {
        auto* e = make_num(std::nullopt);
        e->setFixedWidth(110);
        connect(e, &QLineEdit::textEdited, this, [this, &f, e](const QString&) { read({&f, e}); });
        w = e;
        break;
      }
      case FieldKind::Bool: {
        auto* c = new QCheckBox(qs(f.label));
        connect(c, &QCheckBox::toggled, this, [this, &f, c](bool) { read({&f, c}); });
        w = c;
        break;
      }
      case FieldKind::Choice: {
        auto* c = new Combo;
        for (const auto& ch : f.choices) c->addItem(qs(ch.name), qs(ch.key));
        connect(c, &QComboBox::currentIndexChanged, this, [this, &f, c](int) { read({&f, c}); });
        w = c;
        break;
      }
      case FieldKind::Text: {
        auto* e = new QLineEdit;
        connect(e, &QLineEdit::textEdited, this, [this, &f, e](const QString&) { read({&f, e}); });
        w = e;
        break;
      }
      case FieldKind::Code: {
        auto* e = new QPlainTextEdit;
        e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        e->setMinimumHeight(150);
        e->setLineWrapMode(QPlainTextEdit::NoWrap);
        connect(e, &QPlainTextEdit::textChanged, this, [this, &f, e] { read({&f, e}); });
        w = e;
        break;
      }
    }
    w->setToolTip(tip);
    if (f.kind == FieldKind::Bool) {
      grid->addWidget(w, r, 0, 1, 4);
    } else if (f.kind == FieldKind::Code) {
      auto* l = make_label(qs(f.label), "fieldLabel", false);
      l->setToolTip(tip);
      grid->addWidget(l, r++, 0, 1, 4);
      grid->addWidget(w, r, 0, 1, 4);
      if (!f.tip.empty()) grid->addWidget(make_hint(tip), ++r, 0, 1, 4);
    } else {
      auto* l = make_label(qs(f.label), nullptr, false);
      l->setToolTip(tip);
      grid->addWidget(l, r, 0);
      grid->addWidget(w, r, 1);
      if (!f.unit.empty()) grid->addWidget(make_label(qs(f.unit), "hint", false), r, 2);
      if (f.kind == FieldKind::Text || f.kind == FieldKind::Choice) w->setMinimumWidth(220);
    }
    ++r;
    editors_[section].push_back({&f, w});
  }
  bv->addStretch();
  scroll->setWidget(body);
  pv->addWidget(scroll, 1);
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
    const QSignalBlocker block(e.widget);
    const auto v = e.field->get(s_);
    switch (e.field->kind) {
      case FieldKind::Number:
      case FieldKind::Integer: static_cast<QLineEdit*>(e.widget)->setText(num_text(v.as_double())); break;
      case FieldKind::Bool: static_cast<QCheckBox*>(e.widget)->setChecked(v.as_bool()); break;
      case FieldKind::Choice: {
        auto* c = static_cast<QComboBox*>(e.widget);
        const int i = c->findData(qs(v.as_string()));
        c->setCurrentIndex(i >= 0 ? i : 0);
        break;
      }
      case FieldKind::Text: static_cast<QLineEdit*>(e.widget)->setText(qs(v.as_string())); break;
      case FieldKind::Code: static_cast<QPlainTextEdit*>(e.widget)->setPlainText(qs(v.as_string())); break;
    }
    set_bad(e.widget, false);
    std::erase(bad_, e.widget);
  }
  update_ok();
}

void SettingsDialog::read(const Editor& e) {
  json::Value v;
  switch (e.field->kind) {
    case FieldKind::Number:
    case FieldKind::Integer: {
      const auto x = parse_num(static_cast<QLineEdit*>(e.widget)->text());
      v = x ? json::Value(*x) : json::Value(std::string());
      break;
    }
    case FieldKind::Bool: v = static_cast<QCheckBox*>(e.widget)->isChecked(); break;
    case FieldKind::Choice: v = ss(static_cast<QComboBox*>(e.widget)->currentData().toString()); break;
    case FieldKind::Text: v = ss(static_cast<QLineEdit*>(e.widget)->text()); break;
    case FieldKind::Code: v = ss(static_cast<QPlainTextEdit*>(e.widget)->toPlainText()); break;
  }
  const bool ok = slicer::check_field(*e.field, v, "").empty();
  set_bad(e.widget, !ok);
  std::erase(bad_, e.widget);
  if (ok)
    e.field->set(s_, v);
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
