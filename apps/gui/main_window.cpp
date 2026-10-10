// Главное окно. Логика повторяет fdmfea/web/app.js: те же шаги, подписи, подсказки и проверки.
// Сигналы подключаются лямбдами (без moc), тяжёлая работа — в QThread, результат возвращается
// в окно через очередь событий.

#include "main_window.hpp"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QMimeData>
#include <QPainter>
#include <QPolygonF>
#include <QPixmap>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QSplitter>
#include <QStyle>
#include <QThread>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>

#include "kika/gcode/parser.hpp"
#include "kika/material/material.hpp"
#include "kika/report/report.hpp"
#include "kika/version.hpp"
#include "viewer.hpp"

namespace kika::gui {

namespace {

using app::Field;

struct DetailLevel {
  long long n;
  const char* label;
};
constexpr DetailLevel kDetail[] = {
    {25000, "черновая"}, {60000, "обычная"}, {120000, "точная"}, {220000, "очень точная"}, {350000, "максимальная"}};
constexpr int kDetailCount = static_cast<int>(std::size(kDetail));

int nearest_detail(long long n) {
  int best = 1;
  double best_d = 1e300;
  for (int i = 0; i < kDetailCount; ++i) {
    const double d = std::abs(std::log(static_cast<double>(std::max(1LL, n)) / static_cast<double>(kDetail[i].n)));
    if (d < best_d) {
      best_d = d;
      best = i;
    }
  }
  return best;
}

struct FieldButton {
  Field field;
  const char* name;
};
constexpr FieldButton kFieldsPre[] = {
    {Field::Model, "Модель"}, {Field::Structure, "Структура печати"}, {Field::Density, "Плотность"}};
constexpr FieldButton kFieldsRes[] = {{Field::SafetyFactor, "Запас прочности"}, {Field::Stress, "Напряжения"},
                                      {Field::Displacement, "Перемещения"},     {Field::Mode, "Что разрушится"},
                                      {Field::Model, "Нагрузки"},               {Field::Structure, "Структура"}};

std::optional<Field> field_from_key(const std::string& k) {
  if (k == "model") return Field::Model;
  if (k == "structure") return Field::Structure;
  if (k == "density") return Field::Density;
  if (k == "sf") return Field::SafetyFactor;
  if (k == "stress") return Field::Stress;
  if (k == "disp") return Field::Displacement;
  if (k == "mode") return Field::Mode;
  return std::nullopt;
}

bool setup_field(Field f) { return f == Field::Model || f == Field::Structure || f == Field::Density; }

const char* verdict_name(const std::string& v) {
  if (v == "ok") return "Выдержит";
  if (v == "risk") return "Мало запаса";
  return "Разрушится";
}

// Свойства материала в таблице: строка, ключи вдоль нити / поперёк / по Z.
struct PropRow {
  const char* title;
  const char* keys[3];
};
constexpr PropRow kPropRows[] = {
    {"Модуль упругости, МПа", {"E1", "E2", "E3"}},
    {"Растяжение, МПа", {"Xt", "Yt", "Zt"}},
    {"Сжатие, МПа", {"Xc", "Yc", "Zc"}},
    {"Сдвиг, МПа", {nullptr, "S12", "S13"}},
};

std::optional<double> material_prop(const material::Material& m, const std::string& k) {
  using M = material::Material;
  static const std::pair<const char*, double M::*> props[] = {
      {"E1", &M::E1}, {"E2", &M::E2}, {"E3", &M::E3}, {"Xt", &M::Xt},   {"Yt", &M::Yt},   {"Zt", &M::Zt},
      {"Xc", &M::Xc}, {"Yc", &M::Yc}, {"Zc", &M::Zc}, {"S12", &M::S12}, {"S13", &M::S13}, {"density", &M::density},
      {"hdt", &M::hdt}};
  for (const auto& [key, ptr] : props)
    if (k == key) return m.*ptr;
  return std::nullopt;
}

// Отмена фоновой работы при закрытии окна.
struct Cancelled {};

QString qs(const std::string& s) { return QString::fromStdString(s); }
std::string ss(const QString& s) { return s.toStdString(); }
QString esc(const std::string& s) { return qs(s).toHtmlEscaped(); }
QString fmtq(double v, int d = -1) { return qs(app::fmt(v, d)); }

std::filesystem::path to_path(const QString& s) {
#ifdef _WIN32
  return std::filesystem::path(s.toStdWString());
#else
  return std::filesystem::path(s.toStdString());
#endif
}

// Число из поля ввода: запятая или точка, пробелы между разрядами допустимы.
std::optional<double> parse_num(const QString& text) {
  QString t = text.trimmed();
  t.remove(QChar(' '));
  t.remove(QChar(0x00A0));
  t.remove(QChar(0x202F));
  t.replace(QChar(','), QChar('.'));
  t.replace(QChar(0x2212), QChar('-'));  // «−» из подписей
  if (t.isEmpty()) return std::nullopt;
  bool ok = false;
  const double v = t.toDouble(&ok);
  if (!ok || !std::isfinite(v)) return std::nullopt;
  return v;
}

QString num_text(double v) { return QString::number(v, 'g', 12).replace(QChar('.'), QChar(',')); }

QString ru_int(long long n) { return QLocale(QLocale::Russian).toString(n); }

void repolish(QWidget* w) {
  w->style()->unpolish(w);
  w->style()->polish(w);
  w->update();
}

void set_bad(QWidget* w, bool bad) {
  if (w->property("bad").toBool() == bad) return;
  w->setProperty("bad", bad);
  repolish(w);
}

// Удалить всё из раскладки. Виджеты удаляются позже (может идти обработка их же сигнала),
// а сигналы глушатся сразу, чтобы скрытие не вызвало повторной обработки.
void clear_layout(QLayout* l) {
  while (QLayoutItem* it = l->takeAt(0)) {
    if (QWidget* w = it->widget()) {
      w->blockSignals(true);
      for (QObject* c : w->findChildren<QObject*>()) c->blockSignals(true);
      w->hide();
      w->deleteLater();
      delete it;
    } else if (QLayout* sub = it->layout()) {
      clear_layout(sub);
      delete sub;
    } else {
      delete it;
    }
  }
}

// Колесо мыши не меняет значение, пока поле не в фокусе: иначе прокрутка панели
// случайно переключает материал или направление.
template <class W>
class Calm : public W {
 public:
  explicit Calm(QWidget* parent = nullptr) : W(parent) { this->setFocusPolicy(Qt::StrongFocus); }

 protected:
  void wheelEvent(QWheelEvent* e) override {
    if (this->hasFocus())
      W::wheelEvent(e);
    else
      e->ignore();
  }
};

// Список, который сжимается вместе с панелью (иначе его ширина — по самому длинному пункту).
// Стрелка рисуется здесь: таблица стилей задаёт рамку списка, а стрелку без картинки задать нельзя.
class Combo : public Calm<QComboBox> {
 public:
  Combo() {
    setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    setMinimumContentsLength(4);
  }

 protected:
  void paintEvent(QPaintEvent* e) override {
    Calm<QComboBox>::paintEvent(e);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(isEnabled() ? QColor(0x61, 0x64, 0x6b) : QColor(0xb9, 0xba, 0xb6), 1.6, Qt::SolidLine, Qt::RoundCap,
                  Qt::RoundJoin));
    const QPointF c(width() - 12.5, height() / 2.0 + 0.5);
    p.drawPolyline(QPolygonF({c + QPointF(-4, -2), c + QPointF(0, 2), c + QPointF(4, -2)}));
  }
};

class Slider : public Calm<QSlider> {
 public:
  Slider(int lo, int hi, int value) {
    setOrientation(Qt::Horizontal);
    setRange(lo, hi);
    setValue(value);
  }
};

// Рамка с обратными вызовами наведения и щелчка.
class Frame : public QFrame {
 public:
  explicit Frame(const char* name) { setObjectName(name); }
  std::function<void(bool)> on_hover;
  std::function<void()> on_click;

 protected:
  void enterEvent(QEnterEvent* e) override {
    if (on_hover) on_hover(true);
    QFrame::enterEvent(e);
  }
  void leaveEvent(QEvent* e) override {
    if (on_hover) on_hover(false);
    QFrame::leaveEvent(e);
  }
  void mousePressEvent(QMouseEvent* e) override {
    QFrame::mousePressEvent(e);
    if (on_click && e->button() == Qt::LeftButton) on_click();
  }
};

QLabel* make_label(const QString& text, const char* name = nullptr, bool wrap = true) {
  auto* l = new QLabel(text);
  if (name) l->setObjectName(name);
  l->setWordWrap(wrap);
  return l;
}

QLabel* make_hint(const QString& text) { return make_label(text, "hint"); }
QLabel* make_warn(const QString& text) { return make_label(text, "warn"); }

QPushButton* make_button(const QString& text, const char* cls = nullptr) {
  auto* b = new QPushButton(text);
  if (cls) b->setProperty("cls", cls);
  b->setCursor(Qt::PointingHandCursor);
  return b;
}

QPushButton* make_icon_button(const QString& text, const QString& tip) {
  auto* b = make_button(text, "icon");
  b->setToolTip(tip);
  b->setFocusPolicy(Qt::TabFocus);
  return b;
}

QLabel* make_dot(const app::Rgb& c) {
  auto* d = new QLabel;
  d->setFixedSize(10, 10);
  d->setStyleSheet(QString("background:%1;border-radius:3px;").arg(QColor::fromRgbF(c[0], c[1], c[2]).name()));
  return d;
}

// Поле: подпись сверху, ввод снизу.
QWidget* make_field(const QString& title, QWidget* input) {
  auto* w = new QWidget;
  auto* v = new QVBoxLayout(w);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(4);
  v->addWidget(make_label(title, "fieldLabel"));
  v->addWidget(input);
  return w;
}

QLineEdit* make_num(std::optional<double> v) {
  auto* e = new QLineEdit(v ? num_text(*v) : QString());
  e->setObjectName("num");
  e->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  return e;
}

Combo* make_combo(const std::vector<std::pair<const char*, const char*>>& items, const std::string& cur) {
  auto* c = new Combo;
  for (const auto& [key, name] : items) c->addItem(QString::fromUtf8(name), QString::fromUtf8(key));
  const int i = c->findData(qs(cur));
  if (i >= 0) c->setCurrentIndex(i);
  return c;
}

std::string combo_key(const QComboBox* c) { return ss(c->currentData().toString()); }

QFrame* make_step() {
  auto* f = new QFrame;
  f->setObjectName("step");
  auto* v = new QVBoxLayout(f);
  v->setContentsMargins(16, 14, 16, 14);
  v->setSpacing(10);
  return f;
}

QWidget* make_step_title(const QString& n, const QString& text) {
  auto* w = new QWidget;
  auto* h = new QHBoxLayout(w);
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(9);
  if (!n.isEmpty()) {
    auto* num = new QLabel(n);
    num->setObjectName("stepNum");
    num->setAlignment(Qt::AlignCenter);
    num->setFixedSize(20, 20);
    h->addWidget(num);
  }
  h->addWidget(make_label(text, "stepTitle", false));
  h->addStretch();
  return w;
}

QHBoxLayout* hbox(int spacing = 8) {
  auto* h = new QHBoxLayout;
  h->setContentsMargins(0, 0, 0, 0);
  h->setSpacing(spacing);
  return h;
}

QWidget* wrap_layout(QLayout* l) {
  auto* w = new QWidget;
  w->setLayout(l);
  return w;
}

// Сегментированная группа кнопок.
QFrame* make_seg(QHBoxLayout** layout) {
  auto* f = new QFrame;
  f->setObjectName("seg");
  auto* h = new QHBoxLayout(f);
  h->setContentsMargins(3, 3, 3, 3);
  h->setSpacing(2);
  *layout = h;
  return f;
}

QPushButton* make_seg_button(const QString& text) {
  auto* b = make_button(text, "seg");
  b->setCheckable(true);
  return b;
}

// Таблица «название — значение» для сводок.
QString kv_table(const std::vector<std::pair<QString, QString>>& rows) {
  QString h = "<table cellspacing='0' cellpadding='0'>";
  for (const auto& [k, v] : rows)
    h += "<tr><td style='color:#61646b;padding:1px 12px 1px 0'>" + k + "</td><td style='padding:1px 0'>" + v +
         "</td></tr>";
  return h + "</table>";
}

// JSON задания для файла: с отступами, но короткие массивы чисел (грани, векторы) — в одну строку.
void pretty_json(std::string& out, const json::Value& v, int level) {
  const std::string pad(static_cast<std::size_t>(2 * level), ' '), pad1(static_cast<std::size_t>(2 * level + 2), ' ');
  if (v.is_array()) {
    const auto& a = v.as_array();
    const bool flat = std::all_of(a.begin(), a.end(), [](const json::Value& x) { return !x.is_array() && !x.is_object(); });
    if (a.empty() || flat) {
      out += json::dump(v);
      return;
    }
    out += "[\n";
    for (std::size_t i = 0; i < a.size(); ++i) {
      out += pad1;
      pretty_json(out, a[i], level + 1);
      out += i + 1 < a.size() ? ",\n" : "\n";
    }
    out += pad + "]";
  } else if (v.is_object()) {
    const auto& o = v.as_object();
    if (o.empty()) {
      out += "{}";
      return;
    }
    out += "{\n";
    for (std::size_t i = 0; i < o.size(); ++i) {
      out += pad1 + json::dump(json::Value(o[i].first)) + ": ";
      pretty_json(out, o[i].second, level + 1);
      out += i + 1 < o.size() ? ",\n" : "\n";
    }
    out += pad + "}";
  } else {
    out += json::dump(v);
  }
}

void write_file(const QString& path, const std::string& data) {
  std::ofstream f(to_path(path), std::ios::binary);
  if (f) f.write(data.data(), static_cast<std::streamsize>(data.size()));
  if (!f) throw std::runtime_error("Не удалось записать файл " + ss(QDir::toNativeSeparators(path)) + ".");
}

std::optional<json::Value> read_json(const QString& path, QString* error) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly)) {
    *error = "Не удалось открыть файл " + QFileInfo(path).fileName() + ".";
    return std::nullopt;
  }
  const QByteArray data = f.readAll();
  try {
    return json::parse(std::string_view(data.constData(), static_cast<std::size_t>(data.size())));
  } catch (const std::exception& e) {
    *error = "Файл задания не читается: " + QString::fromUtf8(e.what());
    return std::nullopt;
  }
}

app::UiFixture* fixture_by_id(app::UiJob& job, int id) {
  for (auto& f : job.fixtures)
    if (f.id == id) return &f;
  return nullptr;
}

app::UiCase* case_by_id(app::UiJob& job, int id) {
  for (auto& c : job.cases)
    if (c.id == id) return &c;
  return nullptr;
}

app::UiLoad* load_by_id(app::UiJob& job, int id) {
  for (auto& c : job.cases)
    for (auto& l : c.loads)
      if (l.id == id) return &l;
  return nullptr;
}

std::vector<std::int64_t> merge_keys(const std::vector<std::int64_t>& a, const std::vector<std::int64_t>& b) {
  std::vector<std::int64_t> out;
  std::set_union(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

std::vector<std::int64_t> remove_keys(const std::vector<std::int64_t>& a, const std::vector<std::int64_t>& b) {
  std::vector<std::int64_t> out;
  std::set_difference(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
  return out;
}

QPixmap make_logo(int size) {
  QPixmap pm(size, size);
  pm.fill(Qt::transparent);
  QPainter p(&pm);
  p.setRenderHint(QPainter::Antialiasing);
  p.scale(size / 32.0, size / 32.0);
  p.setPen(Qt::NoPen);
  p.setBrush(QColor(0x16, 0x18, 0x1b));
  p.drawRoundedRect(QRectF(4, 20, 24, 5), 1.5, 1.5);
  p.drawRoundedRect(QRectF(7, 13, 18, 5), 1.5, 1.5);
  p.setBrush(QColor(0xeb, 0x68, 0x34));
  p.drawRoundedRect(QRectF(10, 6, 12, 5), 1.5, 1.5);
  return pm;
}

const QString kIntro =
    "Откройте G-code из Orca Slicer, Bambu Studio, PrusaSlicer или Cura. Нарезайте с теми настройками, с которыми "
    "будете печатать: стенки, заполнение и ориентация на столе влияют на прочность.";

}  // namespace

// ============================================================================ окно

MainWindow::MainWindow() {
  setWindowTitle("Kika — прочность печати");
  QIcon icon;
  for (int s : {16, 24, 32, 48, 64, 128, 256}) icon.addPixmap(make_logo(s));
  setWindowIcon(icon);
  setAcceptDrops(true);
  last_dir_ = QDir::homePath();
  build_ui();
  render_all();
  resize(1400, 900);

  // активный случай — тот, в котором фокус ввода
  connect(qApp, &QApplication::focusChanged, this, [this](QWidget*, QWidget* now) {
    for (QWidget* w = now; w; w = w->parentWidget()) {
      const QVariant id = w->property("case_id");
      if (!id.isValid()) continue;
      for (std::size_t i = 0; i < job_.cases.size(); ++i)
        if (job_.cases[i].id == id.toInt() && static_cast<int>(i) != active_case_) {
          active_case_ = static_cast<int>(i);
          update_case_frames();
          refresh_overlay();
        }
      return;
    }
  });
}

MainWindow::~MainWindow() {
  if (worker_) {
    *cancel_ = true;
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
  }
  // сцена ссылается на модель, вид — на сцену
  if (viewer_) viewer_->set_scene(nullptr);
}

void MainWindow::build_ui() {
  auto* central = new QWidget;
  central->setObjectName("central");
  auto* v = new QVBoxLayout(central);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);
  v->addWidget(build_top_bar());
  auto* split = new QSplitter(Qt::Horizontal);
  split->setChildrenCollapsible(false);
  split->setHandleWidth(1);
  split->addWidget(build_panel());
  split->addWidget(build_stage());
  split->setStretchFactor(0, 0);
  split->setStretchFactor(1, 1);
  split->setSizes({400, 1000});
  v->addWidget(split, 1);
  setCentralWidget(central);

  toast_ = new QLabel(central);
  toast_->setObjectName("toast");
  toast_->setWordWrap(true);
  toast_->setMaximumWidth(560);
  toast_->hide();
  toast_timer_ = new QTimer(this);
  toast_timer_->setSingleShot(true);
  connect(toast_timer_, &QTimer::timeout, toast_, &QLabel::hide);
}

QWidget* MainWindow::build_top_bar() {
  auto* bar = new QFrame;
  bar->setObjectName("topBar");
  auto* h = new QHBoxLayout(bar);
  h->setContentsMargins(16, 10, 16, 10);
  h->setSpacing(14);
  auto* logo = new QLabel;
  logo->setPixmap(make_logo(24));
  auto* brand = hbox(9);
  brand->addWidget(logo);
  brand->addWidget(make_label("Kika", "brand", false));
  h->addLayout(brand);
  file_label_ = make_label("файл не открыт", "fileName", false);
  file_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
  h->addWidget(file_label_, 1);

  btn_open_ = make_button("Открыть G-code", "primary");
  btn_example_ = make_button("Пример");
  btn_open_job_ = make_button("Открыть задание");
  btn_save_job_ = make_button("Сохранить задание");
  btn_report_ = make_button("Отчёт");
  auto* btn_about = make_button("О программе");
  btn_example_->setToolTip("Настенный кронштейн для лампы, напечатанный на боку, с готовым заданием");
  btn_open_job_->setToolTip("Задание на расчёт (.json): закрепления, нагрузки, материал");
  btn_report_->setToolTip("Отчёт с 3D-видом в одном файле HTML — открывается в любом браузере");
  for (auto* b : {btn_open_, btn_example_, btn_open_job_, btn_save_job_, btn_report_, btn_about}) h->addWidget(b);
  connect(btn_open_, &QPushButton::clicked, this, [this] { open_gcode_dialog(); });
  connect(btn_example_, &QPushButton::clicked, this, [this] { open_example(); });
  connect(btn_open_job_, &QPushButton::clicked, this, [this] { open_job_dialog(); });
  connect(btn_save_job_, &QPushButton::clicked, this, [this] { save_job(); });
  connect(btn_report_, &QPushButton::clicked, this, [this] { export_report(); });
  connect(btn_about, &QPushButton::clicked, this, [this] { about(); });
  return bar;
}

QWidget* MainWindow::build_panel() {
  panel_scroll_ = new QScrollArea;
  panel_scroll_->setObjectName("panel");
  panel_scroll_->setWidgetResizable(true);
  panel_scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
  panel_scroll_->setFrameShape(QFrame::NoFrame);
  panel_scroll_->setMinimumWidth(340);
  auto* inner = new QWidget;
  inner->setObjectName("panelInner");
  auto* v = new QVBoxLayout(inner);
  v->setContentsMargins(0, 4, 0, 24);
  v->setSpacing(0);

  // 1. Деталь
  {
    auto* s = make_step();
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    l->addWidget(make_step_title("1", "Деталь"));
    model_info_ = make_label(kIntro, "muted");
    model_info_->setTextFormat(Qt::RichText);
    l->addWidget(model_info_);
    model_warns_ = new QVBoxLayout;
    model_warns_->setSpacing(6);
    l->addLayout(model_warns_);
    mesh_ctl_ = new QWidget;
    auto* m = new QVBoxLayout(mesh_ctl_);
    m->setContentsMargins(0, 0, 0, 0);
    m->setSpacing(4);
    auto* head = hbox(6);
    head->addWidget(make_label("Детальность сетки", "fieldLabel", false));
    detail_label_ = make_label("", "hint", false);
    head->addWidget(detail_label_);
    head->addStretch();
    m->addLayout(head);
    auto* row = hbox();
    detail_slider_ = new Slider(0, kDetailCount - 1, detail_);
    detail_slider_->setPageStep(1);
    btn_rebuild_ = make_button("Перестроить", "sm");
    row->addWidget(detail_slider_, 1);
    row->addWidget(btn_rebuild_);
    m->addLayout(row);
    m->addWidget(make_hint("Точнее — дольше расчёт. Для первой оценки хватит «обычной»."));
    l->addWidget(mesh_ctl_);
    connect(detail_slider_, &QSlider::valueChanged, this, [this](int x) {
      detail_ = x;
      detail_label_->setText(kDetail[x].label);
    });
    connect(btn_rebuild_, &QPushButton::clicked, this, [this] { rebuild_mesh(); });
    v->addWidget(s);
  }

  // 2. Материал
  {
    auto* s = make_step();
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    l->addWidget(make_step_title("2", "Материал"));
    auto* row = hbox(10);
    mat_combo_ = new Combo;
    for (const auto& m : material::builtin_materials()) mat_combo_->addItem(qs(m.name), qs(m.key));
    target_edit_ = make_num(2.0);
    target_edit_->setFixedWidth(110);
    target_edit_->setToolTip("Во сколько раз деталь должна быть прочнее, чем нужно для заданной нагрузки (не меньше 1)");
    row->addWidget(make_field("Пластик", mat_combo_), 1);
    row->addWidget(make_field("Нужный запас", target_edit_));
    l->addLayout(row);
    mat_note_ = make_hint("");
    l->addWidget(mat_note_);
    auto* toggle = make_button("▸ Свойства материала", "link");
    toggle->setCheckable(true);
    l->addWidget(toggle, 0, Qt::AlignLeft);
    prop_box_ = new QWidget;
    auto* pb = new QVBoxLayout(prop_box_);
    pb->setContentsMargins(0, 0, 0, 0);
    pb->setSpacing(8);
    prop_grid_ = new QGridLayout;
    prop_grid_->setHorizontalSpacing(6);
    prop_grid_->setVerticalSpacing(4);
    pb->addLayout(prop_grid_);
    pb->addWidget(make_hint("Значения по умолчанию — типовые для хорошей печати. Если испытали свои образцы, "
                            "впишите свои числа."));
    prop_box_->hide();
    l->addWidget(prop_box_);
    connect(toggle, &QPushButton::toggled, this, [this, toggle](bool on) {
      prop_box_->setVisible(on);
      toggle->setText(on ? "▾ Свойства материала" : "▸ Свойства материала");
    });
    connect(mat_combo_, &QComboBox::currentIndexChanged, this, [this](int) {
      job_.material = combo_key(mat_combo_);
      job_.overrides.clear();
      render_material();
      mark_dirty();
    });
    connect(target_edit_, &QLineEdit::textEdited, this, [this](const QString& t) {
      const auto x = parse_num(t);
      set_bad(target_edit_, !x || *x < 1);
      if (!x || *x < 1) return;
      job_.target_sf = *x;
      mark_dirty();
      if (results_) apply_view();
    });
    v->addWidget(s);
  }

  // 3. Закрепления
  {
    auto* s = make_step();
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    l->addWidget(make_step_title("3", "Закрепления"));
    l->addWidget(make_hint("Выберите на модели поверхности, которыми деталь крепится (отверстия под винты, прижатую "
                           "грань), и нажмите кнопку."));
    fix_list_ = new QVBoxLayout;
    fix_list_->setSpacing(8);
    l->addLayout(fix_list_);
    btn_add_fix_ = make_button("+ Закрепить выбранное", "add");
    l->addWidget(btn_add_fix_);
    connect(btn_add_fix_, &QPushButton::clicked, this, [this] {
      if (sel_.empty()) return;
      app::UiFixture f;
      f.id = job_.new_id();
      f.name = "Закрепление " + std::to_string(job_.fixtures.size() + 1);
      f.faces = sel_;
      job_.fixtures.push_back(std::move(f));
      sel_.clear();
      render_fixtures();
      update_selection();
      mark_dirty();
    });
    v->addWidget(s);
  }

  // 4. Нагрузки
  {
    auto* s = make_step();
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    l->addWidget(make_step_title("4", "Нагрузки"));
    l->addWidget(make_hint("Каждый случай считается отдельно: например, «вес лампы» и «случайный удар»."));
    case_list_ = new QVBoxLayout;
    case_list_->setSpacing(10);
    l->addLayout(case_list_);
    auto* add = make_button("+ Расчётный случай", "add");
    l->addWidget(add);
    connect(add, &QPushButton::clicked, this, [this] {
      app::UiCase c;
      c.id = job_.new_id();
      c.name = "Случай " + std::to_string(job_.cases.size() + 1);
      job_.cases.push_back(std::move(c));
      active_case_ = static_cast<int>(job_.cases.size()) - 1;
      render_cases();
      refresh_overlay();
      mark_dirty();
    });
    v->addWidget(s);
  }

  // Расчёт
  {
    auto* s = make_step();
    s->setObjectName("stepRun");
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    btn_run_ = make_button("Рассчитать", "big");
    l->addWidget(btn_run_);
    progress_box_ = new QWidget;
    auto* pb = new QVBoxLayout(progress_box_);
    pb->setContentsMargins(0, 0, 0, 0);
    pb->setSpacing(6);
    progress_ = new QProgressBar;
    progress_->setRange(0, 1000);
    progress_->setTextVisible(false);
    progress_->setFixedHeight(6);
    progress_text_ = make_hint("");
    pb->addWidget(progress_);
    pb->addWidget(progress_text_);
    progress_box_->hide();
    l->addWidget(progress_box_);
    run_hint_ = make_hint("Нужны деталь, хотя бы одно закрепление и нагрузка.");
    l->addWidget(run_hint_);
    connect(btn_run_, &QPushButton::clicked, this, [this] { run_analysis(); });
    v->addWidget(s);
  }

  // Результаты
  {
    auto* s = make_step();
    auto* l = static_cast<QVBoxLayout*>(s->layout());
    l->addWidget(make_step_title("", "Результаты"));
    results_list_ = new QVBoxLayout;
    results_list_->setSpacing(10);
    l->addLayout(results_list_);
    results_box_ = s;
    results_box_->hide();
    v->addWidget(s);
  }
  v->addStretch(1);
  panel_scroll_->setWidget(inner);
  return panel_scroll_;
}

QWidget* MainWindow::build_stage() {
  auto* stage = new QWidget;
  auto* v = new QVBoxLayout(stage);
  v->setContentsMargins(0, 0, 0, 0);
  v->setSpacing(0);

  // выбор поверхностей
  {
    auto* bar = new QFrame;
    bar->setObjectName("pickBar");
    auto* h = new QHBoxLayout(bar);
    h->setContentsMargins(12, 8, 12, 8);
    h->setSpacing(12);
    QHBoxLayout* seg = nullptr;
    auto* segf = make_seg(&seg);
    tools_ = new QButtonGroup(this);
    tools_->setExclusive(true);
    const std::pair<const char*, const char*> tools[] = {
        {"Грань", "Плоская грань целиком"}, {"Отверстие", "Стенка отверстия или цилиндра"}, {"Кисть", "Всё в радиусе от точки"}};
    int id = 0;
    for (const auto& [name, tip] : tools) {
      auto* b = make_seg_button(name);
      b->setToolTip(tip);
      b->setChecked(id == 0);
      tools_->addButton(b, id++);
      seg->addWidget(b);
    }
    h->addWidget(segf);
    brush_ctl_ = new QWidget;
    auto* bh = hbox(7);
    bh->addWidget(make_label("радиус", "ctl", false));
    auto* brush = new Slider(1, 30, static_cast<int>(brush_r_));
    brush->setFixedWidth(110);
    bh->addWidget(brush);
    brush_label_ = make_label("4 мм", "ctl", false);
    bh->addWidget(brush_label_);
    brush_ctl_->setLayout(bh);
    brush_ctl_->hide();
    h->addWidget(brush_ctl_);
    sel_info_ = make_label("", "selInfo", false);
    sel_info_->setTextFormat(Qt::RichText);
    sel_info_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    h->addWidget(sel_info_, 1);
    btn_clear_sel_ = make_button("Снять выбор", "sm");
    h->addWidget(btn_clear_sel_);
    // готовые виды
    auto* views = hbox(4);
    const std::pair<const char*, const char*> view_names[] = {
        {"iso", "Изо"}, {"front", "Спереди"}, {"top", "Сверху"}, {"right", "Справа"}};
    for (const auto& [key, name] : view_names) {
      auto* b = make_button(name, "sm");
      const std::string k = key;
      connect(b, &QPushButton::clicked, this, [this, k] { viewer_->view(k); });
      views->addWidget(b);
    }
    h->addSpacing(8);
    h->addLayout(views);
    v->addWidget(bar);
    connect(tools_, &QButtonGroup::idClicked, this, [this](int t) {
      tool_ = t == 0 ? "plane" : (t == 1 ? "hole" : "brush");
      brush_ctl_->setVisible(t == 2);
    });
    connect(brush, &QSlider::valueChanged, this, [this](int r) {
      brush_r_ = r;
      brush_label_->setText(QString::number(r) + " мм");
    });
    connect(btn_clear_sel_, &QPushButton::clicked, this, [this] {
      sel_.clear();
      update_selection();
    });
  }

  viewer_ = new Viewer;
  viewer_->set_empty_text(
      "Перетащите сюда файл G-code\n\nили откройте пример — настенный кронштейн для лампы, напечатанный на боку");
  viewer_->on_pick = [this](const std::optional<app::Hit>& hit, Qt::KeyboardModifiers mods) { on_pick(hit, mods); };
  v->addWidget(viewer_, 1);

  // что показать
  {
    auto* bar = new QFrame;
    bar->setObjectName("viewBar");
    auto* vb = new QVBoxLayout(bar);
    vb->setContentsMargins(12, 8, 12, 8);
    vb->setSpacing(8);
    auto* row1 = hbox(12);
    fields_box_ = make_seg(&fields_layout_);
    row1->addWidget(fields_box_);
    row1->addStretch(1);
    vb->addLayout(row1);

    auto* row2 = hbox(12);
    deform_ctl_ = new QWidget;
    auto* dh = hbox(7);
    deform_on_ = new QCheckBox("деформация ×");
    deform_on_->setObjectName("ctl");
    deform_val_ = make_label("—", "ctlNum", false);
    deform_val_->setMinimumWidth(30);
    deform_slider_ = new Slider(0, 100, 50);
    deform_slider_->setFixedWidth(110);
    dh->addWidget(deform_on_);
    dh->addWidget(deform_val_);
    dh->addWidget(deform_slider_);
    deform_ctl_->setLayout(dh);
    // нити из G-code или расчётная сетка
    QHBoxLayout* dseg = nullptr;
    auto* dsegf = make_seg(&dseg);
    display_ = new QButtonGroup(this);
    display_->setExclusive(true);
    const std::pair<const char*, const char*> displays[] = {
        {"Нити", "Валики из G-code — как будет напечатано"},
        {"Сетка", "Воксели расчётной сетки — по ним идёт расчёт и выбираются поверхности"}};
    int did = 0;
    for (const auto& [name, tip] : displays) {
      auto* b = make_seg_button(name);
      b->setToolTip(tip);
      b->setChecked(did == 0);
      display_->addButton(b, did++);
      dseg->addWidget(b);
    }
    connect(display_, &QButtonGroup::idClicked, this, [this](int d) {
      viewer_->set_display(d == 0 ? Viewer::Display::Roads : Viewer::Display::Voxels);
    });
    row1->addWidget(dsegf);
    row2->addWidget(deform_ctl_);
    auto* sh = hbox(7);
    sh->addWidget(make_label("разрез", "ctl", false));
    sec_axis_ = new Combo;
    for (const char* a : {"нет", "X", "Y", "Z"}) sec_axis_->addItem(a);
    sec_pos_ = new Slider(0, 1000, 500);
    sec_pos_->setFixedWidth(130);
    sec_pos_->setEnabled(false);
    sh->addWidget(sec_axis_);
    sh->addWidget(sec_pos_);
    row2->addLayout(sh);
    btn_weak_ = make_button("Слабое место", "sm");
    btn_weak_->setToolTip("Разрез по слою через слабое место");
    row2->addWidget(btn_weak_);
    row2->addStretch(1);
    vb->addLayout(row2);
    v->addWidget(bar);
    connect(deform_on_, &QCheckBox::toggled, this, [this](bool) { apply_deform(); });
    connect(deform_slider_, &QSlider::valueChanged, this, [this](int) { apply_deform(); });
    connect(sec_axis_, &QComboBox::currentIndexChanged, this, [this](int) { apply_section(); });
    connect(sec_pos_, &QSlider::valueChanged, this, [this](int) { apply_section(); });
    connect(btn_weak_, &QPushButton::clicked, this, [this] { show_weak_spot(); });
  }
  return stage;
}

// ============================================================================ панель

void MainWindow::render_all() {
  render_model_info();
  render_material();
  render_fixtures();
  render_cases();
  render_results();
  render_fields();
  update_selection();
  apply_view();
  update_run_state();
}

void MainWindow::render_model_info() {
  mesh_ctl_->setVisible(model_ != nullptr);
  {
    const QSignalBlocker block(detail_slider_);
    detail_slider_->setValue(detail_);
  }
  detail_label_->setText(kDetail[detail_].label);
  clear_layout(model_warns_);
  if (!model_) {
    model_info_->setText(kIntro);
    model_info_->setObjectName("muted");
    repolish(model_info_);
    return;
  }
  const auto s = analysis::model_summary(*model_);
  auto str = [&](const char* k) { return s.find(k)->is_string() ? s.find(k)->as_string() : std::string(); };
  const std::string slicer = str("slicer"), ft = str("filament_type"), pattern = str("infill_pattern");
  QStringList size;
  for (const auto& x : s.find("size")->as_array()) size << fmtq(x.as_double(), 1);
  std::vector<std::pair<QString, QString>> rows = {
      {"Слайсер", slicer == "unknown" || slicer.empty() ? "—" : esc(slicer)},
      {"Пластик", ft.empty() ? "—" : esc(ft)},
      {"Слой", fmtq(s.find("layer_height")->as_double(), 2) + " мм"},
      {"Заполнение", QString::number(std::lround(s.find("infill_density")->as_double() * 100)) + "% " + esc(pattern)},
      {"Габарит", size.join(" × ") + " мм"},
      {"Сетка", fmtq(s.find("voxel")->as_double(), 2) + " мм · " + ru_int(s.find("elements")->as_int()) + " эл."},
  };
  model_info_->setText(kv_table(rows));
  model_info_->setObjectName("");
  repolish(model_info_);
  for (const auto& w : s.find("warnings")->as_array()) model_warns_->addWidget(make_warn(qs(w.as_string())));
  const double removed = s.find("removed_fraction")->as_double();
  if (removed > 0.01)
    model_warns_->addWidget(make_warn("Отброшено " + QString::number(std::lround(removed * 100)) +
                                      "% материала, не связанного с основной деталью (другие объекты на столе или "
                                      "мусор)."));
}

void MainWindow::render_material() {
  {
    const QSignalBlocker block(mat_combo_);
    const int i = mat_combo_->findData(qs(job_.material));
    mat_combo_->setCurrentIndex(i >= 0 ? i : 0);
  }
  {
    target_edit_->setText(num_text(job_.target_sf));
    set_bad(target_edit_, false);
  }
  const auto m = app::job_material(job_);
  mat_note_->setText(qs(m.note));
  mat_note_->setVisible(!m.note.empty());
  clear_layout(prop_grid_);
  int row = 0;
  const char* heads[] = {"вдоль нити", "поперёк", "по Z"};
  for (int c = 0; c < 3; ++c) {
    auto* h = make_label(heads[c], "propHead", false);
    h->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    prop_grid_->addWidget(h, row, c + 1);
  }
  ++row;
  auto add_prop = [&](const char* key, int r, int c) {
    const auto val = material_prop(m, key);
    auto* e = make_num(val);
    e->setFixedWidth(62);
    e->setObjectName("propEdit");
    const QString shown = e->text();
    const std::string k = key;
    connect(e, &QLineEdit::editingFinished, this, [this, e, k, shown] {
      if (e->text() == shown) return;
      const auto x = parse_num(e->text());
      if (!x || !(*x > 0)) {
        toast("Значение должно быть больше нуля.", true);
        render_material();
        return;
      }
      auto it = std::find_if(job_.overrides.begin(), job_.overrides.end(), [&](const auto& p) { return p.first == k; });
      if (it != job_.overrides.end())
        it->second = *x;
      else
        job_.overrides.emplace_back(k, *x);
      render_material();
      mark_dirty();
    });
    prop_grid_->addWidget(e, r, c);
  };
  for (const auto& pr : kPropRows) {
    prop_grid_->addWidget(make_label(pr.title, "propName"), row, 0);
    for (int c = 0; c < 3; ++c) {
      if (pr.keys[c])
        add_prop(pr.keys[c], row, c + 1);
      else {
        auto* dash = make_label("—", "propHead", false);
        dash->setAlignment(Qt::AlignCenter);
        prop_grid_->addWidget(dash, row, c + 1);
      }
    }
    ++row;
  }
  prop_grid_->addWidget(make_label("Плотность, г/см³", "propName"), row, 0);
  add_prop("density", row, 1);
  auto* hdt = make_label("HDT, °C", "propName", false);
  hdt->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
  hdt->setToolTip("Температура, при которой пластик начинает размягчаться под нагрузкой");
  prop_grid_->addWidget(hdt, row, 2);
  add_prop("hdt", row, 3);
  prop_grid_->setColumnStretch(0, 1);
}

void MainWindow::render_fixtures() {
  clear_layout(fix_list_);
  fix_reselect_.clear();
  for (const auto& f : job_.fixtures) {
    const int id = f.id;
    auto* fr = new Frame("item");
    auto* v = new QVBoxLayout(fr);
    v->setContentsMargins(10, 9, 10, 9);
    v->setSpacing(8);
    auto* head = hbox(8);
    head->addWidget(make_dot(app::palette::kFixture));
    auto* name = new QLineEdit(qs(f.name));
    name->setObjectName("itemName");
    head->addWidget(name, 1);
    auto* re = make_icon_button("⟲", "Заменить поверхность выбранной");
    re->setEnabled(!sel_.empty());
    fix_reselect_.emplace_back(re);
    auto* del = make_icon_button("✕", "Удалить");
    head->addWidget(re);
    head->addWidget(del);
    v->addLayout(head);
    auto* meta = make_label(faces_meta(f.faces), "meta");
    meta->setTextFormat(Qt::RichText);
    v->addWidget(meta);
    auto* chk = hbox(10);
    chk->addWidget(make_label("запрещено смещение по", "muted", false));
    std::array<QCheckBox*, 3> boxes{};
    for (int a = 0; a < 3; ++a) {
      const char c = "xyz"[a];
      boxes[static_cast<std::size_t>(a)] = new QCheckBox(QString(QChar(c)).toUpper());
      boxes[static_cast<std::size_t>(a)]->setChecked(f.components.find(c) != std::string::npos);
      chk->addWidget(boxes[static_cast<std::size_t>(a)]);
    }
    chk->addStretch();
    v->addLayout(chk);
    fix_list_->addWidget(fr);

    connect(name, &QLineEdit::textEdited, this, [this, id](const QString& t) {
      if (auto* x = fixture_by_id(job_, id)) x->name = ss(t);
    });
    for (auto* b : boxes)
      connect(b, &QCheckBox::toggled, this, [this, id, boxes](bool) {
        auto* x = fixture_by_id(job_, id);
        if (!x) return;
        x->components.clear();
        for (int a = 0; a < 3; ++a)
          if (boxes[static_cast<std::size_t>(a)]->isChecked()) x->components += "xyz"[a];
        if (x->components.empty()) toast("Закрепление без осей ничего не держит — отметьте хотя бы одну ось.", true);
        mark_dirty();
      });
    connect(re, &QPushButton::clicked, this, [this, id] {
      auto* x = fixture_by_id(job_, id);
      if (!x || sel_.empty()) return;
      x->faces = sel_;
      sel_.clear();
      render_fixtures();
      update_selection();
      mark_dirty();
    });
    connect(del, &QPushButton::clicked, this, [this, id] {
      std::erase_if(job_.fixtures, [id](const app::UiFixture& x) { return x.id == id; });
      if (hover_ && hover_->second == id) hover_.reset();
      render_fixtures();
      update_selection();
      mark_dirty();
    });
    fr->on_hover = [this, id](bool in) {
      const std::pair<char, int> key{'f', id};
      if (in)
        hover_ = key;
      else if (hover_ == key)
        hover_.reset();
      refresh_overlay();
    };
  }
}

QString MainWindow::faces_meta(const std::vector<std::int64_t>& faces) const {
  if (faces.empty()) return "<span style='color:#a35a00'>поверхность не выбрана</span>";
  return QString::number(faces.size()) + " гр. · " + (scene_ ? fmtq(scene_->area(faces), 0) : QString("?")) + " мм²";
}

void MainWindow::render_cases() {
  clear_layout(case_list_);
  case_frames_.clear();
  load_reselect_.clear();
  const bool several = job_.cases.size() > 1;
  for (std::size_t ci = 0; ci < job_.cases.size(); ++ci) {
    const auto& c = job_.cases[ci];
    const int cid = c.id;
    auto* fr = new Frame("case");
    fr->setProperty("case_id", cid);
    fr->on_click = [this, ci] {
      if (static_cast<int>(ci) == active_case_) return;
      active_case_ = static_cast<int>(ci);
      update_case_frames();
      refresh_overlay();
    };
    auto* v = new QVBoxLayout(fr);
    v->setContentsMargins(10, 10, 10, 10);
    v->setSpacing(10);

    auto* head = hbox(8);
    auto* name = new QLineEdit(qs(c.name));
    name->setObjectName("caseName");
    head->addWidget(name, 1);
    connect(name, &QLineEdit::textEdited, this, [this, cid](const QString& t) {
      if (auto* x = case_by_id(job_, cid)) x->name = ss(t);
    });
    if (several) {
      auto* del = make_icon_button("✕", "Удалить случай");
      head->addWidget(del);
      connect(del, &QPushButton::clicked, this, [this, cid] {
        std::erase_if(job_.cases, [cid](const app::UiCase& x) { return x.id == cid; });
        active_case_ = std::clamp(active_case_, 0, std::max(0, static_cast<int>(job_.cases.size()) - 1));
        render_cases();
        refresh_overlay();
        mark_dirty();
      });
    }
    v->addLayout(head);

    // общие параметры случая
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(8);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    auto* dur = make_combo(app::durations(), c.duration);
    auto* temp = make_num(c.temperature);
    grid->addWidget(make_field("Характер нагрузки", dur), 0, 0);
    grid->addWidget(make_field("Температура, °C", temp), 0, 1);
    int grow = 1;
    if (c.duration == "cyclic") {
      auto* cyc = make_num(c.cycles);
      grid->addWidget(make_field("Число циклов", cyc), grow++, 0, 1, 2);
      connect(cyc, &QLineEdit::textEdited, this, [this, cid, cyc](const QString& t) {
        const auto x = parse_num(t);
        set_bad(cyc, !x || !(*x >= 1));
        if (!x || !(*x >= 1)) return;
        if (auto* cs = case_by_id(job_, cid)) cs->cycles = *x;
        mark_dirty();
      });
    }
    auto* thermal = new QCheckBox("учитывать тепловое расширение");
    thermal->setToolTip("Если деталь зажата и нагревается, она расширяется и сама себя нагружает.");
    thermal->setChecked(c.thermal);
    grid->addWidget(thermal, grow, 0, 1, 2);
    v->addLayout(grid);
    connect(dur, &QComboBox::currentIndexChanged, this, [this, cid, dur](int) {
      if (auto* cs = case_by_id(job_, cid)) cs->duration = combo_key(dur);
      render_cases();
      mark_dirty();
    });
    connect(temp, &QLineEdit::textEdited, this, [this, cid, temp](const QString& t) {
      const auto x = parse_num(t);
      const bool ok = x || t.trimmed().isEmpty();
      set_bad(temp, !ok);
      if (!ok) return;
      if (auto* cs = case_by_id(job_, cid)) cs->temperature = x;
      mark_dirty();
    });
    connect(thermal, &QCheckBox::toggled, this, [this, cid](bool on) {
      if (auto* cs = case_by_id(job_, cid)) cs->thermal = on;
      mark_dirty();
    });

    // нагрузки
    for (const auto& l : c.loads) v->addWidget(build_load_item(l));

    // добавить нагрузку
    auto* add = hbox(6);
    auto* type = new Combo;
    for (const auto& t : app::load_types()) type->addItem(QString::fromUtf8(t.name), QString::fromUtf8(t.key));
    auto* add_btn = make_button("+ Нагрузка", "sm");
    add->addWidget(type, 1);
    add->addWidget(add_btn);
    v->addLayout(add);
    connect(add_btn, &QPushButton::clicked, this, [this, cid, type] {
      const std::string t = combo_key(type);
      const auto& info = app::load_type(t);
      if (info.faces && sel_.empty()) {
        toast("Сначала выберите на модели поверхность, к которой приложена нагрузка.", true);
        return;
      }
      auto l = app::default_load(t, job_.new_id());
      if (info.faces) {
        l.faces = sel_;
        sel_.clear();
      }
      for (std::size_t i = 0; i < job_.cases.size(); ++i)
        if (job_.cases[i].id == cid) {
          job_.cases[i].loads.push_back(std::move(l));
          active_case_ = static_cast<int>(i);
        }
      render_cases();
      update_selection();
      mark_dirty();
    });
    case_list_->addWidget(fr);
    case_frames_.emplace_back(fr);
  }
  update_case_frames();
}

QWidget* MainWindow::build_load_item(const app::UiLoad& l) {
  const int lid = l.id;
  const auto& info = app::load_type(l.type);
  auto* fr = new Frame("load");
  auto* v = new QVBoxLayout(fr);
  v->setContentsMargins(10, 9, 10, 9);
  v->setSpacing(8);
  auto* head = hbox(8);
  head->addWidget(make_dot(app::palette::kLoad));
  head->addWidget(make_label(QString::fromUtf8(info.name), "itemTitle", false), 1);
  if (info.faces) {
    auto* re = make_icon_button("⟲", "Заменить поверхность выбранной");
    re->setEnabled(!sel_.empty());
    load_reselect_.emplace_back(re);
    head->addWidget(re);
    connect(re, &QPushButton::clicked, this, [this, lid] {
      auto* x = load_by_id(job_, lid);
      if (!x || sel_.empty()) return;
      x->faces = sel_;
      sel_.clear();
      render_cases();
      update_selection();
      mark_dirty();
    });
  }
  auto* del = make_icon_button("✕", "Удалить");
  head->addWidget(del);
  connect(del, &QPushButton::clicked, this, [this, lid] {
    for (auto& c : job_.cases) std::erase_if(c.loads, [lid](const app::UiLoad& x) { return x.id == lid; });
    if (hover_ && hover_->second == lid) hover_.reset();
    render_cases();
    update_selection();
    mark_dirty();
  });
  v->addLayout(head);
  if (info.faces) {
    auto* meta = make_label(faces_meta(l.faces), "meta");
    meta->setTextFormat(Qt::RichText);
    v->addWidget(meta);
  }

  auto* grid = new QGridLayout;
  grid->setHorizontalSpacing(8);
  grid->setVerticalSpacing(8);
  grid->setColumnStretch(0, 1);
  grid->setColumnStretch(1, 1);
  int row = 0, col = 0;
  auto put = [&](QWidget* w, bool wide = false) {
    if (wide && col) {
      ++row;
      col = 0;
    }
    grid->addWidget(w, row, col, 1, wide ? 2 : 1);
    if (wide || col == 1) {
      ++row;
      col = 0;
    } else {
      col = 1;
    }
  };
  // числовое поле нагрузки; setter получает разобранное число (пусто — только если можно)
  auto num = [this](std::optional<double> val, bool allow_empty, bool overlay,
                    std::function<void(app::UiLoad&, std::optional<double>)> set, int id) {
    auto* e = make_num(val);
    connect(e, &QLineEdit::textEdited, this, [this, e, allow_empty, overlay, set, id](const QString& t) {
      const auto x = parse_num(t);
      const bool ok = x || (allow_empty && t.trimmed().isEmpty());
      set_bad(e, !ok);
      if (!ok) return;
      if (auto* ld = load_by_id(job_, id)) set(*ld, x);
      mark_dirty();
      if (overlay) refresh_overlay();
    });
    return e;
  };
  auto value = [&](const QString& title, bool wide = false) {
    put(make_field(title, num(l.value, false, false, [](app::UiLoad& ld, std::optional<double> x) { ld.value = *x; }, lid)),
        wide);
  };
  auto dir_select = [&](const std::vector<std::pair<const char*, const char*>>& list) {
    auto* d = make_combo(list, l.dir);
    put(make_field("Направление", d));
    connect(d, &QComboBox::currentIndexChanged, this, [this, lid, d](int) {
      if (auto* x = load_by_id(job_, lid)) x->dir = combo_key(d);
      render_cases();
      refresh_overlay();
      mark_dirty();
    });
    if (l.dir == "custom") {
      auto* vec = hbox(6);
      for (int i = 0; i < 3; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        vec->addWidget(num(l.vec[ui], false, true,
                           [ui](app::UiLoad& ld, std::optional<double> x) { ld.vec[ui] = *x; }, lid));
      }
      put(make_field("Вектор направления X, Y, Z", wrap_layout(vec)), true);
    }
  };
  const auto& t = l.type;
  if (t == "force" || t == "bearing") {
    value("Сила, Н");
    dir_select(app::directions());
  } else if (t == "mass") {
    value("Масса, кг");
    dir_select(app::directions());
  } else if (t == "pressure") {
    value("Давление, МПа", true);
    put(make_hint("Давит на выбранную поверхность. 1 МПа = 10 атм = 1 Н/мм²."), true);
  } else if (t == "moment") {
    value("Момент, Н·мм");
    std::vector<std::pair<const char*, const char*>> axes;
    for (const auto& d : app::axis_directions())
      if (std::string(d.first) != "custom") axes.push_back(d);
    auto* ax = make_combo(axes, l.axis);
    put(make_field("Ось вращения", ax));
    connect(ax, &QComboBox::currentIndexChanged, this, [this, lid, ax](int) {
      if (auto* x = load_by_id(job_, lid)) x->axis = combo_key(ax);
      mark_dirty();
    });
    put(make_hint("Направление — по правилу правой руки. 1 Н·м = 1000 Н·мм."), true);
  } else if (t == "impact") {
    value("Масса груза, кг");
    put(make_field("Высота падения, мм",
                   num(l.height, false, false, [](app::UiLoad& ld, std::optional<double> x) { ld.height = *x; }, lid)));
    dir_select(app::directions());
  } else if (t == "displacement") {
    auto* vec = hbox(6);
    for (int i = 0; i < 3; ++i) {
      const auto ui = static_cast<std::size_t>(i);
      vec->addWidget(num(l.disp[ui], true, true, [ui](app::UiLoad& ld, std::optional<double> x) { ld.disp[ui] = x; }, lid));
    }
    put(make_field("Смещение X, Y, Z, мм (пусто — свободно)", wrap_layout(vec)), true);
  } else if (t == "gravity") {
    value("Перегрузка, g");
    dir_select(app::axis_directions());
    put(make_hint("1 g — собственный вес детали; 5–10 g — тряска, падение в коробке."), true);
  }
  v->addLayout(grid);
  fr->on_hover = [this, lid](bool in) {
    const std::pair<char, int> key{'l', lid};
    if (in)
      hover_ = key;
    else if (hover_ == key)
      hover_.reset();
    refresh_overlay();
  };
  return fr;
}

void MainWindow::update_case_frames() {
  const bool several = job_.cases.size() > 1;
  for (std::size_t i = 0; i < case_frames_.size(); ++i) {
    QFrame* f = case_frames_[i];
    if (!f) continue;
    const bool active = several && static_cast<int>(i) == active_case_;
    if (f->property("active").toBool() != active) {
      f->setProperty("active", active);
      repolish(f);
    }
  }
}

void MainWindow::render_results() {
  results_box_->setVisible(results_ != nullptr);
  clear_layout(results_list_);
  if (!results_) return;
  if (dirty_)
    results_list_->addWidget(
        make_warn("Задание изменено после расчёта — нажмите «Рассчитать», чтобы обновить результаты."));
  for (std::size_t i = 0; i < results_->cases.size(); ++i) {
    const auto& s = results_->cases[i].summary;
    auto* card = new Frame("rcard");
    card->setProperty("on", static_cast<int>(i) == result_case_);
    card->setCursor(Qt::PointingHandCursor);
    card->on_click = [this, i] { select_result(static_cast<int>(i)); };
    auto* v = new QVBoxLayout(card);
    v->setContentsMargins(12, 11, 12, 11);
    v->setSpacing(7);
    QStringList kind;
    kind << qs(s.duration_name);
    if (s.cycles) kind << ru_int(std::llround(*s.cycles)) + " циклов";
    if (s.temperature) kind << fmtq(*s.temperature, 0) + " °C";
    auto* top = hbox(8);
    auto* names = new QVBoxLayout;
    names->setSpacing(2);
    names->addWidget(make_label(qs(s.name), "rcTitle"));
    names->addWidget(make_label(kind.join(" · "), "hint"));
    top->addLayout(names, 1);
    auto* verdict = make_label(verdict_name(s.verdict), "verdict", false);
    verdict->setProperty("v", qs(s.verdict));
    top->addWidget(verdict, 0, Qt::AlignTop);
    v->addLayout(top);
    auto* sfrow = hbox(10);
    auto* big = make_label(fmtq(std::min(s.sf, 999.0), 2), "sfbig", false);
    big->setProperty("v", qs(s.verdict));
    QFont nf = big->font();
    nf.setFamilies({"Cascadia Mono", "Consolas", "JetBrains Mono", "DejaVu Sans Mono", "monospace"});
    nf.setPixelSize(26);
    nf.setWeight(QFont::DemiBold);
    big->setFont(nf);
    sfrow->addWidget(big);
    sfrow->addWidget(make_label("запас прочности<br>нужно ≥ " + fmtq(s.target_sf, 1), "hint", false));
    sfrow->addStretch();
    v->addLayout(sfrow);
    QStringList where;
    for (double x : s.sf_xyz) where << fmtq(x, 1);
    std::vector<std::pair<QString, QString>> kv = {
        {"Разрушение", esc(s.mode)}, {"Где", where.join(" · ") + " мм"}, {"Прогиб", fmtq(s.max_disp, 2) + " мм"}};
    if (s.limit) kv.emplace_back("Выдержит до", esc(s.limit->short_text.empty() ? s.limit->text : s.limit->short_text));
    if (s.impact_factor) kv.emplace_back("Удар", "коэффициент " + fmtq(*s.impact_factor, 2));
    if (s.disp_force) {
      const auto& f = *s.disp_force;
      kv.emplace_back("Усилие", fmtq(std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]), 1) + " Н");
    }
    auto* kvl = make_label(kv_table(kv), "kv");
    kvl->setTextFormat(Qt::RichText);
    v->addWidget(kvl);
    for (const auto& w : s.warnings) v->addWidget(make_warn(qs(w)));
    if (s.sf_min_at_bc)
      v->addWidget(make_warn("У места закрепления или нагрузки локальный пик (запас " + fmtq(s.sf_min, 2) +
                             ") — обычно это особенность модели."));
    for (auto* child : card->findChildren<QLabel*>()) child->setAttribute(Qt::WA_TransparentForMouseEvents);
    results_list_->addWidget(card);
  }
  results_list_->addWidget(make_hint("Время расчёта: " + fmtq(results_->total_time, 1) + " с."));
}

void MainWindow::render_fields() {
  clear_layout(fields_layout_);
  auto add = [this](const FieldButton& fb) {
    auto* b = make_seg_button(fb.name);
    b->setChecked(fb.field == field_);
    b->setEnabled(model_ != nullptr);
    const Field f = fb.field;
    connect(b, &QPushButton::clicked, this, [this, f] { set_field(f); });
    fields_layout_->addWidget(b);
  };
  if (results_)
    for (const auto& fb : kFieldsRes) add(fb);
  else
    for (const auto& fb : kFieldsPre) add(fb);
  deform_ctl_->setVisible(results_ != nullptr);
  btn_weak_->setVisible(results_ != nullptr);
}

void MainWindow::set_field(Field f) {
  field_ = f;
  render_fields();
  apply_view();
}

void MainWindow::update_selection() {
  const auto n = sel_.size();
  if (n)
    sel_info_->setText("Выбрано: <b>" + QString::number(n) + "</b> гр. · <b>" + fmtq(scene_ ? scene_->area(sel_) : 0, 0) +
                       " мм²</b>");
  else
    sel_info_->setText("Щелчок — выбрать, Shift — добавить, Ctrl или Alt — убрать");
  btn_clear_sel_->setEnabled(n > 0);
  btn_add_fix_->setEnabled(n > 0);
  for (auto& b : fix_reselect_)
    if (b) b->setEnabled(n > 0);
  for (auto& b : load_reselect_)
    if (b) b->setEnabled(n > 0);
  refresh_overlay();
}

// ============================================================================ 3D-вид

void MainWindow::refresh_overlay() {
  if (!scene_) return;
  std::vector<app::Overlay> ov;
  std::vector<Marker> mk;
  for (const auto& f : job_.fixtures) {
    const bool hov = hover_ && *hover_ == std::pair<char, int>{'f', f.id};
    ov.push_back({f.faces, app::palette::kFixture, hov ? 0.95f : 0.7f});
  }
  if (!results_ || setup_field(field_)) {
    if (active_case_ >= 0 && active_case_ < static_cast<int>(job_.cases.size()))
      for (const auto& l : job_.cases[static_cast<std::size_t>(active_case_)].loads) {
        if (l.faces.empty()) continue;
        const bool hov = hover_ && *hover_ == std::pair<char, int>{'l', l.id};
        ov.push_back({l.faces, app::palette::kLoad, hov ? 0.95f : 0.65f});
        if (auto a = app::load_arrow(l, *scene_)) mk.push_back({l.faces, *a, app::palette::kLoad});
      }
  } else if (result_case_ < static_cast<int>(results_->cases.size())) {
    // стрелки нагрузок того случая, результаты которого показаны
    const auto& r = results_->cases[static_cast<std::size_t>(result_case_)];
    std::size_t li = 0;
    for (const auto& [name, faces] : r.sel_faces) {
      if (name.rfind("load", 0) != 0) continue;
      const auto* ld = li < r.summary.loads.size() ? &r.summary.loads[li] : nullptr;
      ++li;
      if (!ld) continue;
      const json::Value* vec = ld->json.find("vector");
      if (!vec || !vec->is_array() || vec->size() != 3) continue;
      app::Vec3 d{};
      bool ok = true;
      for (std::size_t a = 0; a < 3; ++a) {
        if (!vec->at(a).is_number()) ok = false;
        else d[a] = vec->at(a).as_double();
      }
      if (!ok) continue;
      std::vector<std::int64_t> keys;
      keys.reserve(faces.size());
      for (auto f : faces) keys.push_back(model_->mesh.face_key[static_cast<std::size_t>(f)]);
      std::sort(keys.begin(), keys.end());
      mk.push_back({std::move(keys), d, app::palette::kLoad});
    }
  }
  if (!sel_.empty()) ov.push_back({sel_, app::palette::kSelection, 0.9f});
  viewer_->set_overlays(std::move(ov));
  viewer_->set_markers(std::move(mk));
}

void MainWindow::apply_view() {
  if (!scene_) return;
  scene_->set_target(job_.target_sf);
  scene_->set_field(field_);
  if (results_) scene_->set_case(static_cast<std::size_t>(result_case_));
  std::optional<app::Vec3> crit;
  if (results_ && (field_ == Field::SafetyFactor || field_ == Field::Stress || field_ == Field::Mode))
    crit = results_->cases[static_cast<std::size_t>(result_case_)].summary.sf_xyz;
  viewer_->set_critical(crit);
  apply_deform();
  refresh_overlay();
}

void MainWindow::apply_deform() {
  const bool on = results_ && scene_ && deform_on_->isChecked();
  const double k = std::pow(10.0, (deform_slider_->value() - 50) / 50.0);
  const double s = on ? scene_->auto_deform_scale() * k : 0.0;
  deform_val_->setText(on ? fmtq(s, s >= 10 ? 0 : 1) : QString("—"));
  viewer_->set_deform(s);
}

void MainWindow::apply_section() {
  const int a = sec_axis_->currentIndex() - 1;
  sec_pos_->setEnabled(a >= 0);
  if (!scene_) return;
  if (a < 0)
    scene_->set_section(std::nullopt);
  else
    scene_->set_section(std::pair<int, double>{a, sec_pos_->value() / 1000.0 * scene_->size()[static_cast<std::size_t>(a)]});
  viewer_->rebuild();
  refresh_overlay();
}

void MainWindow::on_pick(const std::optional<app::Hit>& hit, Qt::KeyboardModifiers mods) {
  if (!scene_ || !hit || hit->cut) return;
  std::vector<std::int64_t> region;
  if (tool_ == "plane")
    region = scene_->flood_plane(hit->elem, hit->dir);
  else if (tool_ == "hole")
    region = scene_->hole_region(hit->elem, hit->dir);
  else
    region = scene_->brush(hit->point, brush_r_);
  if (mods & (Qt::AltModifier | Qt::ControlModifier))
    sel_ = remove_keys(sel_, region);
  else if (mods & Qt::ShiftModifier)
    sel_ = merge_keys(sel_, region);
  else
    sel_ = std::move(region);
  update_selection();
}

void MainWindow::select_result(int i) {
  if (!results_ || i < 0 || i >= static_cast<int>(results_->cases.size())) return;
  result_case_ = i;
  if (setup_field(field_)) field_ = Field::SafetyFactor;
  render_results();
  render_fields();
  apply_view();
}

void MainWindow::show_weak_spot() {
  if (!results_ || !scene_) return;
  const double z = results_->cases[static_cast<std::size_t>(result_case_)].summary.sf_xyz[2];
  const double h = std::max(scene_->size()[2], 1e-9);
  {
    const QSignalBlocker b1(sec_axis_), b2(sec_pos_);
    sec_axis_->setCurrentIndex(3);
    sec_pos_->setValue(std::min(1000, static_cast<int>(std::lround((z + 0.01) / h * 1000))));
  }
  apply_section();
  viewer_->view("top");
}

// ============================================================================ состояние

void MainWindow::mark_dirty() {
  if (results_ && !dirty_) {
    dirty_ = true;
    render_results();
  }
  update_run_state();
}

void MainWindow::update_run_state() {
  const bool ready = model_ && app::can_run(job_);
  btn_run_->setEnabled(!busy_ && ready);
  run_hint_->setVisible(!busy_ && !ready);
  btn_save_job_->setEnabled(!busy_ && model_);
  btn_report_->setEnabled(!busy_ && results_);
}

void MainWindow::set_busy(bool busy) {
  busy_ = busy;
  for (auto* b : {btn_open_, btn_example_, btn_open_job_, btn_rebuild_}) b->setEnabled(!busy);
  update_run_state();
}

void MainWindow::show_progress(bool on, double frac, const QString& text) {
  progress_box_->setVisible(on);
  if (!on) return;
  progress_->setValue(static_cast<int>(std::lround(std::clamp(frac, 0.0, 1.0) * 1000)));
  progress_text_->setText(text);
}

void MainWindow::toast(const QString& text, bool error) {
  if (error) {
    had_error_ = true;
    std::cerr << "kika-gui: " << ss(text) << std::endl;
  } else if (verbose_) {
    std::cout << ss(text) << std::endl;
  }
  toast_->setText(text);
  toast_->setProperty("err", error);
  repolish(toast_);
  toast_->adjustSize();
  place_toast();
  toast_->show();
  toast_->raise();
  toast_timer_->start(error ? 7000 : 3500);
}

void MainWindow::place_toast() {
  if (!toast_ || !centralWidget() || !viewer_) return;
  // внизу 3D-вида, над панелью полей
  const QPoint v = viewer_->mapTo(centralWidget(), QPoint(0, 0));
  toast_->move(v.x() + (viewer_->width() - toast_->width()) / 2, v.y() + viewer_->height() - toast_->height() - 16);
}

void MainWindow::resizeEvent(QResizeEvent* e) {
  QMainWindow::resizeEvent(e);
  if (toast_ && toast_->isVisible()) place_toast();
}

void MainWindow::keyPressEvent(QKeyEvent* e) {
  if (e->key() == Qt::Key_Escape && !sel_.empty()) {
    sel_.clear();
    update_selection();
    return;
  }
  QMainWindow::keyPressEvent(e);
}

// ============================================================================ фоновая работа

analysis::Progress MainWindow::progress_cb() {
  auto cancel = cancel_;
  return [this, cancel](std::string_view, double frac, std::string_view text) {
    if (*cancel) throw Cancelled{};
    const QString t = QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
    QMetaObject::invokeMethod(
        this, [this, frac, t] { if (busy_) show_progress(true, frac, t); }, Qt::QueuedConnection);
  };
}

void MainWindow::start_work(std::function<std::function<void()>()> work, const QString& text) {
  if (worker_) return;
  set_busy(true);
  *cancel_ = false;
  show_progress(true, 0.0, text);
  worker_ = QThread::create([this, work = std::move(work)] {
    std::function<void()> done;
    try {
      done = work();
    } catch (const Cancelled&) {
    } catch (const std::bad_alloc&) {
      done = [this] { toast("Не хватило памяти. Уменьшите детальность сетки.", true); };
    } catch (const std::exception& e) {
      const QString msg = QString::fromUtf8(e.what());
      done = [this, msg] { toast(msg, true); };
    } catch (...) {
      done = [this] { toast("Неизвестная ошибка.", true); };
    }
    QMetaObject::invokeMethod(this, [this, done] { finish_work(done); }, Qt::QueuedConnection);
  });
  worker_->start();
}

void MainWindow::finish_work(const std::function<void()>& done) {
  if (worker_) {
    worker_->wait();
    delete worker_;
    worker_ = nullptr;
  }
  set_busy(false);
  show_progress(false);
  if (closing_) {
    close();
    return;
  }
  if (done) done();
  continue_script();
}

void MainWindow::closeEvent(QCloseEvent* e) {
  if (worker_) {
    // расчёт прервётся на ближайшем этапе; окно закроется, когда поток завершится
    *cancel_ = true;
    closing_ = true;
    hide();
    e->ignore();
    return;
  }
  e->accept();
}

// ============================================================================ модель и задание

void MainWindow::load_model(const QString& path, long long max_elems, bool keep_job, std::function<void()> after) {
  if (busy_) return;
  if (path.endsWith(".bgcode", Qt::CaseInsensitive)) {
    toast("Двоичный G-code (.bgcode) не поддерживается — сохраните из слайсера обычный .gcode.", true);
    return;
  }
  last_dir_ = QFileInfo(path).absolutePath();
  const auto fspath = to_path(path);
  auto prog = progress_cb();
  start_work(
      [this, fspath, path, max_elems, keep_job, prog, after]() -> std::function<void()> {
        auto tp = gcode::load(fspath);
        analysis::ModelOptions mo;
        mo.max_elems = static_cast<int>(max_elems);
        auto model = std::make_shared<analysis::Model>(analysis::build_model(std::move(tp), mo, prog));
        return [this, model, path, max_elems, keep_job, after] {
          on_model_ready(model, path, max_elems, keep_job);
          if (after) after();
        };
      },
      "Читаю G-code");
}

void MainWindow::on_model_ready(std::shared_ptr<analysis::Model> model, const QString& path, long long max_elems,
                                bool keep_job) {
  const auto old_model = model_;  // старая сцена ссылается на старую модель
  const auto old_scene = std::move(scene_);
  const auto old_roads = std::move(roads_);
  model_ = std::move(model);
  scene_ = std::make_unique<app::Scene>(*model_);
  roads_ = std::make_unique<app::Roads>(*model_, *scene_);
  results_.reset();
  results_job_ = json::Value();
  dirty_ = false;
  result_case_ = 0;
  sel_.clear();
  hover_.reset();
  field_ = Field::Model;
  gcode_path_ = path;
  model_max_elems_ = max_elems;
  detail_ = nearest_detail(max_elems);
  if (keep_job && old_scene) {
    app::remap_faces(job_, *old_scene, *scene_);
  } else if (!keep_job) {
    job_ = app::new_job();
    job_title_.clear();
    job_subtitle_.clear();
    active_case_ = 0;
    if (model_->material_guess && material::find_material(*model_->material_guess))
      job_.material = *model_->material_guess;
  }
  viewer_->set_scene(scene_.get(), roads_.get());
  apply_section();
  const QFileInfo fi(path);
  file_label_->setText(fi.fileName());
  file_label_->setToolTip(QDir::toNativeSeparators(fi.absoluteFilePath()));
  setWindowTitle(fi.fileName() + " — Kika");
  render_all();
  if (!keep_job && pending_job_) {
    const json::Value j = std::move(*pending_job_);
    pending_job_.reset();
    apply_job_json(j);
  }
}

bool MainWindow::apply_job_json(const json::Value& j) {
  if (!model_) {
    pending_job_ = j;
    return false;
  }
  std::vector<std::string> warns;
  try {
    job_ = app::from_job_json(j, *model_, &warns);
  } catch (const std::exception& e) {
    toast("Не удалось применить задание: " + QString::fromUtf8(e.what()), true);
    return false;
  }
  active_case_ = 0;
  sel_.clear();
  hover_.reset();
  const auto* t = j.find("title");
  const auto* st = j.find("subtitle");
  job_title_ = t && t->is_string() ? t->as_string() : std::string();
  job_subtitle_ = st && st->is_string() ? st->as_string() : std::string();
  render_all();
  mark_dirty();
  QString msg = "Задание загружено.";
  for (const auto& w : warns) msg += " " + qs(w);
  toast(msg);
  return true;
}

namespace {

// Задание, сохранённое приложением прототипа: {"app": "fdmfea", "detail": …, "job": {…}}.
// Сетка у нас та же, что у прототипа, поэтому номера граней в нём подходят без пересчёта.
std::optional<json::Value> unwrap_prototype_save(const json::Value& j) {
  const auto* app = j.find("app");
  const auto* inner = j.find("job");
  if (!app || !app->is_string() || app->as_string() != "fdmfea" || !inner || !inner->is_object()) return std::nullopt;
  json::Value out = *inner;
  if (const auto* g = j.find("gcode"); g && g->is_string() && !out.contains("gcode")) out["gcode"] = *g;
  if (const auto* d = j.find("detail"); d && d->is_number() && !out.contains("max_elems")) {
    const auto i = std::clamp<long long>(static_cast<long long>(d->as_double()), 0, kDetailCount - 1);
    out["max_elems"] = kDetail[i].n;
  }
  return out;
}

}  // namespace

void MainWindow::open_job_file(const QString& path) {
  QString err;
  auto j = read_json(path, &err);
  if (!j) {
    toast(err, true);
    return;
  }
  if (auto inner = unwrap_prototype_save(*j)) j = std::move(inner);
  last_dir_ = QFileInfo(path).absolutePath();
  // G-code из задания — относительно папки задания
  QString gpath;
  if (const auto* g = j->find("gcode"); g && g->is_string() && !g->as_string().empty()) {
    const QString name = qs(g->as_string());
    gpath = QFileInfo(name).isAbsolute() ? name : QDir(QFileInfo(path).absolutePath()).filePath(name);
    if (!QFileInfo::exists(gpath)) gpath.clear();
  }
  std::optional<long long> max;
  if (const auto* m = j->find("max_elems"); m && m->is_number()) max = static_cast<long long>(m->as_double());
  const auto same = [](const QString& a, const QString& b) {
    return !a.isEmpty() && !b.isEmpty() && QFileInfo(a).canonicalFilePath() == QFileInfo(b).canonicalFilePath();
  };
  if (!gpath.isEmpty() && (!model_ || !same(gpath, gcode_path_) || (max && *max != model_max_elems_))) {
    // грани в задании привязаны к сетке: строим ту же, что при сохранении
    pending_job_ = *j;
    load_model(gpath, max ? *max : kDetail[detail_].n, false);
  } else if (model_) {
    if (max && *max != model_max_elems_) {
      pending_job_ = *j;
      load_model(gcode_path_, *max, false);
    } else {
      apply_job_json(*j);
    }
  } else {
    pending_job_ = *j;
    const auto* g = j->find("gcode");
    toast("Задание прочитано. Теперь откройте G-code этой детали" +
          (g && g->is_string() ? " (" + qs(g->as_string()) + ")" : QString()) + ".");
  }
}

void MainWindow::open_paths(const QStringList& paths) {
  QString gcode, job;
  for (const auto& p : paths) {
    if (p.endsWith(".json", Qt::CaseInsensitive))
      job = p;
    else
      gcode = p;
  }
  if (!job.isEmpty() && !gcode.isEmpty()) {
    QString err;
    auto j = read_json(job, &err);
    if (!j) {
      toast(err, true);
      return;
    }
    if (auto inner = unwrap_prototype_save(*j)) j = std::move(inner);
    pending_job_ = *j;
    long long max = kDetail[detail_].n;
    if (const auto* m = j->find("max_elems"); m && m->is_number()) max = static_cast<long long>(m->as_double());
    load_model(gcode, max, false);
  } else if (!job.isEmpty()) {
    open_job_file(job);
  } else if (!gcode.isEmpty()) {
    long long max = kDetail[detail_].n;
    if (pending_job_)
      if (const auto* m = pending_job_->find("max_elems"); m && m->is_number())
        max = static_cast<long long>(m->as_double());
    load_model(gcode, max, false);
  }
}

QString MainWindow::examples_dir() const {
  const QString app = QCoreApplication::applicationDirPath();
  QStringList dirs = {app + "/examples", app + "/../share/kika/examples"};
#ifdef KIKA_SOURCE_DIR
  dirs << QString::fromUtf8(KIKA_SOURCE_DIR) + "/prototype/examples";
#endif
  for (const auto& d : dirs)
    if (QFileInfo::exists(d + "/bracket_side.gcode")) return QDir(d).absolutePath();
  return {};
}

json::Value MainWindow::job_json(const QString& gcode_name) const {
  std::string title = job_title_;
  if (title.empty()) title = ss(QFileInfo(gcode_path_).completeBaseName());
  if (title.empty()) title = "Деталь";
  json::Value j = app::to_job_json(job_, title, ss(gcode_name), model_max_elems_);
  if (!job_subtitle_.empty()) j["subtitle"] = job_subtitle_;
  return j;
}

// ============================================================================ действия

void MainWindow::open_gcode_dialog() {
  const QString f = QFileDialog::getOpenFileName(this, "Открыть G-code", last_dir_,
                                                 "G-code (*.gcode *.gco *.g *.txt *.bgcode);;Все файлы (*)");
  if (!f.isEmpty()) open_paths({f});
}

void MainWindow::open_job_dialog() {
  const QString f = QFileDialog::getOpenFileName(this, "Открыть задание", last_dir_, "Задание (*.json);;Все файлы (*)");
  if (!f.isEmpty()) open_job_file(f);
}

void MainWindow::open_example() {
  if (busy_) return;
  const QString dir = examples_dir();
  if (dir.isEmpty()) {
    toast("Не нашёл пример: рядом с программой нет папки examples.", true);
    return;
  }
  QString err;
  const auto j = read_json(dir + "/bracket_side.job.json", &err);
  if (!j) {
    toast(err, true);
    return;
  }
  const json::Value job = *j;
  load_model(dir + "/bracket_side.gcode", kDetail[detail_].n, false, [this, job] {
    if (apply_job_json(job)) toast("Открыт пример. Закрепления и нагрузки уже заданы — нажмите «Рассчитать».");
  });
}

void MainWindow::save_job() {
  if (!model_) return;
  const QFileInfo gi(gcode_path_);
  const QString def = gi.absolutePath() + "/" + gi.completeBaseName() + ".job.json";
  const QString out = QFileDialog::getSaveFileName(this, "Сохранить задание", def, "Задание (*.json)");
  if (!out.isEmpty()) save_job_to(out);
}

bool MainWindow::save_job_to(const QString& out) {
  try {
    // G-code — относительно папки задания: так задание находит деталь и после переноса папки
    const QString rel = QDir(QFileInfo(out).absolutePath()).relativeFilePath(QFileInfo(gcode_path_).absoluteFilePath());
    const auto j = job_json(rel);
    std::string text;
    pretty_json(text, j, 0);
    write_file(out, text + "\n");
    toast("Задание сохранено: " + QFileInfo(out).fileName() +
          ". Его можно открыть здесь же или посчитать командой kika run.");
    return true;
  } catch (const std::exception& e) {
    toast(QString::fromUtf8(e.what()), true);
    return false;
  }
}

void MainWindow::export_report(const QString& path) {
  if (!results_ || !model_ || busy_) return;
  QString out = path;
  const bool interactive = out.isEmpty();
  if (interactive) {
    const QFileInfo gi(gcode_path_);
    out = QFileDialog::getSaveFileName(this, "Сохранить отчёт",
                                       gi.absolutePath() + "/" + gi.completeBaseName() + "_отчёт.html",
                                       "Отчёт (*.html)");
    if (out.isEmpty()) return;
  }
  report::ReportInfo info;
  if (const auto* t = results_job_.find("title"); t && t->is_string()) info.title = t->as_string();
  info.gcode_name = ss(QFileInfo(gcode_path_).fileName());
  auto model = model_;
  auto res = results_;
  auto jj = results_job_;
  start_work(
      [this, model, res, jj, info, out, interactive]() -> std::function<void()> {
        write_file(out, report::report_html(*model, *res, jj, info));
        return [this, out, interactive] {
          toast("Отчёт сохранён: " + QFileInfo(out).fileName());
          if (interactive) QDesktopServices::openUrl(QUrl::fromLocalFile(out));
        };
      },
      "Готовлю отчёт");
}

void MainWindow::about() {
  QMessageBox box(this);
  box.setWindowTitle("О программе");
  box.setIconPixmap(make_logo(64));
  box.setText(QString("<b>Kika %1</b><br>Расчёт прочности деталей для FDM-печати по G-code.").arg(kVersion));
  box.setInformativeText(QString("Интерфейс — Qt %1 (LGPL-3.0, подключается как отдельные библиотеки). Отчёт: "
                                 "three.js (MIT), сжатие zlib. Лицензии — в файле THIRD_PARTY.md.")
                             .arg(qVersion()));
  auto* qt = box.addButton("О Qt", QMessageBox::HelpRole);
  box.addButton(QMessageBox::Ok);
  box.exec();
  if (box.clickedButton() == qt) QMessageBox::aboutQt(this);
}

void MainWindow::rebuild_mesh() {
  if (!model_ || busy_) return;
  load_model(gcode_path_, kDetail[detail_].n, true);
}

void MainWindow::run_analysis() {
  if (!model_ || busy_ || !app::can_run(job_)) return;
  const json::Value jj = job_json(QFileInfo(gcode_path_).fileName());
  analysis::Job job;
  try {
    job = analysis::parse_job(jj);
  } catch (const std::exception& e) {
    toast(QString::fromUtf8(e.what()), true);
    return;
  }
  auto model = model_;
  auto prog = progress_cb();
  start_work(
      [this, model, job, jj, prog]() -> std::function<void()> {
        auto res = std::make_shared<analysis::AnalysisResult>(analysis::run_analysis(*model, job, prog));
        return [this, model, res, jj] {
          if (model != model_) return;  // пока считали, открыли другую деталь
          results_ = res;
          results_job_ = jj;
          dirty_ = false;
          result_case_ = 0;
          field_ = Field::SafetyFactor;
          scene_->set_results(results_.get());
          scene_->set_case(0);
          render_fields();
          render_results();
          apply_view();
          update_run_state();
          double worst = 1e300;
          for (const auto& c : results_->cases) worst = std::min(worst, c.summary.sf);
          toast("Расчёт готов: минимальный запас прочности " + fmtq(worst, 2) + ".");
          // после того как панель пересчитает размеры (иначе полоса прокрутки ещё короткая)
          for (int ms : {60, 250})
            QTimer::singleShot(ms, this, [this] {
              if (results_box_->isVisible()) panel_scroll_->verticalScrollBar()->setValue(results_box_->y());
            });
        };
      },
      "Проверяю задание");
}

// ============================================================================ перетаскивание

void MainWindow::dragEnterEvent(QDragEnterEvent* e) {
  if (busy_ || !e->mimeData()->hasUrls()) return;
  for (const auto& u : e->mimeData()->urls())
    if (u.isLocalFile()) {
      e->acceptProposedAction();
      return;
    }
}

void MainWindow::dropEvent(QDropEvent* e) {
  QStringList files;
  for (const auto& u : e->mimeData()->urls())
    if (u.isLocalFile()) files << u.toLocalFile();
  if (files.isEmpty()) return;
  e->acceptProposedAction();
  open_paths(files);
}

// ============================================================================ сценарий проверки

void MainWindow::start_script(StartupScript s) {
  script_ = std::move(s);
  script_active_ = true;
  verbose_ = true;
  QTimer::singleShot(0, this, [this] { continue_script(); });
}

void MainWindow::continue_script() {
  if (!script_active_ || busy_) return;
  if (script_.example) {
    script_.example = false;
    open_example();
    if (busy_) return;
  }
  if (script_.run) {
    script_.run = false;
    if (model_ && app::can_run(job_)) {
      run_analysis();
      if (busy_) return;
    } else {
      toast("Нечего считать: нужны деталь, закрепление и нагрузка.", true);
    }
  }
  script_active_ = false;
  if (results_ && script_.result_case > 0) select_result(script_.result_case);
  if (!script_.field.empty()) {
    if (const auto f = field_from_key(script_.field))
      set_field(*f);
    else
      toast("Неизвестное поле: " + qs(script_.field), true);
  }
  if (script_.section) {
    {
      const QSignalBlocker b1(sec_axis_), b2(sec_pos_);
      sec_axis_->setCurrentIndex(script_.section->first + 1);
      sec_pos_->setValue(static_cast<int>(std::lround(script_.section->second * 1000)));
    }
    apply_section();
  }
  if (script_.deform && results_) deform_on_->setChecked(true);
  if (!script_.display.empty()) display_->button(script_.display == "voxels" ? 1 : 0)->click();
  if (!script_.view.empty()) viewer_->view(script_.view);
  if (!script_.tool.empty()) {
    const int id = script_.tool == "hole" ? 1 : (script_.tool == "brush" ? 2 : 0);
    tools_->button(id)->click();
  }
  for (const auto& [fx, fy] : script_.picks) viewer_->click_at(fx, fy, Qt::ShiftModifier);
  if (!script_.save_job.isEmpty() && model_) save_job_to(script_.save_job);
  if (!script_.report.isEmpty() && results_) {
    try {
      report::ReportInfo info;
      if (const auto* t = results_job_.find("title"); t && t->is_string()) info.title = t->as_string();
      info.gcode_name = ss(QFileInfo(gcode_path_).fileName());
      write_file(script_.report, report::report_html(*model_, *results_, results_job_, info));
      std::cout << "отчёт: " << ss(script_.report) << std::endl;
    } catch (const std::exception& e) {
      toast(QString::fromUtf8(e.what()), true);
    }
  }
  if (script_.screenshot.isEmpty()) {
    QTimer::singleShot(0, this, [this] { QCoreApplication::exit(had_error_ ? 1 : 0); });
    return;
  }
  QTimer::singleShot(700, this, [this] {
    // сообщения об успехе на снимке не нужны, ошибки — оставляем
    if (!toast_->property("err").toBool()) toast_->hide();
    // grab() не видит рисования QPainter поверх OpenGL (легенда, подписи) — 3D-вид берём из его буфера
    QPixmap pm = grab();
    {
      QPainter p(&pm);
      p.drawImage(QRect(viewer_->mapTo(this, QPoint(0, 0)), viewer_->size()), viewer_->grabFramebuffer());
      if (toast_->isVisible()) toast_->render(&p, toast_->mapTo(this, QPoint(0, 0)));
    }
    const bool ok = pm.save(script_.screenshot);
    if (ok)
      std::cout << "снимок: " << ss(script_.screenshot) << std::endl;
    else
      std::cerr << "kika-gui: не удалось сохранить снимок " << ss(script_.screenshot) << std::endl;
    QCoreApplication::exit(ok && !had_error_ ? 0 : 1);
  });
}

}  // namespace kika::gui
