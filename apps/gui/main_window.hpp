#pragma once
// Главное окно: слева пошаговая панель задания (деталь, материал, закрепления, нагрузки, расчёт,
// результаты), справа 3D-вид с инструментами выбора граней. Повторяет приложение прототипа
// (fdmfea/web/app.html, app.js), но ядро считает на C++ в фоновом потоке.

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kika/analysis/analysis.hpp"
#include "kika/app/job_model.hpp"
#include "kika/app/roads.hpp"
#include "kika/app/scene.hpp"
#include "kika/geometry/import.hpp"
#include "kika/slicer/print_job.hpp"
#include "kika/util/json.hpp"

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QFrame;
class QGridLayout;
class QHBoxLayout;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSlider;
class QThread;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace kika::gui {

class Viewer;

// Что сделать после запуска — для проверок окна без человека (снимок экрана в CI).
struct StartupScript {
  bool example = false;             // открыть пример
  bool run = false;                 // посчитать задание
  std::string field;                // sf, stress, disp, mode, structure, density, model
  int result_case = 0;
  std::optional<std::pair<int, double>> section;  // ось и доля габарита 0…1
  bool deform = false;
  std::string view;                 // iso, front, top, right
  std::string display;              // roads (нити), voxels (сетка)
  std::vector<int> rotate;          // повороты модели на 90° (оси 0…2) с новой нарезкой
  std::string tool;                 // plane, hole, brush
  std::vector<std::pair<double, double>> picks;   // щелчки по 3D-виду, доли ширины и высоты
  QString report;                   // сохранить отчёт
  QString save_job;                 // сохранить задание
  QString save_gcode;               // сохранить G-code своей нарезки
  QString screenshot;               // снимок окна и выход
};

class MainWindow : public QMainWindow {
 public:
  MainWindow();
  ~MainWindow() override;

  // G-code и/или задания (.json): из командной строки и перетаскиванием.
  void open_paths(const QStringList& paths);
  // Запустить сценарий проверки (после open_paths).
  void start_script(StartupScript s);

 protected:
  void dragEnterEvent(QDragEnterEvent* e) override;
  void dropEvent(QDropEvent* e) override;
  void closeEvent(QCloseEvent* e) override;
  void keyPressEvent(QKeyEvent* e) override;
  void resizeEvent(QResizeEvent* e) override;

 private:
  // интерфейс
  void build_ui();
  QWidget* build_top_bar();
  QWidget* build_panel();
  QWidget* build_stage();
  void render_all();
  void render_model_info();
  void render_material();
  void render_fixtures();
  void render_cases();
  QWidget* build_load_item(const app::UiLoad& load);
  QString faces_meta(const std::vector<std::int64_t>& faces) const;
  void render_results();
  void render_fields();
  void update_case_frames();
  void update_selection();
  void refresh_overlay();
  void apply_view();
  void apply_deform();
  void apply_section();
  void mark_dirty();
  void update_run_state();
  void set_busy(bool busy);
  void show_progress(bool on, double frac = 0, const QString& text = {});
  void toast(const QString& text, bool error = false);
  void place_toast();
  void set_field(app::Field f);

  // действия
  void open_gcode_dialog();
  void open_job_dialog();
  void open_example();
  void save_job();
  bool save_job_to(const QString& path);
  void save_gcode(const QString& path = {});
  void reslice();
  void rotate_part(int axis);
  void export_report(const QString& path = {});
  void about();
  void rebuild_mesh();
  void run_analysis();
  void on_pick(const std::optional<app::Hit>& hit, Qt::KeyboardModifiers mods);
  void select_result(int i);
  void show_weak_spot();

  // Фоновая работа: work выполняется в потоке и возвращает продолжение для окна.
  void start_work(std::function<std::function<void()>()> work, const QString& text);
  void finish_work(const std::function<void()>& done);
  analysis::Progress progress_cb();

  // Открыть G-code или модель (STL, 3MF, STEP — режется своим слайсером).
  void load_model(const QString& path, long long max_elems, bool keep_job, std::function<void()> after = {});
  // Модель: уже прочитанная сетка и/или готовый G-code берутся, если переданы (перенарезка, смена детальности).
  struct LoadPlan {
    QString path;
    long long max_elems = 0;
    bool keep_job = false;
    std::optional<std::array<double, 3>> turned_from;  // деталь повёрнута: прежние углы — поверхности переносятся
    std::function<void()> after;
    bool model = false;
    std::shared_ptr<const geometry::ImportedModel> mesh;
    slicer::PrintJob print;
    std::shared_ptr<const std::string> gcode;
  };
  void start_load(LoadPlan plan);
  void render_print_settings();
  void read_print_settings();
  void update_print_state();
  void on_model_ready(std::shared_ptr<analysis::Model> model, const QString& path, long long max_elems, bool keep_job,
                      std::optional<std::array<double, 3>> turned_from = std::nullopt);
  bool apply_job_json(const json::Value& job);
  void open_job_file(const QString& path);
  // Задание: G-code или модель — из задания или source_override (файл, открытый вместе с заданием).
  void open_job_json(json::Value j, const QString& dir, const QString& source_override);
  void continue_script();
  QString examples_dir() const;
  // Задание JSON (формат kika run): заголовок — из открытого задания или по имени G-code.
  json::Value job_json(const QString& gcode_name) const;

  // данные
  std::shared_ptr<analysis::Model> model_;
  // Открыта модель, нарезанная своим слайсером (пусто — открыт G-code).
  struct ModelSource {
    std::shared_ptr<const geometry::ImportedModel> mesh;
    slicer::PrintJob print;  // как нарезано сейчас
    std::shared_ptr<const std::string> gcode;
    int layers = 0;
    double filament_g = 0, time_s = 0;
    std::vector<std::string> warnings;
  };
  std::optional<ModelSource> source_;
  slicer::PrintJob print_ui_;  // настройки в панели (до «Нарезать заново» могут отличаться)
  std::unique_ptr<app::Scene> scene_;
  std::unique_ptr<app::Roads> roads_;
  std::shared_ptr<analysis::AnalysisResult> results_;
  json::Value results_job_;  // задание, по которому посчитаны результаты (для отчёта)
  app::UiJob job_ = app::new_job();
  std::string job_title_, job_subtitle_;  // из открытого задания (для отчёта)
  std::vector<std::int64_t> sel_;
  QString gcode_path_;
  long long model_max_elems_ = 0;
  int detail_ = 1;
  int active_case_ = 0;
  int result_case_ = 0;
  app::Field field_ = app::Field::Model;
  bool dirty_ = false;
  bool busy_ = false;
  bool closing_ = false;
  bool had_error_ = false;
  bool verbose_ = false;  // писать сообщения в консоль (сценарий проверки)
  QString last_dir_;
  std::optional<std::pair<char, int>> hover_;  // 'f' — закрепление, 'l' — нагрузка; id
  std::string tool_ = "plane";
  double brush_r_ = 4;
  std::optional<json::Value> pending_job_;  // задание, открытое раньше G-code
  StartupScript script_;
  bool script_active_ = false;

  QThread* worker_ = nullptr;
  std::shared_ptr<std::atomic<bool>> cancel_ = std::make_shared<std::atomic<bool>>(false);

  // виджеты
  Viewer* viewer_ = nullptr;
  QLabel* file_label_ = nullptr;
  QPushButton* btn_open_ = nullptr;
  QPushButton* btn_example_ = nullptr;
  QPushButton* btn_open_job_ = nullptr;
  QPushButton* btn_save_job_ = nullptr;
  QPushButton* btn_report_ = nullptr;
  QPushButton* btn_save_gcode_ = nullptr;
  QWidget* print_box_ = nullptr;
  QComboBox* printer_combo_ = nullptr;
  QComboBox* pattern_combo_ = nullptr;
  QLineEdit* layer_edit_ = nullptr;
  QLineEdit* walls_edit_ = nullptr;
  QLineEdit* infill_edit_ = nullptr;
  QLineEdit* top_edit_ = nullptr;
  QLineEdit* bottom_edit_ = nullptr;
  QLabel* rotate_label_ = nullptr;
  QLabel* slice_info_ = nullptr;
  QVBoxLayout* print_warns_ = nullptr;
  QPushButton* btn_slice_ = nullptr;
  QScrollArea* panel_scroll_ = nullptr;
  QLabel* model_info_ = nullptr;
  QVBoxLayout* model_warns_ = nullptr;
  QWidget* mesh_ctl_ = nullptr;
  QSlider* detail_slider_ = nullptr;
  QLabel* detail_label_ = nullptr;
  QPushButton* btn_rebuild_ = nullptr;
  QComboBox* mat_combo_ = nullptr;
  QLineEdit* target_edit_ = nullptr;
  QLabel* mat_note_ = nullptr;
  QWidget* prop_box_ = nullptr;
  QGridLayout* prop_grid_ = nullptr;
  QVBoxLayout* fix_list_ = nullptr;
  QPushButton* btn_add_fix_ = nullptr;
  QVBoxLayout* case_list_ = nullptr;
  QPushButton* btn_run_ = nullptr;
  QWidget* progress_box_ = nullptr;
  QProgressBar* progress_ = nullptr;
  QLabel* progress_text_ = nullptr;
  QLabel* run_hint_ = nullptr;
  QWidget* results_box_ = nullptr;
  QVBoxLayout* results_list_ = nullptr;
  QButtonGroup* tools_ = nullptr;
  QWidget* brush_ctl_ = nullptr;
  QLabel* brush_label_ = nullptr;
  QLabel* sel_info_ = nullptr;
  QPushButton* btn_clear_sel_ = nullptr;
  QWidget* fields_box_ = nullptr;
  QHBoxLayout* fields_layout_ = nullptr;
  QWidget* deform_ctl_ = nullptr;
  QCheckBox* deform_on_ = nullptr;
  QSlider* deform_slider_ = nullptr;
  QLabel* deform_val_ = nullptr;
  QComboBox* sec_axis_ = nullptr;
  QSlider* sec_pos_ = nullptr;
  QPushButton* btn_weak_ = nullptr;
  QButtonGroup* display_ = nullptr;
  QLabel* toast_ = nullptr;
  QTimer* toast_timer_ = nullptr;
  std::vector<QPointer<QPushButton>> fix_reselect_, load_reselect_;  // «заменить выбранным»
  std::vector<QPointer<QFrame>> case_frames_;
};

}  // namespace kika::gui
