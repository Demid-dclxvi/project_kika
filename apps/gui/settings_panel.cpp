// Панель настроек печати слева. Без moc: сигналы — лямбдами.

#include "settings_panel.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QInputDialog>
#include <QMessageBox>
#include <QScrollArea>
#include <QSettings>
#include <QStackedWidget>

#include "field_editors.hpp"
#include "kika/app/profile_store.hpp"
#include "settings_dialog.hpp"
#include "widgets.hpp"

namespace kika::gui {

namespace {

using slicer::Section;

std::string section_json(const slicer::Settings& s, Section sec) { return json::dump(slicer::section_to_json(s, sec)); }

}  // namespace

SettingsPanel::SettingsPanel(QWidget* parent) : QFrame(parent) {
  setObjectName("settingsPanel");
  advanced_ = QSettings("Kika", "kika-gui").value("settings/advanced", false).toBool();
  auto* outer = new QHBoxLayout(this);
  outer->setContentsMargins(0, 0, 0, 0);
  outer->setSpacing(0);

  // свёрнутая панель: узкая полоса с кнопкой
  strip_ = new QWidget;
  {
    auto* sv = new QVBoxLayout(strip_);
    sv->setContentsMargins(4, 10, 4, 10);
    auto* open = make_icon_button("»", "Показать настройки печати");
    connect(open, &QPushButton::clicked, this, [this] { set_collapsed(false); });
    sv->addWidget(open);
    sv->addStretch();
  }
  strip_->hide();
  outer->addWidget(strip_);

  body_ = new QWidget;
  outer->addWidget(body_, 1);
  auto* v = new QVBoxLayout(body_);
  v->setContentsMargins(14, 10, 10, 12);
  v->setSpacing(8);
  {
    auto* head = hbox(6);
    head->addWidget(make_label("Настройки печати", "stepTitle", false), 1);
    auto* hide = make_icon_button("«", "Свернуть панель");
    connect(hide, &QPushButton::clicked, this, [this] { set_collapsed(true); });
    head->addWidget(hide);
    v->addLayout(head);
  }
  lock_note_ = make_hint("");
  lock_note_->hide();
  v->addWidget(lock_note_);

  controls_ = new QWidget;
  auto* cv = new QVBoxLayout(controls_);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(8);
  v->addWidget(controls_, 1);
  // принтер, пластик, профиль печати
  auto* top = new QGridLayout;
  top->setHorizontalSpacing(6);
  top->setVerticalSpacing(6);
  top->setColumnStretch(1, 1);
  printer_ = new Combo;
  filament_ = new Combo;
  process_ = new Combo;
  auto* edit_printer = make_icon_button("⚙", "Настройки принтера: стол, пределы, откат, G-code");
  auto* edit_filament = make_icon_button("⚙", "Настройки пластика: температуры, обдув, расход, откат");
  top->addWidget(make_label("Принтер", "fieldLabel", false), 0, 0);
  top->addWidget(printer_, 0, 1);
  top->addWidget(edit_printer, 0, 2);
  top->addWidget(make_label("Пластик", "fieldLabel", false), 1, 0);
  top->addWidget(filament_, 1, 1);
  top->addWidget(edit_filament, 1, 2);
  top->addWidget(make_label("Печать", "fieldLabel", false), 2, 0);
  top->addWidget(process_, 2, 1, 1, 2);
  cv->addLayout(top);
  auto* save_proc = make_button("Сохранить как свой профиль…", "link");
  save_proc->setToolTip("Сохранить настройки печати под своим именем — они появятся в списке «Печать»");
  cv->addWidget(save_proc, 0, Qt::AlignRight);
  connect(edit_printer, &QPushButton::clicked, this, [this] { edit(Section::Printer); });
  connect(edit_filament, &QPushButton::clicked, this, [this] { edit(Section::Filament); });
  connect(save_proc, &QPushButton::clicked, this, [this] { save_process(); });
  connect(printer_, &QComboBox::activated, this, [this](int) { choose(Section::Printer, printer_); });
  connect(filament_, &QComboBox::activated, this, [this](int) { choose(Section::Filament, filament_); });
  connect(process_, &QComboBox::activated, this, [this](int) { choose(Section::Process, process_); });

  // страницы
  {
    QHBoxLayout* seg = nullptr;
    auto* segf = make_seg(&seg);
    segf->setObjectName("pageSeg");
    auto* group = new QButtonGroup(this);
    group->setExclusive(true);
    pages_ = new QStackedWidget;
    int i = 0;
    for (const auto& name : slicer::pages(Section::Process)) {
      auto* b = make_seg_button(qs(name));
      b->setProperty("cls", "page");
      b->setChecked(i == 0);
      group->addButton(b, i++);
      seg->addWidget(b, 1);
      page_buttons_.push_back(b);
      pages_->addWidget(build_page(name));
    }
    connect(group, &QButtonGroup::idClicked, pages_, &QStackedWidget::setCurrentIndex);
    cv->addWidget(segf);
  }
  advanced_box_ = new QCheckBox("Все настройки");
  advanced_box_->setObjectName("ctl");
  advanced_box_->setToolTip("Показать и тонкие настройки: ширину каждой линии, поправки размеров, ускорения…");
  advanced_box_->setChecked(advanced_);
  connect(advanced_box_, &QCheckBox::toggled, this, [this](bool on) {
    advanced_ = on;
    QSettings("Kika", "kika-gui").setValue("settings/advanced", on);
    apply_advanced();
  });
  cv->addWidget(advanced_box_);
  cv->addWidget(pages_, 1);

  slice_ = make_button("Нарезать", "big");
  slice_->setToolTip("Нарезать деталь с этими настройками: предпросмотр, расчёт прочности и G-code для печати");
  connect(slice_, &QPushButton::clicked, this, [this] {
    if (on_slice) on_slice();
  });
  v->addWidget(slice_);
  status_ = make_hint("");
  v->addWidget(status_);
  apply_advanced();
  set_settings(s_);
}

QWidget* SettingsPanel::build_page(const std::string& name) {
  auto* scroll = new QScrollArea;
  scroll->setObjectName("settingsScroll");
  scroll->setWidgetResizable(true);
  scroll->setFrameShape(QFrame::NoFrame);
  scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  auto* body = new QWidget;
  body->setObjectName("settingsBody");
  auto* bv = new QVBoxLayout(body);
  bv->setContentsMargins(0, 4, 6, 8);
  bv->setSpacing(4);
  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(5);
  grid->setColumnStretch(0, 1);
  bv->addLayout(grid);
  std::string group;
  int r = 0;
  std::vector<std::pair<QWidget*, std::vector<const slicer::Field*>>> heads;  // заголовок раздела и его поля
  for (const auto& f : slicer::fields()) {
    if (f.section != Section::Process || f.page != name) continue;
    if (f.group != group) {
      if (!group.empty()) grid->setRowMinimumHeight(r++, 8);
      group = f.group;
      auto* title = make_label(qs(group), "groupTitle", false);
      grid->addWidget(title, r++, 0, 1, 3);
      heads.push_back({title, {}});
    }
    heads.back().second.push_back(&f);
    Editor ed{&f, nullptr, {}};
    auto* w = make_field_editor(f, this, [this, &f] {
      for (auto& e : editors_)
        if (e.field == &f) {
          if (const auto v = read_field(f, e.widget)) {
            f.set(s_, *v);
            changed();
          }
        }
    });
    ed.widget = w;
    r = add_field_row(grid, r, f, w, &ed.parts);
    editors_.push_back(std::move(ed));
  }
  // заголовок раздела, в котором все поля тонкие, прячется вместе с ними
  for (auto& [title, list] : heads) {
    bool all_adv = !list.empty();
    for (const auto* f : list) all_adv = all_adv && f->advanced;
    if (all_adv) title->setProperty("advanced_group", true);
  }
  bv->addStretch();
  scroll->setWidget(body);
  return scroll;
}

void SettingsPanel::apply_advanced() {
  for (auto& e : editors_)
    for (auto* w : e.parts) w->setVisible(advanced_ || !e.field->advanced);
  for (auto* page : pages_->findChildren<QLabel*>())
    if (page->property("advanced_group").toBool()) page->setVisible(advanced_);
}

void SettingsPanel::set_settings(const slicer::Settings& s) {
  s_ = s;
  fill_printers();
  fill_filaments();
  fill_processes();
  refresh();
}

void SettingsPanel::refresh() {
  for (auto& e : editors_) show_field(*e.field, e.widget, s_);
}

void SettingsPanel::changed() {
  fill_processes();  // профиль печати «(изменён)»
  if (on_changed) on_changed();
}

void SettingsPanel::fill_printers() {
  const QSignalBlocker block(printer_);
  printer_->clear();
  const std::string now = section_json(s_, Section::Printer);
  int sel = -1;
  for (const auto& p : slicer::printers()) {
    printer_->addItem(qs(p.name), "b:" + qs(p.key));
    slicer::Settings t = s_;
    t.printer = p;
    if (sel < 0 && section_json(t, Section::Printer) == now) sel = printer_->count() - 1;
  }
  for (const auto& p : app::list_profiles(Section::Printer)) {
    printer_->addItem(qs(p.name) + " (свой)", "u:" + to_qpath(p.path));
    if (sel >= 0) continue;
    try {
      slicer::Settings t = s_;
      app::load_profile(Section::Printer, p.path, t);
      if (section_json(t, Section::Printer) == now) sel = printer_->count() - 1;
    } catch (const std::exception&) {
    }
  }
  if (sel < 0) {
    printer_->addItem(qs(s_.printer.name) + " (изменён)", "c:");
    sel = printer_->count() - 1;
  }
  printer_->setCurrentIndex(sel);
}

void SettingsPanel::fill_filaments() {
  const QSignalBlocker block(filament_);
  filament_->clear();
  const std::string now = section_json(s_, Section::Filament);
  int sel = -1;
  for (const auto& f : slicer::filaments()) {
    filament_->addItem(qs(f.name), "b:" + qs(f.type));
    slicer::Settings t = s_;
    t.filament = f;
    if (sel < 0 && section_json(t, Section::Filament) == now) sel = filament_->count() - 1;
  }
  for (const auto& p : app::list_profiles(Section::Filament)) {
    filament_->addItem(qs(p.name) + " (свой)", "u:" + to_qpath(p.path));
    if (sel >= 0) continue;
    try {
      slicer::Settings t = s_;
      app::load_profile(Section::Filament, p.path, t);
      if (section_json(t, Section::Filament) == now) sel = filament_->count() - 1;
    } catch (const std::exception&) {
    }
  }
  if (sel < 0) {
    filament_->addItem(qs(s_.filament.name) + " (изменён)", "c:");
    sel = filament_->count() - 1;
  }
  filament_->setCurrentIndex(sel);
}

void SettingsPanel::fill_processes() {
  const QSignalBlocker block(process_);
  process_->clear();
  const std::string now = section_json(s_, Section::Process);
  int sel = -1;
  {
    slicer::Settings t = s_;
    const slicer::Settings d;
    for (const auto& f : slicer::fields())
      if (f.section == Section::Process) f.set(t, f.get(d));
    process_->addItem("Стандартный", "b:default");
    if (section_json(t, Section::Process) == now) sel = 0;
  }
  for (const auto& p : app::list_profiles(Section::Process)) {
    process_->addItem(qs(p.name), "u:" + to_qpath(p.path));
    if (sel >= 0) continue;
    try {
      slicer::Settings t = s_;
      app::load_profile(Section::Process, p.path, t);
      if (section_json(t, Section::Process) == now) sel = process_->count() - 1;
    } catch (const std::exception&) {
    }
  }
  if (sel < 0) {
    process_->addItem("Свои настройки (не сохранены)", "c:");
    sel = process_->count() - 1;
  }
  process_->setCurrentIndex(sel);
}

void SettingsPanel::choose(Section section, QComboBox* combo) {
  const QString d = combo->currentData().toString();
  if (d == "c:") return;
  try {
    if (d.startsWith("u:")) {
      app::load_profile(section, to_path(d.mid(2)), s_);
    } else {
      const std::string key = ss(d.mid(2));
      if (section == Section::Printer) {
        if (const auto* p = slicer::find_printer(key)) s_.printer = *p;
      } else if (section == Section::Filament) {
        s_.filament = slicer::filament_preset(key);
      } else {
        const slicer::Settings d0;
        for (const auto& f : slicer::fields())
          if (f.section == Section::Process) f.set(s_, f.get(d0));
      }
    }
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "Профиль", QString::fromUtf8(e.what()));
  }
  set_settings(s_);
  if (on_changed) on_changed();
}

void SettingsPanel::edit(Section section) {
  SettingsDialog dlg(s_, section, this, true);
  if (dlg.exec() != QDialog::Accepted) return;
  set_settings(dlg.settings());
  if (on_changed) on_changed();
}

void SettingsPanel::save_process() {
  bool ok = false;
  const QString cur = process_->currentData().toString().startsWith("u:") ? process_->currentText() : QString();
  const QString name = QInputDialog::getText(this, "Сохранить настройки печати", "Название профиля:", QLineEdit::Normal, cur, &ok).trimmed();
  if (!ok || name.isEmpty()) return;
  try {
    app::save_profile(Section::Process, ss(name), s_);
    fill_processes();
  } catch (const std::exception& e) {
    QMessageBox::warning(this, "Профиль", QString::fromUtf8(e.what()));
  }
}

void SettingsPanel::set_slice_button(bool enabled, const QString& text, bool primary) {
  slice_->setEnabled(enabled);
  slice_->setText(text);
  slice_->setProperty("cls", primary ? "big" : "bigQuiet");
  repolish(slice_);
}

void SettingsPanel::set_status(const QString& text) {
  status_->setText(text);
  status_->setVisible(!text.isEmpty());
}

void SettingsPanel::set_locked(const QString& why) {
  lock_note_->setText(why);
  lock_note_->setVisible(!why.isEmpty());
  controls_->setEnabled(why.isEmpty());
  slice_->setVisible(why.isEmpty());
}

void SettingsPanel::select_page(const std::string& name) {
  const auto names = slicer::pages(Section::Process);
  for (std::size_t i = 0; i < names.size(); ++i)
    if (names[i] == name) {
      page_buttons_[i]->setChecked(true);
      pages_->setCurrentIndex(static_cast<int>(i));
    }
}

void SettingsPanel::set_collapsed(bool on) {
  collapsed_ = on;
  body_->setVisible(!on);
  strip_->setVisible(on);
  if (on) {
    setMinimumWidth(0);
    setMaximumWidth(40);
  } else {
    setMinimumWidth(340);
    setMaximumWidth(QWIDGETSIZE_MAX);
  }
}

}  // namespace kika::gui
