// Главное окно: вкладки «Подготовка», «Предпросмотр» и «Принтер». Вкладка «Расчёт нагрузок» —
// в main_window.cpp (пошаговая панель задания и 3D-вид расчёта).

#include <QCheckBox>
#include <QComboBox>
#include <QFrame>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <numbers>

#include "main_window.hpp"
#include "plate_view.hpp"
#include "printer_panel.hpp"
#include "range_slider.hpp"
#include "settings_panel.hpp"
#include "web_pane.hpp"
#include "widgets.hpp"

namespace kika::gui {

namespace {

// Правая колонка вкладки: прокрутка с панелью фиксированной ширины.
QScrollArea* side_scroll(QWidget* inner, int width) {
  auto* sc = new QScrollArea;
  sc->setObjectName("sidePanel");
  sc->setWidgetResizable(true);
  sc->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  sc->setFrameShape(QFrame::NoFrame);
  sc->setFixedWidth(width);
  inner->setObjectName("sideInner");
  sc->setWidget(inner);
  return sc;
}

QLineEdit* small_num() {
  auto* e = make_num(std::nullopt);
  e->setFixedWidth(64);
  return e;
}

geometry::Box placed_box(const geometry::TriangleMesh& m, const geometry::Transform& t) {
  geometry::Box b;
  bool first = true;
  for (const auto& v : m.vertices) {
    const auto p = t.apply(v);
    for (std::size_t a = 0; a < 3; ++a) {
      b.min[a] = first ? p[a] : std::min(b.min[a], p[a]);
      b.max[a] = first ? p[a] : std::max(b.max[a], p[a]);
    }
    first = false;
  }
  return b;
}

}  // namespace

// ============================================================================ подготовка

QWidget* MainWindow::build_prepare_page() {
  auto* page = new QWidget;
  auto* h = new QHBoxLayout(page);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  auto* center = new QWidget;
  auto* cv = new QVBoxLayout(center);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  {
    auto* bar = new QFrame;
    bar->setObjectName("pickBar");
    auto* b = new QHBoxLayout(bar);
    b->setContentsMargins(12, 8, 12, 8);
    b->setSpacing(8);
    for (int a = 0; a < 3; ++a) {
      auto* btn = make_button(QString("↻ %1").arg(QChar("XYZ"[a])), "sm");
      btn->setToolTip(QString("Повернуть деталь на 90° вокруг оси %1").arg(QChar("XYZ"[a])));
      connect(btn, &QPushButton::clicked, this, [this, a] { rotate_part(a); });
      b->addWidget(btn);
    }
    btn_lay_ = make_button("На грань", "sm");
    btn_lay_->setCheckable(true);
    btn_lay_->setToolTip("Щёлкните по грани детали — деталь ляжет на неё на стол");
    connect(btn_lay_, &QPushButton::toggled, this, [this](bool on) { plate_->set_face_pick(on); });
    b->addWidget(btn_lay_);
    auto* center_btn = make_button("По центру", "sm");
    center_btn->setToolTip("Поставить деталь в центр стола");
    connect(center_btn, &QPushButton::clicked, this, [this] {
      print_ui_.position.reset();
      render_prepare();
      render_slice_state();
    });
    b->addWidget(center_btn);
    auto* reset = make_button("Сброс", "sm");
    reset->setToolTip("Как в файле модели: без поворота и масштаба, в центре стола");
    connect(reset, &QPushButton::clicked, this, [this] { reset_placement(true); });
    b->addWidget(reset);
    b->addStretch();
    const std::pair<const char*, const char*> views[] = {
        {"part", "К детали"}, {"iso", "Стол"}, {"front", "Спереди"}, {"top", "Сверху"}, {"right", "Справа"}};
    for (const auto& [key, name] : views) {
      auto* vb = make_button(name, "sm");
      const std::string k = key;
      connect(vb, &QPushButton::clicked, this, [this, k] { plate_->view(k); });
      b->addWidget(vb);
    }
    cv->addWidget(bar);
  }
  plate_ = new PlateView(PlateView::Mode::Prepare);
  plate_->set_empty_text("Перетащите сюда модель (STL, 3MF, STEP) или G-code — или откройте пример");
  plate_->on_drag = [this](double dx, double dy, bool done) { on_part_drag(dx, dy, done); };
  plate_->on_face = [this](const geometry::Vec3& n) { lay_on_face(n); };
  cv->addWidget(plate_, 1);
  h->addWidget(center, 1);

  // справа — деталь и её положение
  auto* inner = new QWidget;
  auto* v = new QVBoxLayout(inner);
  v->setContentsMargins(14, 12, 14, 14);
  v->setSpacing(10);
  v->addWidget(make_label("Деталь", "stepTitle", false));
  prep_info_ = make_label("", "muted");
  prep_info_->setTextFormat(Qt::RichText);
  v->addWidget(prep_info_);
  prep_box_ = new QWidget;
  {
    auto* g = new QGridLayout(prep_box_);
    g->setContentsMargins(0, 4, 0, 0);
    g->setHorizontalSpacing(6);
    g->setVerticalSpacing(6);
    int r = 0;
    auto head = [&](const QString& t) {
      g->addWidget(make_label(t, "groupTitle", false), r++, 0, 1, 4);
    };
    auto axes = [&](const char* first) {
      for (int a = 0; a < 3; ++a) {
        auto* l = make_label(QString(first[a]), "hint", false);
        l->setAlignment(Qt::AlignCenter);
        g->addWidget(l, r, a + 1);
      }
      ++r;
    };
    head("Положение на столе, мм");
    axes("XY ");
    g->addWidget(make_label("центр", "hint", false), r, 0);
    for (int a = 0; a < 2; ++a) {
      pos_edit_[static_cast<std::size_t>(a)] = small_num();
      pos_edit_[static_cast<std::size_t>(a)]->setToolTip("Центр детали на столе; перетащите деталь мышью или впишите");
      g->addWidget(pos_edit_[static_cast<std::size_t>(a)], r, a + 1);
    }
    ++r;
    head("Поворот, °");
    axes("XYZ");
    g->addWidget(make_label("угол", "hint", false), r, 0);
    for (int a = 0; a < 3; ++a) {
      rot_edit_[static_cast<std::size_t>(a)] = small_num();
      g->addWidget(rot_edit_[static_cast<std::size_t>(a)], r, a + 1);
    }
    ++r;
    head("Масштаб и размер");
    axes("XYZ");
    g->addWidget(make_label("%", "hint", false), r, 0);
    for (int a = 0; a < 3; ++a) {
      scale_edit_[static_cast<std::size_t>(a)] = small_num();
      g->addWidget(scale_edit_[static_cast<std::size_t>(a)], r, a + 1);
    }
    ++r;
    g->addWidget(make_label("мм", "hint", false), r, 0);
    for (int a = 0; a < 3; ++a) {
      size_edit_[static_cast<std::size_t>(a)] = small_num();
      size_edit_[static_cast<std::size_t>(a)]->setToolTip("Размер детали по оси стола");
      g->addWidget(size_edit_[static_cast<std::size_t>(a)], r, a + 1);
    }
    ++r;
    uniform_ = new QCheckBox("Пропорционально");
    uniform_->setChecked(true);
    uniform_->setToolTip("Масштаб одинаковый по всем осям");
    g->addWidget(uniform_, r++, 0, 1, 4);
    g->setColumnStretch(0, 1);
    for (auto* e : pos_edit_) connect(e, &QLineEdit::editingFinished, this, [this] { read_placement(); });
    for (auto* e : rot_edit_) connect(e, &QLineEdit::editingFinished, this, [this] { read_placement(); });
    for (int a = 0; a < 3; ++a) {
      connect(scale_edit_[static_cast<std::size_t>(a)], &QLineEdit::editingFinished, this, [this, a] {
        const auto x = parse_num(scale_edit_[static_cast<std::size_t>(a)]->text());
        if (!x || *x < 1 || *x > 10000) {
          set_bad(scale_edit_[static_cast<std::size_t>(a)], true);
          return;
        }
        if (uniform_->isChecked())
          print_ui_.scale = {*x / 100, *x / 100, *x / 100};
        else
          print_ui_.scale[static_cast<std::size_t>(a)] = *x / 100;
        render_prepare();
        render_slice_state();
      });
      connect(size_edit_[static_cast<std::size_t>(a)], &QLineEdit::editingFinished, this, [this, a] {
        const auto x = parse_num(size_edit_[static_cast<std::size_t>(a)]->text());
        if (!source_ || !x || *x <= 0) return;
        // размер без масштаба по этой оси — после поворота
        slicer::PrintJob one = print_ui_;
        one.scale = {1, 1, 1};
        const auto b = placed_box(source_->mesh->mesh, slicer::placement(source_->mesh->mesh, one));
        const double base = b.max[static_cast<std::size_t>(a)] - b.min[static_cast<std::size_t>(a)];
        if (base <= 1e-9) return;
        const double k = std::clamp(*x / base, 0.01, 100.0);
        if (uniform_->isChecked())
          print_ui_.scale = {k, k, k};
        else
          print_ui_.scale[static_cast<std::size_t>(a)] = k;
        render_prepare();
        render_slice_state();
      });
    }
  }
  v->addWidget(prep_box_);
  fit_warn_ = make_warn("");
  fit_warn_->hide();
  v->addWidget(fit_warn_);
  prep_warns_ = new QVBoxLayout;
  prep_warns_->setSpacing(6);
  v->addLayout(prep_warns_);
  prep_note_ = make_hint("Ориентация на столе сильнее всего влияет на прочность: слои плохо держат растяжение поперёк. "
                         "Подобрать положение по нагрузкам — во вкладке «Расчёт нагрузок».");
  v->addWidget(prep_note_);
  v->addStretch();
  h->addWidget(side_scroll(inner, 300));
  return page;
}

void MainWindow::render_prepare() {
  if (!plate_) return;
  plate_->set_printer(print_ui_.settings.printer);
  pview_->set_printer(print_ui_.settings.printer);
  clear_layout(prep_warns_);
  if (!source_) {
    plate_->set_model(nullptr);
    plate_->set_info({});
    prep_box_->hide();
    fit_warn_->hide();
    btn_lay_->setChecked(false);
    prep_info_->setText(model_ ? "Открыт готовый G-code — деталь уже нарезана, размещать нечего. Смотрите «Предпросмотр» и "
                                 "«Расчёт нагрузок». Чтобы нарезать здесь, откройте модель (STL, 3MF, STEP)."
                               : "Откройте модель (STL, 3MF, STEP) — она встанет в центр стола. Её можно передвинуть, "
                                 "повернуть, положить на грань и масштабировать.");
    plate_->set_empty_text(model_ ? "Открыт готовый G-code — смотрите вкладку «Предпросмотр»"
                                  : "Перетащите сюда модель (STL, 3MF, STEP) или G-code — или откройте пример");
    return;
  }
  const auto& mesh = source_->mesh->mesh;
  plate_->set_model(std::shared_ptr<const geometry::TriangleMesh>(source_->mesh, &source_->mesh->mesh));
  const auto t = slicer::placement(mesh, print_ui_);
  const auto box = placed_box(mesh, t);
  const auto& pr = print_ui_.settings.printer;
  const double eps = 1e-6;
  const bool fits = box.min[0] >= -eps && box.min[1] >= -eps && box.max[0] <= pr.bed_x + eps && box.max[1] <= pr.bed_y + eps &&
                    box.max[2] <= pr.max_z + eps;
  plate_->set_placement(t, fits);
  const auto sz = box.size();
  plate_->set_info({QString("%1 × %2 × %3 мм").arg(fmtq(sz[0], 1), fmtq(sz[1], 1), fmtq(sz[2], 1))});
  fit_warn_->setText(QString("Деталь не помещается в область печати %1 (%2 × %3 × %4 мм) — передвиньте, поверните или "
                             "уменьшите её.")
                         .arg(qs(pr.name), fmtq(pr.bed_x, 0), fmtq(pr.bed_y, 0), fmtq(pr.max_z, 0)));
  fit_warn_->setVisible(!fits);
  const auto& im = *source_->mesh;
  prep_info_->setText(QString("<b>%1</b><br>%2 · %3 треуг. · %4 см³")
                          .arg(esc(im.name.empty() ? std::string("деталь") : im.name), esc(im.format),
                               ru_int(static_cast<long long>(im.mesh.triangles.size())),
                               fmtq(im.mesh.volume() * print_ui_.scale[0] * print_ui_.scale[1] * print_ui_.scale[2] / 1000, 2)));
  prep_box_->show();
  const auto at = slicer::bed_position(print_ui_);
  auto put = [](QLineEdit* e, double v, int digits) {
    const QSignalBlocker block(e);
    if (!e->hasFocus()) e->setText(qs(app::fmt(v, digits)));
    set_bad(e, false);
  };
  for (std::size_t a = 0; a < 2; ++a) put(pos_edit_[a], at[a], 1);
  for (std::size_t a = 0; a < 3; ++a) {
    const double r = print_ui_.rotate[a];
    put(rot_edit_[a], r, std::abs(r - std::round(r)) < 1e-9 ? 0 : 2);
    put(scale_edit_[a], print_ui_.scale[a] * 100, std::abs(print_ui_.scale[a] * 100 - std::round(print_ui_.scale[a] * 100)) < 1e-9 ? 0 : 1);
    put(size_edit_[a], sz[a], 1);
  }
  for (const auto& w : im.warnings) prep_warns_->addWidget(make_warn(qs(w)));
}

void MainWindow::read_placement() {
  if (!source_) return;
  bool ok = true;
  std::array<double, 2> pos{};
  std::array<double, 3> rot{};
  for (std::size_t a = 0; a < 2; ++a) {
    const auto x = parse_num(pos_edit_[a]->text());
    set_bad(pos_edit_[a], !x);
    ok = ok && x.has_value();
    if (x) pos[a] = *x;
  }
  for (std::size_t a = 0; a < 3; ++a) {
    const auto x = parse_num(rot_edit_[a]->text());
    set_bad(rot_edit_[a], !x);
    ok = ok && x.has_value();
    if (x) rot[a] = *x;
  }
  if (!ok) return;
  const auto at = slicer::bed_position(print_ui_);
  if (std::abs(pos[0] - at[0]) > 0.05 || std::abs(pos[1] - at[1]) > 0.05) print_ui_.position = pos;
  for (std::size_t a = 0; a < 3; ++a)
    if (std::abs(rot[a] - print_ui_.rotate[a]) > 1e-6) print_ui_.rotate[a] = rot[a];
  render_prepare();
  render_slice_state();
}

void MainWindow::on_part_drag(double dx, double dy, bool done) {
  if (!source_) return;
  const auto at = slicer::bed_position(print_ui_);
  const std::array<double, 2> p{at[0] + dx, at[1] + dy};
  if (!done) {
    for (std::size_t a = 0; a < 2; ++a) {
      const QSignalBlocker block(pos_edit_[a]);
      pos_edit_[a]->setText(qs(app::fmt(p[a], 1)));
    }
    return;
  }
  print_ui_.position = p;
  render_prepare();
  render_slice_state();
}

void MainWindow::lay_on_face(const geometry::Vec3& n) {
  if (!source_) return;
  // нормаль до масштаба: поворот Rₐ переводит её вниз, и после масштаба грань тоже смотрит вниз
  geometry::Vec3 r{n[0] * print_ui_.scale[0], n[1] * print_ui_.scale[1], n[2] * print_ui_.scale[2]};
  const double l = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  if (l < 1e-12) return;
  for (auto& x : r) x /= l;
  // поворот r → (0, 0, −1) по формуле Родрига
  const geometry::Vec3 down{0, 0, -1};
  geometry::Vec3 k{r[1] * down[2] - r[2] * down[1], r[2] * down[0] - r[0] * down[2], r[0] * down[1] - r[1] * down[0]};
  const double s = std::sqrt(k[0] * k[0] + k[1] * k[1] + k[2] * k[2]), c = -r[2];
  geometry::Transform ra;
  if (s < 1e-9) {
    if (c < 0) ra = geometry::Transform::rotation(0, 180);  // смотрит вверх — перевернуть
  } else {
    for (auto& x : k) x /= s;
    const double kx = k[0], ky = k[1], kz = k[2], t = 1 - c;
    ra.r = {c + kx * kx * t,      kx * ky * t - kz * s, kx * kz * t + ky * s,
            ky * kx * t + kz * s, c + ky * ky * t,      ky * kz * t - kx * s,
            kz * kx * t - ky * s, kz * ky * t + kx * s, c + kz * kz * t};
  }
  const auto e = slicer::euler_xyz(ra * slicer::rotation_of(print_ui_.rotate));
  print_ui_.rotate = e;
  // почти целые углы — целыми
  for (auto& a : print_ui_.rotate)
    if (std::abs(a - std::round(a)) < 1e-6) a = std::round(a);
  btn_lay_->setChecked(false);
  render_prepare();
  render_slice_state();
  toast("Деталь положена на выбранную грань.");
}

void MainWindow::reset_placement(bool all) {
  print_ui_.position.reset();
  if (all) {
    print_ui_.rotate = {0, 0, 0};
    print_ui_.scale = {1, 1, 1};
  }
  render_prepare();
  render_slice_state();
}

// ============================================================================ предпросмотр

QWidget* MainWindow::build_preview_page() {
  auto* page = new QWidget;
  auto* h = new QHBoxLayout(page);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  auto* center = new QWidget;
  auto* cv = new QVBoxLayout(center);
  cv->setContentsMargins(0, 0, 0, 0);
  cv->setSpacing(0);
  stale_banner_ = new QFrame;
  stale_banner_->setObjectName("staleBanner");
  {
    auto* b = new QHBoxLayout(stale_banner_);
    b->setContentsMargins(12, 7, 12, 7);
    b->setSpacing(10);
    b->addWidget(make_label("Настройки или положение детали изменены после нарезки — предпросмотр по прежней.", nullptr, true), 1);
    auto* again = make_button("Нарезать заново", "sm");
    connect(again, &QPushButton::clicked, this, [this] { reslice(); });
    b->addWidget(again);
  }
  stale_banner_->hide();
  cv->addWidget(stale_banner_);
  {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    pview_ = new PlateView(PlateView::Mode::Preview);
    pview_->set_empty_text("Здесь будет нарезка по слоям — откройте модель или G-code");
    row->addWidget(pview_, 1);
    layer_slider_ = new RangeSlider;
    layer_slider_->setObjectName("layerSlider");
    layer_slider_->setFixedWidth(96);
    layer_slider_->setToolTip("Слои: верхняя ручка — до какого слоя показывать, нижняя — с какого. Колесо и стрелки — "
                              "верхняя, с Shift — нижняя.");
    layer_slider_->label = [this](int i) {
      if (!preview_ || i < 0 || static_cast<std::size_t>(i) >= preview_->layers.size()) return QString::number(i + 1);
      return QString("%1 · %2").arg(i + 1).arg(fmtq(preview_->layers[static_cast<std::size_t>(i)].z, 2));
    };
    layer_slider_->on_change = [this](int, int) { apply_layers(); };
    row->addWidget(layer_slider_);
    cv->addLayout(row, 1);
  }
  {
    auto* bar = new QFrame;
    bar->setObjectName("viewBar");
    auto* b = new QHBoxLayout(bar);
    b->setContentsMargins(12, 8, 12, 8);
    b->setSpacing(10);
    b->addWidget(make_label("движения слоя", "ctl", false));
    move_slider_ = new Slider(1, 1, 1);
    move_slider_->setToolTip("Как сопло проходит верхний показанный слой");
    b->addWidget(move_slider_, 1);
    move_label_ = make_label("", "ctl", false);
    move_label_->setMinimumWidth(260);
    b->addWidget(move_label_);
    const std::pair<const char*, const char*> views[] = {{"part", "К детали"}, {"iso", "Стол"}, {"top", "Сверху"}};
    for (const auto& [key, name] : views) {
      auto* vb = make_button(name, "sm");
      const std::string k = key;
      connect(vb, &QPushButton::clicked, this, [this, k] { pview_->view(k); });
      b->addWidget(vb);
    }
    connect(move_slider_, &QSlider::valueChanged, this, [this](int) { apply_layers(); });
    cv->addWidget(bar);
  }
  h->addWidget(center, 1);

  // справа: легенда, итоги, печать
  auto* inner = new QWidget;
  auto* v = new QVBoxLayout(inner);
  v->setContentsMargins(14, 12, 14, 14);
  v->setSpacing(10);
  v->addWidget(make_label("Нарезка", "stepTitle", false));
  totals_ = make_label("", nullptr, true);
  totals_->setTextFormat(Qt::RichText);
  v->addWidget(totals_);
  {
    auto* row = hbox(8);
    row->addWidget(make_label("Цвет", "fieldLabel", false));
    color_combo_ = new Combo;
    for (const auto c : {app::ColorBy::Type, app::ColorBy::Speed, app::ColorBy::Flow, app::ColorBy::Width,
                         app::ColorBy::Height, app::ColorBy::Fan, app::ColorBy::LayerTime})
      color_combo_->addItem(qs(std::string(app::color_by_name(c))), qs(std::string(app::color_by_key(c))));
    row->addWidget(color_combo_, 1);
    v->addLayout(row);
    connect(color_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
      if (const auto c = app::color_by_from_key(combo_key(color_combo_))) preview_view_.color = *c;
      if (preview_) pview_->set_preview(preview_, preview_view_);
      apply_layers();
      render_legend();
    });
  }
  legend_list_ = new QVBoxLayout;
  legend_list_->setSpacing(3);
  v->addLayout(legend_list_);
  layer_info_ = make_hint("");
  v->addWidget(layer_info_);
  preview_warns_ = new QVBoxLayout;
  preview_warns_->setSpacing(6);
  v->addLayout(preview_warns_);
  v->addSpacing(4);
  v->addWidget(make_label("Печать", "groupTitle", false));
  btn_print_ = make_button("Печать на принтере", "primary");
  btn_print_->setToolTip("Отправить G-code на принтер (Klipper, Moonraker) и начать печать — с подтверждением");
  btn_send_ = make_button("Только отправить", "sm");
  btn_send_->setToolTip("Положить G-code на принтер, не начиная печать");
  btn_save_gcode_ = make_button("Сохранить G-code…", "sm");
  v->addWidget(btn_print_);
  {
    auto* row = hbox(8);
    row->addWidget(btn_send_);
    row->addWidget(btn_save_gcode_);
    row->addStretch();
    v->addLayout(row);
  }
  auto_switch_ = new QCheckBox("После отправки — во вкладку «Принтер»");
  auto_switch_->setToolTip("Сразу показать принтер: ход печати, температуры, камеру");
  auto_switch_->setChecked(QSettings("Kika", "kika-gui").value("printer/auto_switch", true).toBool());
  connect(auto_switch_, &QCheckBox::toggled, this, [](bool on) { QSettings("Kika", "kika-gui").setValue("printer/auto_switch", on); });
  v->addWidget(auto_switch_);
  connect(btn_print_, &QPushButton::clicked, this, [this] { send_to_printer(true); });
  connect(btn_send_, &QPushButton::clicked, this, [this] { send_to_printer(false); });
  connect(btn_save_gcode_, &QPushButton::clicked, this, [this] { save_gcode(); });
  v->addStretch();
  h->addWidget(side_scroll(inner, 340));
  render_preview();
  return page;
}

void MainWindow::set_preview(std::shared_ptr<const app::Preview> p) {
  preview_ = std::move(p);
  pview_->set_preview(preview_, preview_view_);
  layer_slider_->set_count(preview_ ? static_cast<int>(preview_->layers.size()) : 0);
  apply_layers();
  render_preview();
}

void MainWindow::render_preview() {
  if (!totals_) return;
  clear_layout(preview_warns_);
  const bool has = preview_ && !preview_->empty();
  btn_print_->setEnabled(has && !busy_);
  btn_send_->setEnabled(has && !busy_);
  btn_save_gcode_->setVisible(source_.has_value());
  btn_save_gcode_->setEnabled(source_.has_value() && !busy_);
  if (!has) {
    totals_->setText("<span style='color:#61646b'>Нарезки пока нет.</span>");
    layer_info_->clear();
    render_legend();
    return;
  }
  const auto& p = *preview_;
  std::vector<std::pair<QString, QString>> rows;
  rows.emplace_back("Печать", qs(app::fmt_duration(p.time_s)) + (p.time_from_file ? QString() : QString(" (оценка)")));
  rows.emplace_back("Пластик", fmtq(p.filament_g, p.filament_g < 10 ? 2 : 1) + " г · " + fmtq(p.filament_mm / 1000, 2) + " м");
  if (p.cost && *p.cost > 0) rows.emplace_back("Стоимость", fmtq(*p.cost, 2));
  if (source_ && source_->support_g > 0) rows.emplace_back("Поддержки", fmtq(source_->support_g, 1) + " г");
  rows.emplace_back("Слоёв", QString::number(p.layers.size()));
  rows.emplace_back("Нарезано", p.slicer == "Kika" ? QString("здесь") : (p.slicer.empty() ? QString("—") : esc(p.slicer)));
  QString t = "<table cellspacing='0' cellpadding='0'>";
  for (const auto& [k, val] : rows)
    t += "<tr><td style='color:#61646b;padding:1px 12px 1px 0'>" + k + "</td><td style='padding:1px 0'>" + val + "</td></tr>";
  totals_->setText(t + "</table>");
  if (source_)
    for (const auto& w : source_->warnings) preview_warns_->addWidget(make_warn(qs(w)));
  render_legend();
}

void MainWindow::render_legend() {
  clear_layout(legend_list_);
  if (!preview_ || preview_->empty()) return;
  const auto lg = app::preview_legend(*preview_, preview_view_);
  auto swatch = [](const app::Rgb& c) {
    auto* d = new QLabel;
    d->setFixedSize(12, 12);
    d->setStyleSheet(QString("background:%1;border-radius:3px;").arg(QColor::fromRgbF(c[0], c[1], c[2]).name()));
    return d;
  };
  if (preview_view_.color == app::ColorBy::Type) {
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(6);
    grid->setVerticalSpacing(3);
    int r = 0;
    grid->addWidget(make_label("тип линии", "propHead", false), r, 2);
    grid->addWidget(make_label("время", "propHead", false), r, 3, Qt::AlignRight);
    grid->addWidget(make_label("%", "propHead", false), r, 4, Qt::AlignRight);
    grid->addWidget(make_label("пластик", "propHead", false), r, 5, Qt::AlignRight);
    ++r;
    // время коротко: «41 с», «12 мин», «1 ч 05 мин»
    auto short_time = [](const app::Preview& pv, app::LineType t) {
      const auto* st = pv.stat(t);
      const double sec = st ? st->time : 0;
      if (sec < 60) return QString("%1 с").arg(std::lround(sec));
      const long long m = std::llround(sec / 60);
      return m < 60 ? QString("%1 мин").arg(m) : QString("%1 ч %2 мин").arg(m / 60).arg(m % 60, 2, 10, QChar('0'));
    };
    for (const auto& e : lg.entries) {
      auto* box = new QCheckBox;
      box->setChecked(e.visible);
      box->setToolTip(e.visible ? "Скрыть" : "Показать");
      const auto type = *e.type;
      connect(box, &QCheckBox::toggled, this, [this, type](bool on) {
        preview_view_.visible[static_cast<std::size_t>(type)] = on;
        pview_->set_preview(preview_, preview_view_);
        apply_layers();
      });
      grid->addWidget(box, r, 0);
      grid->addWidget(swatch(e.color), r, 1);
      auto* name = new ElideLabel(qs(e.label));
      name->setObjectName("kv");
      grid->addWidget(name, r, 2);
      grid->addWidget(make_label(short_time(*preview_, type), "kv", false), r, 3, Qt::AlignRight);
      QString share = qs(e.share);
      share.remove(" %");
      grid->addWidget(make_label(share, "kv", false), r, 4, Qt::AlignRight);
      grid->addWidget(make_label(qs(e.grams), "kv", false), r, 5, Qt::AlignRight);
      ++r;
    }
    grid->setColumnStretch(2, 1);
    legend_list_->addLayout(grid);
  } else {
    for (const auto& e : lg.entries) {
      auto* row = hbox(8);
      row->addWidget(swatch(e.color));
      row->addWidget(make_label(qs(e.label), "kv", false), 1);
      legend_list_->addLayout(row);
    }
  }
}

void MainWindow::apply_layers() {
  if (!pview_) return;
  if (!preview_ || preview_->empty()) {
    pview_->set_layers(0, 0);
    move_slider_->setEnabled(false);
    move_label_->clear();
    pview_->set_info({});
    return;
  }
  const auto& p = *preview_;
  const std::size_t lo = static_cast<std::size_t>(layer_slider_->lower()), hi = static_cast<std::size_t>(layer_slider_->upper());
  const auto& L = p.layers[std::min(hi, p.layers.size() - 1)];
  const int n = static_cast<int>(L.last - L.first);
  {
    const QSignalBlocker block(move_slider_);
    const bool was_end = move_slider_->value() >= move_slider_->maximum();
    move_slider_->setRange(1, std::max(1, n));
    if (was_end || move_slider_->value() > n) move_slider_->setValue(std::max(1, n));
    move_slider_->setEnabled(n > 1);
  }
  const int k = move_slider_->value();
  std::optional<std::size_t> upto;
  if (k < n) upto = L.first + static_cast<std::size_t>(k) - 1;
  pview_->set_layers(lo, hi, upto);
  if (upto) {
    const auto& mv = p.moves[*upto];
    move_label_->setText(QString("%1 из %2 · %3 · %4 мм/с").arg(k).arg(n).arg(qs(std::string(app::line_type_name(mv.type))),
                                                                     fmtq(mv.speed, 0)));
  } else {
    move_label_->setText(QString("весь слой · %1 движений").arg(n));
  }
  pview_->set_info({QString("Слой %1 из %2 · Z %3 мм").arg(hi + 1).arg(p.layers.size()).arg(fmtq(L.z, 2)),
                    QString("высота %1 мм · время слоя %2").arg(fmtq(L.height, 2), qs(app::fmt_duration(L.time)))});
  layer_info_->setText(lo > 0 ? QString("Показаны слои %1–%2.").arg(lo + 1).arg(hi + 1) : QString());
}

void MainWindow::send_to_printer(bool start) {
  if (!preview_ || preview_->empty() || busy_) return;
  update_printer_source();
  if (!printer_->connected() && !printer_->has_address()) {
    set_tab(Tab::Printer);
    toast("Подключите принтер: впишите его адрес (с экрана принтера) или найдите в сети.");
    return;
  }
  toast(start ? "Отправляю на принтер…" : "Отправляю G-code на принтер…");
  printer_->send_now(start, [this](bool ok) {
    if (!ok) {
      toast("Не отправлено — подробности во вкладке «Принтер».", true);
      return;
    }
    toast("G-code на принтере.");
    if (auto_switch_->isChecked()) set_tab(Tab::Printer);
  });
}

// ============================================================================ принтер

QWidget* MainWindow::build_printer_page() {
  auto* page = new QWidget;
  auto* h = new QHBoxLayout(page);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(0);
  printer_ = new PrinterPanel;
  auto* sc = side_scroll(printer_, 400);
  sc->setObjectName("printerSide");
  h->addWidget(sc);
  web_ = new WebPane;
  h->addWidget(web_, 1);
  printer_->on_web = [this](const QString& url) { web_->set_url(url); };
  return page;
}

}  // namespace kika::gui
