#pragma once
// Поля ввода настроек нарезки по таблице slicer::fields(): одно и то же в панели настроек слева
// и в окне настроек принтера и пластика.

#include <QCheckBox>
#include <QFontDatabase>
#include <QGridLayout>
#include <QLineEdit>
#include <QPlainTextEdit>

#include <functional>
#include <optional>
#include <string>

#include "kika/slicer/profiles.hpp"
#include "widgets.hpp"

namespace kika::gui {

// Поле ввода для настройки; changed — после каждой правки.
inline QWidget* make_field_editor(const slicer::Field& f, QWidget* owner, std::function<void()> changed) {
  using slicer::FieldKind;
  QWidget* w = nullptr;
  switch (f.kind) {
    case FieldKind::Number:
    case FieldKind::Integer: {
      auto* e = make_num(std::nullopt);
      e->setFixedWidth(84);
      QObject::connect(e, &QLineEdit::textEdited, owner, [changed](const QString&) { changed(); });
      w = e;
      break;
    }
    case FieldKind::Bool: {
      auto* c = new QCheckBox(qs(f.label));
      QObject::connect(c, &QCheckBox::toggled, owner, [changed](bool) { changed(); });
      w = c;
      break;
    }
    case FieldKind::Choice: {
      auto* c = new Combo;
      for (const auto& ch : f.choices) c->addItem(qs(ch.name), qs(ch.key));
      QObject::connect(c, &QComboBox::currentIndexChanged, owner, [changed](int) { changed(); });
      w = c;
      break;
    }
    case FieldKind::Text: {
      auto* e = new QLineEdit;
      QObject::connect(e, &QLineEdit::textEdited, owner, [changed](const QString&) { changed(); });
      w = e;
      break;
    }
    case FieldKind::Code: {
      auto* e = new QPlainTextEdit;
      e->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
      e->setMinimumHeight(150);
      e->setLineWrapMode(QPlainTextEdit::NoWrap);
      QObject::connect(e, &QPlainTextEdit::textChanged, owner, [changed] { changed(); });
      w = e;
      break;
    }
  }
  w->setToolTip(qs(f.tip));
  return w;
}

// Показать значение настройки (без сигналов правки).
inline void show_field(const slicer::Field& f, QWidget* w, const slicer::Settings& s) {
  using slicer::FieldKind;
  const QSignalBlocker block(w);
  const auto v = f.get(s);
  switch (f.kind) {
    case FieldKind::Number:
    case FieldKind::Integer: {
      auto* e = static_cast<QLineEdit*>(w);
      if (!e->hasFocus() || !parse_num(e->text()) || std::abs(*parse_num(e->text()) - v.as_double()) > 1e-12)
        e->setText(num_text(v.as_double()));
      break;
    }
    case FieldKind::Bool: static_cast<QCheckBox*>(w)->setChecked(v.as_bool()); break;
    case FieldKind::Choice: {
      auto* c = static_cast<QComboBox*>(w);
      const int i = c->findData(qs(v.as_string()));
      c->setCurrentIndex(i >= 0 ? i : 0);
      break;
    }
    case FieldKind::Text: static_cast<QLineEdit*>(w)->setText(qs(v.as_string())); break;
    case FieldKind::Code: {
      auto* e = static_cast<QPlainTextEdit*>(w);
      if (e->toPlainText() != qs(v.as_string())) e->setPlainText(qs(v.as_string()));
      break;
    }
  }
  set_bad(w, false);
}

// Значение из поля, если оно допустимо (иначе поле отмечается красным и пусто).
inline std::optional<json::Value> read_field(const slicer::Field& f, QWidget* w) {
  using slicer::FieldKind;
  json::Value v;
  switch (f.kind) {
    case FieldKind::Number:
    case FieldKind::Integer: {
      const auto x = parse_num(static_cast<QLineEdit*>(w)->text());
      v = x ? json::Value(*x) : json::Value(std::string());
      break;
    }
    case FieldKind::Bool: v = static_cast<QCheckBox*>(w)->isChecked(); break;
    case FieldKind::Choice: v = ss(static_cast<QComboBox*>(w)->currentData().toString()); break;
    case FieldKind::Text: v = ss(static_cast<QLineEdit*>(w)->text()); break;
    case FieldKind::Code: v = ss(static_cast<QPlainTextEdit*>(w)->toPlainText()); break;
  }
  const bool ok = slicer::check_field(f, v, "").empty();
  set_bad(w, !ok);
  if (!ok) return std::nullopt;
  return v;
}

// Строка поля в сетке: подпись, поле, единицы. Возвращает номер следующей строки.
inline int add_field_row(QGridLayout* grid, int r, const slicer::Field& f, QWidget* w, std::vector<QWidget*>* parts = nullptr) {
  using slicer::FieldKind;
  const QString tip = qs(f.tip);
  auto keep = [&](QWidget* x) {
    if (parts) parts->push_back(x);
  };
  if (f.kind == FieldKind::Bool) {
    grid->addWidget(w, r, 0, 1, 3);
    keep(w);
  } else if (f.kind == FieldKind::Code) {
    auto* l = make_label(qs(f.label), "fieldLabel", false);
    l->setToolTip(tip);
    grid->addWidget(l, r++, 0, 1, 3);
    grid->addWidget(w, r, 0, 1, 3);
    keep(l);
    keep(w);
    if (!f.tip.empty()) {
      auto* h = make_hint(tip);
      grid->addWidget(h, ++r, 0, 1, 3);
      keep(h);
    }
  } else {
    auto* l = make_label(qs(f.label), nullptr, true);
    l->setToolTip(tip);
    grid->addWidget(l, r, 0);
    keep(l);
    if (f.kind == FieldKind::Number || f.kind == FieldKind::Integer) {
      auto* row = hbox(6);
      row->addWidget(w);
      auto* u = make_label(qs(f.unit), "hint", false);
      u->setMinimumWidth(40);
      row->addWidget(u);
      auto* box = wrap_layout(row);
      grid->addWidget(box, r, 1, 1, 2, Qt::AlignLeft);
      keep(box);
    } else {
      grid->addWidget(w, r, 1, 1, 2);
      w->setMinimumWidth(f.kind == FieldKind::Choice ? 160 : 120);
      keep(w);
    }
  }
  return r + 1;
}

}  // namespace kika::gui
