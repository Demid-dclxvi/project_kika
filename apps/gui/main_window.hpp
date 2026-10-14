#pragma once
// Главное окно — вкладки, как в OrcaSlicer:
//   «Подготовка» — деталь на столе принтера: положение, поворот, масштаб;
//   «Предпросмотр» — нарезка по слоям: типы линий, время, пластик; отправка на принтер;
//   «Расчёт нагрузок» — пошаговая панель задания (материал, закрепления, нагрузки, расчёт,
//     результаты, подбор печати) и 3D-вид с выбором граней — как приложение прототипа
//     (fdmfea/web/app.html, app.js);
//   «Принтер» — состояние принтера и его веб-интерфейс.
// В первых трёх вкладках слева — панель настроек печати. Ядро считает в фоновом потоке.

#include <QMainWindow>
#include <QPointer>
#include <QString>
#include <QStringList>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kika/advisor/advisor.hpp"
#include "kika/analysis/analysis.hpp"
#include "kika/app/job_model.hpp"
#include "kika/app/preview.hpp"
#include "kika/app/roads.hpp"
#include "kika/app/scene.hpp"
#include "kika/geometry/import.hpp"
#include "kika/slicer/print_job.hpp"
#include "kika/slicer/profiles.hpp"
#include "kika/util/json.hpp"

class QAction;
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
class QStackedWidget;
class QTabBar;
class QThread;
class QTimer;
class QVBoxLayout;
class QWidget;

namespace kika::gui {

class Viewer;
class PlateView;
class PrinterPanel;
class RangeSlider;
class SettingsPanel;
class WebPane;

// Вкладки окна.
enum class Tab { Prepare = 0, Preview = 1, Strength = 2, Printer = 3 };

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
  std::string support;              // поддержки: none, normal, tree (с новой нарезкой)
  std::string advise;               // подбор печати: две цели, например economy,strength
  bool apply_advice = false;        // применить рекомендацию подбора
  std::string tool;                 // plane, hole, brush
  std::vector<std::pair<double, double>> picks;   // щелчки по 3D-виду, доли ширины и высоты
  QString report;                   // сохранить отчёт
  QString save_job;                 // сохранить задание
  QString save_gcode;               // сохранить G-code своей нарезки
  QString screenshot;               // снимок окна и выход
  QString settings_shot;            // снимок окна всех настроек печати
  QString printer;                  // подключиться к принтеру (адрес)
  bool send = false;                // отправить G-code на принтер (без запуска)
  bool send_print = false;          // отправить и начать печать (без подтверждения — для проверок)
  QString printer_shot;             // снимок окна принтера
  std::optional<Tab> tab;           // вкладка для снимка
  std::string color;                // предпросмотр: раскраска (type, speed, flow, width, height, fan, layer_time)
  std::optional<std::pair<int, int>> layers;  // предпросмотр: слои с … по … (с 1; 0 — последний)
  std::vector<std::pair<std::string, std::string>> sets;  // настройки печати ключ=значение (и нарезать заново)
  std::optional<std::array<double, 3>> scale;  // масштаб детали (и нарезать заново)
  std::optional<std::array<double, 2>> move;   // положение детали на столе (и нарезать заново)
  std::optional<std::pair<double, double>> lay;  // положить на грань под точкой вида «Подготовка» (доли) — и нарезать
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
  QWidget* build_prepare_page();
  QWidget* build_preview_page();
  QWidget* build_printer_page();
  void set_tab(Tab t);
  Tab tab() const;
  QWidget* tab_view() const;  // 3D-вид текущей вкладки (для подсказок и снимка)
  // подготовка
  void render_prepare();
  void read_placement();
  void on_part_drag(double dx, double dy, bool done);
  void lay_on_face(const geometry::Vec3& normal);
  void reset_placement(bool all);
  // предпросмотр
  void set_preview(std::shared_ptr<const app::Preview> p);
  void render_preview();
  void render_legend();
  void apply_layers();
  void send_to_printer(bool start);
  // настройки печати слева
  void on_settings_changed();
  void render_slice_state();
  // Нарезка устарела: настройки или положение детали изменены после неё.
  bool slice_stale() const;
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
  // настройки печати и принтер по сети
  slicer::Filament job_filament() const;  // пластик для нарезки: свой из настроек или по материалу
  void apply_settings(const slicer::Settings& s);
  void update_printer_source();
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
    // деталь повёрнута или масштабирована: прежняя постановка — поверхности задания переносятся
    std::optional<slicer::PrintJob> placed_from;
    std::function<void()> after;
    bool model = false;
    std::shared_ptr<const geometry::ImportedModel> mesh;
    slicer::PrintJob print;
    std::shared_ptr<const std::string> gcode;
    bool show_preview = false;  // после нарезки — во вкладку «Предпросмотр»
  };
  void start_load(LoadPlan plan);
  void render_print_settings();
  // подбор печати
  void on_goal(int k, bool on);
  advisor::Goals goals() const;
  void update_advise_state();
  void run_advisor();
  void render_advice();
  void apply_candidate(std::size_t k);
  void refit_panel();
  void update_print_state();
  void on_model_ready(std::shared_ptr<analysis::Model> model, const QString& path, long long max_elems, bool keep_job,
                      std::optional<slicer::PrintJob> placed_from = std::nullopt);
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
    double filament_g = 0, time_s = 0, support_g = 0;
    std::vector<std::string> warnings;
  };
  std::optional<ModelSource> source_;
  slicer::PrintJob print_ui_;  // настройки и положение детали сейчас (до «Нарезать» могут отличаться)
  std::shared_ptr<const app::Preview> preview_;  // нарезка для вкладки «Предпросмотр»
  app::PreviewView preview_view_;
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

  // виджеты: вкладки и общее
  QTabBar* tabs_ = nullptr;
  QStackedWidget* stack_ = nullptr;
  SettingsPanel* settings_ = nullptr;
  QWidget* busy_strip_ = nullptr;  // полоса хода фоновой работы под верхней панелью
  QProgressBar* busy_bar_ = nullptr;
  QLabel* busy_text_ = nullptr;
  Viewer* viewer_ = nullptr;
  QLabel* file_label_ = nullptr;
  QPushButton* btn_open_ = nullptr;
  QPushButton* btn_example_ = nullptr;
  QPushButton* btn_file_ = nullptr;
  QAction* act_open_job_ = nullptr;
  QAction* act_save_job_ = nullptr;
  QAction* act_report_ = nullptr;
  QAction* act_save_gcode_ = nullptr;
  // подготовка
  PlateView* plate_ = nullptr;
  QLabel* prep_info_ = nullptr;
  QWidget* prep_box_ = nullptr;  // поля положения (только для модели)
  QLabel* prep_note_ = nullptr;
  std::array<QLineEdit*, 2> pos_edit_{};
  std::array<QLineEdit*, 3> rot_edit_{};
  std::array<QLineEdit*, 3> scale_edit_{};
  std::array<QLineEdit*, 3> size_edit_{};
  QCheckBox* uniform_ = nullptr;
  QPushButton* btn_lay_ = nullptr;
  QVBoxLayout* prep_warns_ = nullptr;
  QLabel* fit_warn_ = nullptr;
  // предпросмотр
  PlateView* pview_ = nullptr;
  RangeSlider* layer_slider_ = nullptr;
  QSlider* move_slider_ = nullptr;
  QLabel* move_label_ = nullptr;
  QComboBox* color_combo_ = nullptr;
  QVBoxLayout* legend_list_ = nullptr;
  QLabel* totals_ = nullptr;
  QLabel* layer_info_ = nullptr;
  QFrame* stale_banner_ = nullptr;
  QPushButton* btn_print_ = nullptr;
  QPushButton* btn_send_ = nullptr;
  QPushButton* btn_save_gcode_ = nullptr;
  QCheckBox* auto_switch_ = nullptr;
  QVBoxLayout* preview_warns_ = nullptr;
  // принтер
  PrinterPanel* printer_ = nullptr;
  WebPane* web_ = nullptr;
  // расчёт нагрузок
  QLabel* strength_stale_ = nullptr;
  QPushButton* btn_reslice_ = nullptr;
  QCheckBox* supports_on_ = nullptr;  // показ поддержек на нитях
  bool refit_pending_ = false;
  std::array<QPushButton*, 3> goal_btn_{};
  std::vector<int> goal_order_;  // порядок выбора целей: третья вытесняет первую
  QPushButton* btn_advise_ = nullptr;
  QLabel* advise_summary_ = nullptr;
  QVBoxLayout* advise_list_ = nullptr;
  std::shared_ptr<const advisor::Result> advice_;
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
