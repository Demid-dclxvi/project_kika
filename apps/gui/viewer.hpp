#pragma once
// 3D-вид детали на OpenGL: грани вокселей с цветами из kika::app::Scene, сетка стола, оси,
// стрелки нагрузок, отметка слабого места, легенда и подсказка под курсором.
// Камера как в прототипе: вращение левой кнопкой, сдвиг правой или с Shift, колесо — масштаб.

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPointF>
#include <QVector3D>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "kika/app/roads.hpp"
#include "kika/app/scene.hpp"

namespace kika::gui {

// Стрелка нагрузки: приходит в центр граней по направлению vec.
struct Marker {
  std::vector<std::int64_t> keys;
  app::Vec3 vec{};
  app::Rgb color{};
};

class Viewer : public QOpenGLWidget, protected QOpenGLFunctions {
 public:
  // Что рисовать: нити из G-code или воксели расчётной сетки.
  enum class Display { Roads, Voxels };

  explicit Viewer(QWidget* parent = nullptr);
  ~Viewer() override;

  // Сцена и нити (принадлежат окну). nullptr — пусто.
  void set_scene(app::Scene* scene, const app::Roads* roads = nullptr);
  void set_display(Display d);
  Display display() const { return display_; }
  // Пересчитать грани (после смены разреза или сцены), позиции (деформация), цвета (поле, подсветка).
  void rebuild();
  void set_overlays(std::vector<app::Overlay> overlays);
  void set_markers(std::vector<Marker> markers);
  void set_critical(std::optional<app::Vec3> point);
  void set_deform(double scale);
  double deform() const { return deform_; }
  void view(const std::string& name);  // iso, front, top, right

  // Щелчок по детали: попадание (или пусто) и модификаторы.
  std::function<void(const std::optional<app::Hit>&, Qt::KeyboardModifiers)> on_pick;
  // Щелчок в точке вида (доли ширины и высоты) — для проверок без мыши.
  void click_at(double fx, double fy, Qt::KeyboardModifiers mods = {});
  // Текст, когда детали нет.
  void set_empty_text(const QString& text);

 protected:
  void initializeGL() override;
  void resizeGL(int w, int h) override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;
  void leaveEvent(QEvent* e) override;

 private:
  void update_camera();
  QMatrix4x4 view_matrix() const;
  QMatrix4x4 projection_matrix() const;
  std::pair<app::Vec3, app::Vec3> ray_at(QPointF p) const;
  std::optional<QPointF> project(const app::Vec3& p) const;
  void upload_positions();
  void upload_colors();
  void upload_roads();
  bool show_roads() const { return display_ == Display::Roads && roads_ != nullptr; }
  void build_helpers();
  void draw_lines(const std::vector<float>& verts, bool depth);
  void draw_overlay_2d();
  void rotate(double dx, double dy);
  void pan(double dx, double dy);

  app::Scene* scene_ = nullptr;
  const app::Roads* roads_ = nullptr;
  Display display_ = Display::Roads;
  std::vector<app::Overlay> overlays_;
  std::vector<Marker> markers_;
  std::optional<app::Vec3> critical_;
  double deform_ = 0.0;
  QString empty_text_;

  // камера
  QVector3D target_{0, 0, 0};
  double radius_ = 100, theta_ = -0.785, phi_ = 1.05;
  double fov_ = 35.0;

  // мышь
  QPointF last_{};
  QPointF press_pos_{};
  qint64 press_time_ = 0;
  double moved_ = 0;
  enum class Drag { None, Rotate, Pan } drag_ = Drag::None;
  Qt::KeyboardModifiers press_mods_{};
  std::optional<QPointF> hover_pos_;
  std::vector<std::string> hover_text_;

  // GL
  bool gl_ready_ = false;
  QOpenGLShaderProgram mesh_prog_, line_prog_;
  QOpenGLBuffer vbo_pos_{QOpenGLBuffer::VertexBuffer}, vbo_nrm_{QOpenGLBuffer::VertexBuffer},
      vbo_col_{QOpenGLBuffer::VertexBuffer}, ibo_{QOpenGLBuffer::IndexBuffer};
  int index_count_ = 0;
  bool geometry_dirty_ = true, positions_dirty_ = true, colors_dirty_ = true;
  // нити: пересобираются целиком при любой смене поля, подсветки, разреза или деформации
  QOpenGLBuffer road_pos_{QOpenGLBuffer::VertexBuffer}, road_nrm_{QOpenGLBuffer::VertexBuffer},
      road_col_{QOpenGLBuffer::VertexBuffer}, road_ibo_{QOpenGLBuffer::IndexBuffer};
  app::RoadMesh road_mesh_;
  int road_index_count_ = 0;
  bool roads_dirty_ = true;
  std::vector<float> grid_lines_, axis_lines_;  // x y z r g b
};

}  // namespace kika::gui
