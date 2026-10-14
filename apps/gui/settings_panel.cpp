// Панель настроек печати слева. Без moc: сигналы — лямбдами.

#include "settings_panel.hpp"

#include <QButtonGroup>
#include <QCheckBox>
#include <QGridLayout>
#include <QInputDialog>
#include <QLineEdit>
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

  // поиск
  search_ = new QLineEdit;
  search_->setObjectName("search");
  search_->setPlaceholderText("Найти настройку…");
  search_->setClearButtonEnabled(true);
  search_->setToolTip("По названию, разделу или ключу (как в JSON и kika settings): «откат», «шов», seam_gap");
  connect(search_, &QLineEdit::textChanged, this, [this](const QString& t) { search(t); });
  cv->addWidget(search_);
  search_note_ = make_hint("");
  search_note_->hide();
  cv->addWidget(search_note_);

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
    apply_visibility();
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
  apply_visibility();
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
  QWidget* title = nullptr;
  const int page = static_cast<int>(page_buttons_.size()) - 1;  // кнопка страницы уже добавлена
  for (const auto& f : slicer::fields()) {
    if (f.section != Section::Process || f.page != name) continue;
    if (f.group != group) {
      if (!group.empty()) grid->setRowMinimumHeight(r++, 8);
      group = f.group;
      title = make_label(qs(group), "groupTitle", false);
      grid->addWidget(title, r++, 0, 1, 4);
    }
    Editor ed{&f, nullptr, {}};
    ed.page = page;
    ed.title = title;
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
    const int row0 = r;
    r = add_field_row(grid, r, f, w, &ed.parts);
    ed.label = ed.parts.empty() ? w : ed.parts.front();
    auto* reset = make_icon_button("↺", "Вернуть значение профиля");
    reset->setObjectName("resetField");
    reset->setFixedWidth(22);
    grid->addWidget(reset, row0, 3, Qt::AlignTop);
    connect(reset, &QPushButton::clicked, this, [this, &f] {
      f.set(s_, f.get(base_));
      refresh();
      changed();
    });
    ed.reset = reset;
    editors_.push_back(std::move(ed));
  }
  grid->setColumnMinimumWidth(3, 22);
  bv->addStretch();
  scroll->setWidget(body);
  return scroll;
}

namespace {

bool matches(const slicer::Field& f, const QString& q) {
  return qs(f.label).contains(q, Qt::CaseInsensitive) || qs(f.group).contains(q, Qt::CaseInsensitive) ||
         qs(f.key).contains(q, Qt::CaseInsensitive) || qs(f.page).contains(q, Qt::CaseInsensitive);
}

}  // namespace

// Видимость строк: тонкие настройки — в режиме «Все настройки»; при поиске — только найденные
// (и тонкие тоже). Заголовок раздела — если в нём что-то видно; страницы без найденного — серые.
void SettingsPanel::apply_visibility() {
  const QString q = query_.trimmed();
  std::vector<int> found(page_buttons_.size(), 0);
  std::vector<std::pair<QWidget*, bool>> titles;
  for (auto& e : editors_) {
    const bool on = q.isEmpty() ? (advanced_ || !e.field->advanced) : matches(*e.field, q);
    for (auto* w : e.parts) w->setVisible(on);
    if (e.reset) e.reset->setVisible(on && e.reset->property("on").toBool());
    if (on) ++found[static_cast<std::size_t>(e.page)];
    if (titles.empty() || titles.back().first != e.title)
      titles.push_back({e.title, on});
    else
      titles.back().second = titles.back().second || on;
  }
  for (auto& [t, on] : titles)
    if (t) t->setVisible(on);
  int total = 0;
  for (std::size_t i = 0; i < page_buttons_.size(); ++i) {
    page_buttons_[i]->setEnabled(q.isEmpty() || found[i] > 0);
    total += found[i];
  }
  if (!q.isEmpty()) {
    // открыть страницу с найденным
    const int cur = pages_->currentIndex();
    if (cur < 0 || found[static_cast<std::size_t>(cur)] == 0)
      for (std::size_t i = 0; i < found.size(); ++i)
        if (found[i] > 0) {
          page_buttons_[i]->setChecked(true);
          pages_->setCurrentIndex(static_cast<int>(i));
          break;
        }
    QString pages;
    for (std::size_t i = 0; i < found.size(); ++i)
      if (found[i] > 0) pages += (pages.isEmpty() ? "" : ", ") + page_buttons_[i]->text() + QString(" — %1").arg(found[i]);
    search_note_->setText(total ? QString("Найдено: %1").arg(pages) : QString("Ничего не нашлось."));
  }
  search_note_->setVisible(!q.isEmpty());
}

void SettingsPanel::search(const QString& text) {
  query_ = text;
  if (search_->text() != text) search_->setText(text);
  apply_visibility();
}

// Изменённые против профиля: подпись подсвечена, «↺» виден.
void SettingsPanel::update_marks() {
  for (auto& e : editors_) {
    const bool mod = json::dump(e.field->get(s_)) != json::dump(e.field->get(base_));
    if (e.label && e.label->property("modified").toBool() != mod) {
      e.label->setProperty("modified", mod);
      repolish(e.label);
    }
    if (e.reset) {
      e.reset->setProperty("on", mod);
      e.reset->setVisible(mod && e.label && !e.label->isHidden());  // строка не спрятана (страницы — не в счёт)
    }
  }
}

int SettingsPanel::modified_count() const {
  int n = 0;
  for (const auto& e : editors_)
    if (json::dump(e.field->get(s_)) != json::dump(e.field->get(base_))) ++n;
  return n;
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
  update_marks();
}

void SettingsPanel::changed() {
  fill_processes();  // профиль печати «(изменён)»
  update_marks();
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
    if (section_json(t, Section::Process) == now) {
      sel = 0;
      base_ = t;
    }
  }
  for (const auto& p : app::list_profiles(Section::Process)) {
    process_->addItem(qs(p.name), "u:" + to_qpath(p.path));
    if (sel >= 0) continue;
    try {
      slicer::Settings t = s_;
      app::load_profile(Section::Process, p.path, t);
      if (section_json(t, Section::Process) == now) {
        sel = process_->count() - 1;
        base_ = t;
      }
    } catch (const std::exception&) {
    }
  }
  if (sel < 0) {
    // профиль, от которого правки, — прежний: подпись вида «Стандартный — изменён: 3»
    int n = 0;
    for (const auto& f : slicer::fields())
      if (f.section == Section::Process && json::dump(f.get(s_)) != json::dump(f.get(base_))) ++n;
    process_->addItem(QString("Свои настройки — изменено %1").arg(n), "c:");
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
    base_ = s_;
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
