// kika-gui: окно для расчёта прочности — открыть G-code, выбрать на модели закрепления и места
// нагрузок, посчитать и посмотреть результат в 3D.
//
// kika-gui [деталь.gcode | модель.stl/.3mf/.step] [задание.json]
// Для проверок без человека (CI): --example --run --field sf --screenshot окно.png и т. п.

#include <QApplication>
#include <QDir>
#include <QLibraryInfo>
#include <QLocale>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>
#include <QSurfaceFormat>
#include <QTranslator>

#include <cstdlib>
#include <iostream>
#include <string>

#include "kika/version.hpp"
#include "main_window.hpp"

namespace {

const char* kStyle = R"(
QMainWindow, QWidget#central { background: #f1f1ee; }
QFrame#topBar { background: #ffffff; border: none; border-bottom: 1px solid #dfe0da; }
QLabel#brand { font-weight: 700; font-size: 16px; }
QLabel#fileName { color: #61646b; }
QScrollArea#panel, QWidget#panelInner { background: #f1f1ee; }
QFrame#step { border: none; border-bottom: 1px solid #dfe0da; }
QFrame#stepRun { border: none; }
QLabel#stepNum { background: #16181b; color: #f1f1ee; border-radius: 10px; font-weight: 600; font-size: 11px; }
QLabel#stepTitle { font-weight: 600; font-size: 14px; }
QLabel#hint, QLabel#muted, QLabel#meta, QLabel#fieldLabel, QLabel#ctl, QLabel#selInfo, QLabel#propHead,
QLabel#propName, QCheckBox#ctl { color: #61646b; }
QLabel#hint, QLabel#meta, QLabel#fieldLabel, QLabel#propHead, QLabel#propName { font-size: 12px; }
QLabel#ctlNum { color: #16181b; }
QLabel#warn { background: #fdeccd; border-radius: 6px; padding: 6px 8px; font-size: 12px; }
QLabel#kv { font-size: 12px; }

QPushButton { background: #ffffff; color: #16181b; border: 1px solid #dfe0da; border-radius: 7px;
  padding: 7px 12px; font-weight: 500; }
QPushButton:hover { border-color: #61646b; }
QPushButton:disabled { color: #a9aaa6; border-color: #e6e6e1; }
QPushButton[cls="primary"], QPushButton[cls="big"] { background: #16181b; color: #ffffff; border-color: #16181b; }
QPushButton[cls="primary"]:disabled, QPushButton[cls="big"]:disabled { background: #a7a8a6; border-color: #a7a8a6; }
QPushButton[cls="big"] { padding: 11px 14px; font-size: 15px; font-weight: 600; }
QPushButton[cls="sm"] { padding: 5px 9px; font-size: 12px; }
QPushButton[cls="add"] { border-style: dashed; color: #61646b; }
QPushButton[cls="add"]:hover { color: #16181b; }
QPushButton[cls="add"]:disabled { color: #b9bab6; }
QPushButton[cls="icon"] { border: none; background: transparent; color: #61646b; padding: 2px 6px; border-radius: 5px;
  font-size: 15px; }
QPushButton[cls="icon"]:hover { background: #ebebe6; color: #16181b; }
QPushButton[cls="icon"]:disabled { color: #c9cac5; }
QPushButton[cls="link"] { border: none; background: transparent; color: #61646b; padding: 2px 0; }
QPushButton[cls="link"]:hover { color: #16181b; }
QFrame#seg { background: #ebebe6; border: none; border-radius: 8px; }
QPushButton[cls="seg"] { background: transparent; border: none; border-radius: 6px; padding: 6px 10px; }
QPushButton[cls="seg"]:hover { background: #dfe0da; }
QPushButton[cls="seg"]:checked { background: #16181b; color: #ffffff; }
QPushButton[cls="seg"]:disabled { color: #a9aaa6; background: transparent; }

QLineEdit, QComboBox, QDoubleSpinBox { background: #ffffff; border: 1px solid #dfe0da; border-radius: 6px;
  padding: 5px 7px; color: #16181b; selection-background-color: #16181b; selection-color: #ffffff; }
QLineEdit:focus, QComboBox:focus, QDoubleSpinBox:focus { border-color: #16181b; }
QComboBox::drop-down { border: none; width: 22px; }
QComboBox QAbstractItemView { background: #ffffff; border: 1px solid #dfe0da; selection-background-color: #16181b;
  selection-color: #ffffff; outline: none; }
QLineEdit[bad="true"] { border-color: #b42323; background: #fbe1df; }
QLineEdit#itemName, QLineEdit#caseName { border-color: transparent; background: transparent; font-weight: 600;
  padding: 3px 4px; }
QLineEdit#itemName:hover, QLineEdit#itemName:focus, QLineEdit#caseName:hover, QLineEdit#caseName:focus {
  border-color: #dfe0da; background: #ffffff; }
QLineEdit#propEdit { padding: 3px 5px; font-size: 12px; }

QFrame#item { background: #ffffff; border: 1px solid #dfe0da; border-radius: 9px; }
QFrame#case { background: #ffffff; border: 1px solid #dfe0da; border-radius: 11px; }
QFrame#case[active="true"] { border: 2px solid #16181b; }
QFrame#load { background: #f1f1ee; border: 1px solid #dfe0da; border-radius: 9px; }
QLabel#itemTitle { font-weight: 600; }
QFrame#rcard { background: #ffffff; border: 1px solid #dfe0da; border-radius: 10px; }
QFrame#rcard:hover { border-color: #61646b; }
QFrame#rcard[on="true"] { border: 2px solid #16181b; }
QLabel#rcTitle { font-weight: 600; font-size: 14px; }
QLabel#verdict { font-size: 12px; font-weight: 600; padding: 3px 8px; border-radius: 10px; }
QLabel#verdict[v="ok"] { color: #0a7d0a; background: #e3f3e1; }
QLabel#verdict[v="risk"] { color: #a35a00; background: #fdeccd; }
QLabel#verdict[v="fail"] { color: #b42323; background: #fbe1df; }
QLabel#sfbig[v="ok"] { color: #0a7d0a; }
QLabel#sfbig[v="risk"] { color: #a35a00; }
QLabel#sfbig[v="fail"] { color: #b42323; }

QFrame#pickBar { background: #ffffff; border: none; border-bottom: 1px solid #dfe0da; }
QFrame#viewBar { background: #ffffff; border: none; border-top: 1px solid #dfe0da; }
QLabel#toast { background: #16181b; color: #f4f4f1; border-radius: 9px; padding: 10px 14px; font-size: 13px; }
QLabel#toast[err="true"] { background: #b42323; color: #ffffff; }
QProgressBar { background: #ebebe6; border: none; border-radius: 3px; }
QProgressBar::chunk { background: #16181b; border-radius: 3px; }
QSplitter::handle { background: #dfe0da; }
QToolTip { background: #16181b; color: #f4f4f1; border: none; padding: 5px 8px; }
QSlider::groove:horizontal { height: 4px; background: #dfe0da; border-radius: 2px; }
QSlider::sub-page:horizontal { background: #16181b; border-radius: 2px; }
QSlider::handle:horizontal { background: #16181b; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
QSlider::sub-page:horizontal:disabled { background: #c9cac5; }
QSlider::handle:horizontal:disabled { background: #b5b6b2; }
QScrollBar:vertical { background: transparent; width: 10px; margin: 0; }
QScrollBar::handle:vertical { background: #cfd0ca; border-radius: 4px; min-height: 30px; margin: 2px; }
QScrollBar::handle:vertical:hover { background: #a9aaa6; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: transparent; }
)";

// Светлая палитра (как у прототипа): окно не перекрашивается тёмной темой системы.
QPalette light_palette() {
  QPalette p;
  const QColor bg(0xf1, 0xf1, 0xee), surface(0xff, 0xff, 0xff), fg(0x16, 0x18, 0x1b), muted(0x61, 0x64, 0x6b),
      line(0xdf, 0xe0, 0xda), chip(0xeb, 0xeb, 0xe6);
  p.setColor(QPalette::Window, bg);
  p.setColor(QPalette::WindowText, fg);
  p.setColor(QPalette::Base, surface);
  p.setColor(QPalette::AlternateBase, chip);
  p.setColor(QPalette::Text, fg);
  p.setColor(QPalette::Button, surface);
  p.setColor(QPalette::ButtonText, fg);
  p.setColor(QPalette::BrightText, Qt::white);
  p.setColor(QPalette::Highlight, fg);
  p.setColor(QPalette::HighlightedText, Qt::white);
  p.setColor(QPalette::ToolTipBase, fg);
  p.setColor(QPalette::ToolTipText, QColor(0xf4, 0xf4, 0xf1));
  p.setColor(QPalette::PlaceholderText, muted);
  p.setColor(QPalette::Light, surface);
  p.setColor(QPalette::Midlight, chip);
  p.setColor(QPalette::Mid, line);
  p.setColor(QPalette::Dark, muted);
  p.setColor(QPalette::Shadow, fg);
  p.setColor(QPalette::Link, fg);
  for (auto role : {QPalette::WindowText, QPalette::Text, QPalette::ButtonText})
    p.setColor(QPalette::Disabled, role, QColor(0xa9, 0xaa, 0xa6));
  return p;
}

void usage() {
  std::cout << "kika-gui " << kika::kVersion
            << " — окно расчёта прочности деталей для FDM-печати\n\n"
               "  kika-gui [деталь.gcode | модель.stl] [задание.json]\n\n"
               "Для проверок без человека:\n"
               "  --example            открыть пример (кронштейн)\n"
               "  --run                посчитать задание\n"
               "  --field ПОЛЕ         model, structure, density, sf, stress, disp, mode\n"
               "  --case N             показать N-й расчётный случай (с нуля)\n"
               "  --section ОСЬ:ДОЛЯ   разрез, например z:0.5\n"
               "  --deform             показать деформацию\n"
               "  --view ВИД           iso, front, top, right\n"
               "  --display ВИД        roads (нити), voxels (расчётная сетка)\n"
               "  --rotate ОСЬ         повернуть модель на 90° вокруг x, y или z и нарезать заново\n"
               "  --tool ИНСТРУМЕНТ    plane, hole, brush\n"
               "  --pick X,Y           щелчок по 3D-виду (доли ширины и высоты), можно несколько\n"
               "  --report ФАЙЛ.html   сохранить отчёт\n"
               "  --save-job ФАЙЛ.json сохранить задание\n"
               "  --save-gcode ФАЙЛ    сохранить G-code своей нарезки модели\n"
               "  --screenshot ФАЙЛ    снимок окна и выход (код 1, если были ошибки)\n"
               "  --size ШxВ           размер окна\n";
}

bool parse_pair(const QString& s, QChar sep, double& a, double& b) {
  const auto parts = s.split(sep);
  if (parts.size() != 2) return false;
  bool ok1 = false, ok2 = false;
  a = parts[0].toDouble(&ok1);
  b = parts[1].toDouble(&ok2);
  return ok1 && ok2;
}

}  // namespace

int main(int argc, char** argv) {
  QSurfaceFormat fmt;
  fmt.setDepthBufferSize(24);
  fmt.setSamples(4);
  QSurfaceFormat::setDefaultFormat(fmt);
  QApplication::setStyle(QStyleFactory::create("Fusion"));
  QApplication app(argc, argv);
  QApplication::setApplicationName("Kika");
  QApplication::setApplicationVersion(kika::kVersion);
#if QT_VERSION >= QT_VERSION_CHECK(6, 8, 0)
  QGuiApplication::styleHints()->setColorScheme(Qt::ColorScheme::Light);
#endif
  QLocale::setDefault(QLocale(QLocale::Russian, QLocale::Russia));
  QApplication::setPalette(light_palette());
  QFont font = QApplication::font();
  font.setFamilies({"Segoe UI", "Inter", "Noto Sans", "DejaVu Sans"});
  font.setPixelSize(13);
  QApplication::setFont(font);
  app.setStyleSheet(kStyle);

  // русские подписи стандартных диалогов Qt (открыть/сохранить файл)
  QTranslator qt_ru;
  for (const QString& dir : {QLibraryInfo::path(QLibraryInfo::TranslationsPath),
                             QCoreApplication::applicationDirPath() + "/translations"})
    if (qt_ru.load(QLocale(QLocale::Russian), "qtbase", "_", dir)) {
      QCoreApplication::installTranslator(&qt_ru);
      break;
    }

  kika::gui::StartupScript script;
  bool scripted = false;
  QStringList files;
  QSize size;
  const QStringList args = QCoreApplication::arguments();
  for (qsizetype i = 1; i < args.size(); ++i) {
    const QString& a = args[i];
    auto next = [&]() -> QString {
      if (i + 1 >= args.size()) {
        std::cerr << "kika-gui: после " << a.toStdString() << " нужно значение\n";
        std::exit(2);
      }
      return args[++i];
    };
    if (a == "--help" || a == "-h") {
      usage();
      return 0;
    } else if (a == "--version") {
      std::cout << kika::kVersion << "\n";
      return 0;
    } else if (a == "--example") {
      script.example = scripted = true;
    } else if (a == "--run") {
      script.run = scripted = true;
    } else if (a == "--field") {
      script.field = next().toStdString();
      scripted = true;
    } else if (a == "--case") {
      script.result_case = next().toInt();
      scripted = true;
    } else if (a == "--section") {
      const QString v = next();
      const int axis = static_cast<int>(QString("xyz").indexOf(v.left(1).toLower()));
      bool ok = false;
      const double frac = v.mid(2).toDouble(&ok);
      if (axis < 0 || !ok) {
        std::cerr << "kika-gui: --section ожидает ось и долю, например z:0.5\n";
        return 2;
      }
      script.section = std::pair<int, double>{axis, frac};
      scripted = true;
    } else if (a == "--deform") {
      script.deform = scripted = true;
    } else if (a == "--view") {
      script.view = next().toStdString();
      scripted = true;
    } else if (a == "--display") {
      script.display = next().toStdString();
      scripted = true;
    } else if (a == "--rotate") {
      const QString v = next().toLower();
      const int axis = v.size() == 1 ? static_cast<int>(QString("xyz").indexOf(v)) : -1;
      if (axis < 0) {
        std::cerr << "kika-gui: --rotate ожидает ось x, y или z\n";
        return 2;
      }
      script.rotate.push_back(axis);
      scripted = true;
    } else if (a == "--tool") {
      script.tool = next().toStdString();
      scripted = true;
    } else if (a == "--pick") {
      double x = 0, y = 0;
      if (!parse_pair(next(), ',', x, y)) {
        std::cerr << "kika-gui: --pick ожидает X,Y — доли ширины и высоты вида\n";
        return 2;
      }
      script.picks.emplace_back(x, y);
      scripted = true;
    } else if (a == "--report") {
      script.report = next();
      scripted = true;
    } else if (a == "--save-gcode") {
      script.save_gcode = next();
      scripted = true;
    } else if (a == "--save-job") {
      script.save_job = next();
      scripted = true;
    } else if (a == "--screenshot") {
      script.screenshot = next();
      scripted = true;
    } else if (a == "--size") {
      double w = 0, h = 0;
      if (!parse_pair(next(), 'x', w, h)) {
        std::cerr << "kika-gui: --size ожидает ШxВ, например 1400x900\n";
        return 2;
      }
      size = QSize(static_cast<int>(w), static_cast<int>(h));
    } else if (a.startsWith("--")) {
      std::cerr << "kika-gui: неизвестный параметр " << a.toStdString() << "\n";
      return 2;
    } else {
      files << QDir::fromNativeSeparators(a);
    }
  }

  kika::gui::MainWindow w;
  if (size.isValid()) w.resize(size);
  w.show();
  if (!files.isEmpty()) w.open_paths(files);
  if (scripted) w.start_script(std::move(script));
  return QApplication::exec();
}
