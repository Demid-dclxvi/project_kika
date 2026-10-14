#pragma once
// Стол принтера в 3D: пластина с сеткой, область печати, деталь в её положении на столе
// (вкладка «Подготовка») или нити нарезки по слоям (вкладка «Предпросмотр»). Деталь можно
// перетащить мышью по столу и положить на выбранную грань. Камера — как у вида расчёта.

#include <QMatrix4x4>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QOpenGLWidget>
#include <QPointF>
#include <QVector3D>

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "kika/app/preview.hpp"
#include "kika/geometry/mesh.hpp"
#include "kika/slicer/slicer.hpp"

namespace kika::gui {

class PlateView : public QOpenGLWidget, protected QOpenGLFunctions {
 public:
  enum class Mode { Prepare, Preview };

  explicit PlateView(Mode mode, QWidget* parent = nullptr);
  ~PlateView() override;

  // Стол и область печати принтера.
  void set_printer(const slicer::Printer& p);

  // Подготовка: сетка детали в своих координатах и её постановка на стол (мир = place · точка).
  void set_model(std::shared_ptr<const geometry::TriangleMesh> mesh);
  void set_placement(const geometry::Transform& place, bool fits);
  // Перетаскивание детали по столу: сдвиг от начала, done — кнопку отпустили.
  std::function<void(double dx, double dy, bool done)> on_drag;
  // Режим «положить на грань»: щелчок по детали отдаёт нормаль грани (в координатах стола).
  void set_face_pick(bool on);
  bool face_pick() const { return face_pick_; }
  // Щелчок в точке вида (доли ширины и высоты) — для проверок без мыши: в режиме «на грань».
  bool click_at(double fx, double fy);
  std::function<void(const geometry::Vec3& normal)> on_face;

  // Предпросмотр: движения нарезки и как их показывать (сетка нитей пересобирается).
  void set_preview(std::shared_ptr<const app::Preview> p, const app::PreviewView& v);
  // Показать слои [lo, hi], верхний — до движения upto (пусто — весь).
  void set_layers(std::size_t lo, std::size_t hi, std::optional<std::size_t> upto = std::nullopt);

  void view(const std::string& name);  // iso, top, front, right
  void set_empty_text(const QString& text);
  // Строки в левом верхнем углу (размер детали, слой…).
  void set_info(const QStringList& lines);

 protected:
  void initializeGL() override;
  void paintGL() override;
  void mousePressEvent(QMouseEvent* e) override;
  void mouseMoveEvent(QMouseEvent* e) override;
  void mouseReleaseEvent(QMouseEvent* e) override;
  void wheelEvent(QWheelEvent* e) override;

 private:
  QMatrix4x4 view_matrix() const;
  QMatrix4x4 projection_matrix() const;
  std::pair<geometry::Vec3, geometry::Vec3> ray_at(QPointF p) const;
  std::optional<QPointF> project(const geometry::Vec3& p) const;
  // Попадание луча в деталь: точка и нормаль грани в координатах стола.
  std::optional<std::pair<geometry::Vec3, geometry::Vec3>> pick_model(QPointF p) const;
  void build_bed();
  void upload_model();
  void upload_preview();
  void draw_lines(const std::vector<float>& verts, bool depth, float width = 1.0f);

  Mode mode_;
  slicer::Printer printer_;
  std::shared_ptr<const geometry::TriangleMesh> mesh_;
  geometry::Transform place_;
  bool fits_ = true;
  bool face_pick_ = false;
  std::shared_ptr<const app::Preview> preview_;
  app::PreviewView pview_;
  app::PreviewMesh pmesh_;
  std::size_t lo_ = 0, hi_ = 0;
  std::optional<std::size_t> upto_;
  QString empty_text_;
  QStringList info_;

  // камера
  QVector3D target_{135, 135, 0};
  double radius_ = 500, theta_ = -1.2, phi_ = 0.95, fov_ = 35.0;
  bool fitted_ = false;  // камера ещё не наведена на стол (до первого показа размер вида неизвестен)
  // мышь
  QPointF last_{}, press_{};
  double moved_ = 0;
  enum class Drag { None, Rotate, Pan, Part } drag_ = Drag::None;
  geometry::Vec3 drag_origin_{};  // точка на детали, за которую тянут (плоскость z = её высота)
  double drag_dx_ = 0, drag_dy_ = 0;

  // GL
  bool gl_ready_ = false;
  QOpenGLShaderProgram mesh_prog_, line_prog_;
  QOpenGLBuffer m_pos_{QOpenGLBuffer::VertexBuffer}, m_nrm_{QOpenGLBuffer::VertexBuffer};
  int model_vertices_ = 0;
  bool model_dirty_ = true;
  QOpenGLBuffer p_pos_{QOpenGLBuffer::VertexBuffer}, p_nrm_{QOpenGLBuffer::VertexBuffer},
      p_col_{QOpenGLBuffer::VertexBuffer}, p_ibo_{QOpenGLBuffer::IndexBuffer}, p_travel_{QOpenGLBuffer::VertexBuffer};
  bool preview_dirty_ = true;
  std::vector<float> bed_fill_, bed_lines_, volume_lines_;  // x y z r g b
};

}  // namespace kika::gui
