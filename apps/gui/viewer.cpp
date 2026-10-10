// 3D-вид: перенос сцены three.js из fdmfea/web/viewer.js на OpenGL 2.x (работает и на программном
// рендеринге Mesa / opengl32sw.dll на машинах без видеокарты).

#include "viewer.hpp"

#include <QDateTime>
#include <QFontMetrics>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace kika::gui {

namespace {

const char* kMeshVs = R"(
attribute vec3 a_pos;
attribute vec3 a_nrm;
attribute vec3 a_col;
uniform mat4 u_mvp;
uniform mat4 u_view;
varying vec3 v_col;
varying vec3 v_n;
void main() {
  gl_Position = u_mvp * vec4(a_pos, 1.0);
  v_col = a_col;
  v_n = (u_view * vec4(a_nrm, 0.0)).xyz;
}
)";

// Освещение как в прототипе: полусферический свет (небо/земля) и два направленных от камеры.
const char* kMeshFs = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec3 v_col;
varying vec3 v_n;
uniform vec3 u_up;
uniform vec3 u_l1;
uniform vec3 u_l2;
void main() {
  vec3 n = normalize(v_n);
  if (!gl_FrontFacing) n = -n;
  float h = 0.5 * dot(n, u_up) + 0.5;
  vec3 hemi = mix(vec3(0.541, 0.541, 0.502), vec3(1.0), h) * 0.75;
  float d = max(dot(n, u_l1), 0.0) * 0.55 + max(dot(n, u_l2), 0.0) * 0.25;
  gl_FragColor = vec4(min(v_col * (hemi + vec3(d)), vec3(1.0)), 1.0);
}
)";

const char* kLineVs = R"(
attribute vec3 a_pos;
attribute vec3 a_col;
uniform mat4 u_mvp;
varying vec3 v_col;
void main() {
  gl_Position = u_mvp * vec4(a_pos, 1.0);
  v_col = a_col;
}
)";

const char* kLineFs = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec3 v_col;
void main() { gl_FragColor = vec4(v_col, 1.0); }
)";

// Шрифт крупнее или мельче основного (размер задан в пикселях или пунктах).
void scale_font(QFont& f, double k) {
  if (f.pixelSize() > 0)
    f.setPixelSize(std::max(1, static_cast<int>(std::lround(f.pixelSize() * k))));
  else
    f.setPointSizeF(f.pointSizeF() * k);
}

QColor qcolor(const app::Rgb& c) { return QColor::fromRgbF(c[0], c[1], c[2]); }

void push_vertex(std::vector<float>& v, const app::Vec3& p, const app::Rgb& c) {
  v.insert(v.end(), {static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), c[0], c[1], c[2]});
}

void push_line(std::vector<float>& v, const app::Vec3& a, const app::Vec3& b, const app::Rgb& c) {
  push_vertex(v, a, c);
  push_vertex(v, b, c);
}

app::Vec3 add(const app::Vec3& a, const app::Vec3& b, double k = 1.0) {
  return {a[0] + k * b[0], a[1] + k * b[1], a[2] + k * b[2]};
}

app::Vec3 cross(const app::Vec3& a, const app::Vec3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

app::Vec3 normalized(const app::Vec3& a) {
  const double l = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  return l > 0 ? app::Vec3{a[0] / l, a[1] / l, a[2] / l} : app::Vec3{0, 0, 1};
}

// Стрелка: стержень и конус-наконечник из треугольников. lines — для стержня, tris — для конуса.
void push_arrow(std::vector<float>& lines, std::vector<float>& tris, const app::Vec3& from, const app::Vec3& dir,
                double len, double head, double width, const app::Rgb& color) {
  const app::Vec3 d = normalized(dir);
  const app::Vec3 tip = add(from, d, len);
  const app::Vec3 base = add(from, d, len - head);
  push_line(lines, from, base, color);
  app::Vec3 u = cross(d, {0, 0, 1});
  if (std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]) < 1e-6) u = cross(d, {1, 0, 0});
  u = normalized(u);
  const app::Vec3 w = normalized(cross(d, u));
  constexpr int seg = 12;
  const app::Rgb dark{color[0] * 0.8f, color[1] * 0.8f, color[2] * 0.8f};
  for (int i = 0; i < seg; ++i) {
    const double a0 = 2 * std::numbers::pi * i / seg, a1 = 2 * std::numbers::pi * (i + 1) / seg;
    const app::Vec3 p0 = add(add(base, u, width * std::cos(a0)), w, width * std::sin(a0));
    const app::Vec3 p1 = add(add(base, u, width * std::cos(a1)), w, width * std::sin(a1));
    push_vertex(tris, tip, color);
    push_vertex(tris, p0, i % 2 ? dark : color);
    push_vertex(tris, p1, i % 2 ? dark : color);
    push_vertex(tris, base, dark);
    push_vertex(tris, p1, dark);
    push_vertex(tris, p0, dark);
  }
}

}  // namespace

Viewer::Viewer(QWidget* parent) : QOpenGLWidget(parent) {
  setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(320, 240);
}

Viewer::~Viewer() {
  makeCurrent();
  vbo_pos_.destroy();
  vbo_nrm_.destroy();
  vbo_col_.destroy();
  ibo_.destroy();
  doneCurrent();
}

void Viewer::set_scene(app::Scene* scene) {
  scene_ = scene;
  overlays_.clear();
  markers_.clear();
  critical_.reset();
  hover_text_.clear();
  deform_ = 0;
  build_helpers();
  rebuild();
  if (scene_) view("iso");
}

void Viewer::rebuild() {
  geometry_dirty_ = positions_dirty_ = colors_dirty_ = true;
  update();
}

void Viewer::set_overlays(std::vector<app::Overlay> overlays) {
  overlays_ = std::move(overlays);
  colors_dirty_ = true;
  update();
}

void Viewer::set_markers(std::vector<Marker> markers) {
  markers_ = std::move(markers);
  update();
}

void Viewer::set_critical(std::optional<app::Vec3> point) {
  critical_ = point;
  update();
}

void Viewer::set_deform(double scale) {
  if (scale == deform_) return;
  deform_ = scale;
  positions_dirty_ = true;
  update();
}

void Viewer::set_empty_text(const QString& text) {
  empty_text_ = text;
  update();
}

void Viewer::view(const std::string& name) {
  if (!scene_) return;
  const auto s = scene_->size();
  target_ = QVector3D(static_cast<float>(s[0] / 2), static_cast<float>(s[1] / 2), static_cast<float>(s[2] / 2));
  const double r = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]) / 2;
  const double aspect = height() > 0 ? static_cast<double>(width()) / height() : 1.0;
  radius_ = r / std::sin(fov_ / 2 * std::numbers::pi / 180) * 1.12 / std::min(1.0, aspect * 1.1);
  if (name == "top") {
    theta_ = -std::numbers::pi / 2;
    phi_ = 0.02;
  } else if (name == "front") {
    theta_ = -std::numbers::pi / 2;
    phi_ = std::numbers::pi / 2;
  } else if (name == "right") {
    theta_ = 0;
    phi_ = std::numbers::pi / 2;
  } else {
    theta_ = -std::numbers::pi / 3;
    phi_ = 1.0;
  }
  update_camera();
}

void Viewer::update_camera() { update(); }

QMatrix4x4 Viewer::view_matrix() const {
  const double s = std::sin(phi_);
  const QVector3D eye = target_ + QVector3D(static_cast<float>(s * std::cos(theta_)), static_cast<float>(s * std::sin(theta_)),
                                            static_cast<float>(std::cos(phi_))) *
                                      static_cast<float>(radius_);
  QMatrix4x4 v;
  v.lookAt(eye, target_, QVector3D(0, 0, 1));
  return v;
}

QMatrix4x4 Viewer::projection_matrix() const {
  QMatrix4x4 p;
  const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
  p.perspective(static_cast<float>(fov_), aspect, static_cast<float>(radius_ / 200), static_cast<float>(radius_ * 20));
  return p;
}

std::pair<app::Vec3, app::Vec3> Viewer::ray_at(QPointF pos) const {
  const QMatrix4x4 inv = (projection_matrix() * view_matrix()).inverted();
  const float x = static_cast<float>(2.0 * pos.x() / std::max(1, width()) - 1.0);
  const float y = static_cast<float>(1.0 - 2.0 * pos.y() / std::max(1, height()));
  const QVector4D a = inv * QVector4D(x, y, -1, 1);
  const QVector4D b = inv * QVector4D(x, y, 1, 1);
  const QVector3D p0 = a.toVector3DAffine(), p1 = b.toVector3DAffine();
  const QVector3D d = (p1 - p0).normalized();
  return {{p0.x(), p0.y(), p0.z()}, {d.x(), d.y(), d.z()}};
}

std::optional<QPointF> Viewer::project(const app::Vec3& p) const {
  const QVector4D c = projection_matrix() * view_matrix() *
                      QVector4D(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), 1);
  if (c.w() <= 0) return std::nullopt;
  const double x = (c.x() / c.w() + 1) / 2 * width();
  const double y = (1 - c.y() / c.w()) / 2 * height();
  return QPointF(x, y);
}

void Viewer::initializeGL() {
  initializeOpenGLFunctions();
  mesh_prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, kMeshVs);
  mesh_prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, kMeshFs);
  mesh_prog_.bindAttributeLocation("a_pos", 0);
  mesh_prog_.bindAttributeLocation("a_nrm", 1);
  mesh_prog_.bindAttributeLocation("a_col", 2);
  mesh_prog_.link();
  line_prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVs);
  line_prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFs);
  line_prog_.bindAttributeLocation("a_pos", 0);
  line_prog_.bindAttributeLocation("a_col", 1);
  line_prog_.link();
  for (auto* b : {&vbo_pos_, &vbo_nrm_, &vbo_col_, &ibo_}) {
    b->create();
    b->setUsagePattern(QOpenGLBuffer::DynamicDraw);
  }
  gl_ready_ = true;
  geometry_dirty_ = positions_dirty_ = colors_dirty_ = true;
}

void Viewer::resizeGL(int, int) {}

void Viewer::upload_positions() {
  const auto& faces = scene_->faces();
  std::vector<float> pos(faces.size() * 12), nrm(faces.size() * 12);
  std::array<app::Vec3, 4> c;
  for (std::size_t f = 0; f < faces.size(); ++f) {
    scene_->face_corners(f, deform_, c);
    for (std::size_t q = 0; q < 4; ++q)
      for (std::size_t a = 0; a < 3; ++a) pos[12 * f + 3 * q + a] = static_cast<float>(c[q][a]);
    // нормаль по диагоналям четырёхугольника
    const app::Vec3 d1{c[2][0] - c[0][0], c[2][1] - c[0][1], c[2][2] - c[0][2]};
    const app::Vec3 d2{c[3][0] - c[1][0], c[3][1] - c[1][1], c[3][2] - c[1][2]};
    const app::Vec3 n = normalized(cross(d1, d2));
    for (std::size_t q = 0; q < 4; ++q)
      for (std::size_t a = 0; a < 3; ++a) nrm[12 * f + 3 * q + a] = static_cast<float>(n[a]);
  }
  vbo_pos_.bind();
  vbo_pos_.allocate(pos.data(), static_cast<int>(pos.size() * sizeof(float)));
  vbo_nrm_.bind();
  vbo_nrm_.allocate(nrm.data(), static_cast<int>(nrm.size() * sizeof(float)));
}

void Viewer::upload_colors() {
  const auto colors = scene_->face_colors(overlays_);
  std::vector<float> col(colors.size() * 12);
  for (std::size_t f = 0; f < colors.size(); ++f)
    for (std::size_t q = 0; q < 4; ++q)
      for (std::size_t a = 0; a < 3; ++a) col[12 * f + 3 * q + a] = colors[f][a];
  vbo_col_.bind();
  vbo_col_.allocate(col.data(), static_cast<int>(col.size() * sizeof(float)));
}

void Viewer::build_helpers() {
  grid_lines_.clear();
  axis_lines_.clear();
  if (!scene_) return;
  const auto s = scene_->size();
  const double l = std::max(s[0], s[1]) * 1.6 + 20;
  const double step = l > 300 ? 50 : (l > 120 ? 10 : 5);
  const double half = std::ceil(l / 2 / step) * step;
  const double cx = s[0] / 2, cy = s[1] / 2, z = -0.01;
  const int n = static_cast<int>(std::round(2 * half / step));
  for (int i = 0; i <= n; ++i) {
    const double t = -half + i * step;
    const bool center = 2 * i == n;
    const app::Rgb c = center ? app::palette::kGrid : app::palette::kGrid2;
    push_line(grid_lines_, {cx + t, cy - half, z}, {cx + t, cy + half, z}, c);
    push_line(grid_lines_, {cx - half, cy + t, z}, {cx + half, cy + t, z}, c);
  }
}

void Viewer::draw_lines(const std::vector<float>& verts, bool depth) {
  if (verts.empty()) return;
  QOpenGLBuffer buf(QOpenGLBuffer::VertexBuffer);
  buf.create();
  buf.bind();
  buf.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(float)));
  if (depth)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  line_prog_.enableAttributeArray(0);
  line_prog_.enableAttributeArray(1);
  line_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
  line_prog_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 6));
  buf.release();
  buf.destroy();
}

void Viewer::paintGL() {
  const auto bg = app::palette::kBackground;
  glClearColor(bg[0], bg[1], bg[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (scene_ && gl_ready_) {
    if (geometry_dirty_) {
      const std::size_t nf = scene_->faces().size();
      std::vector<std::uint32_t> idx(nf * 6);
      for (std::size_t f = 0; f < nf; ++f) {
        const auto v = static_cast<std::uint32_t>(4 * f);
        const std::uint32_t q[6] = {v, v + 1, v + 2, v, v + 2, v + 3};
        std::copy(q, q + 6, idx.begin() + static_cast<std::ptrdiff_t>(6 * f));
      }
      ibo_.bind();
      ibo_.allocate(idx.data(), static_cast<int>(idx.size() * sizeof(std::uint32_t)));
      index_count_ = static_cast<int>(idx.size());
      geometry_dirty_ = false;
    }
    if (positions_dirty_) {
      upload_positions();
      positions_dirty_ = false;
    }
    if (colors_dirty_) {
      upload_colors();
      colors_dirty_ = false;
    }
    const QMatrix4x4 view = view_matrix();
    const QMatrix4x4 mvp = projection_matrix() * view;
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_CULL_FACE);
    // сетка стола и оси
    line_prog_.bind();
    line_prog_.setUniformValue("u_mvp", mvp);
    draw_lines(grid_lines_, true);
    // грани детали
    glEnable(GL_POLYGON_OFFSET_FILL);
    glPolygonOffset(1.0f, 1.0f);
    mesh_prog_.bind();
    mesh_prog_.setUniformValue("u_mvp", mvp);
    mesh_prog_.setUniformValue("u_view", view);
    const QVector3D up = (view * QVector4D(0, 0, 1, 0)).toVector3D().normalized();
    mesh_prog_.setUniformValue("u_up", up);
    mesh_prog_.setUniformValue("u_l1", QVector3D(1, -1.5f, 2).normalized());
    mesh_prog_.setUniformValue("u_l2", QVector3D(-1, 1, -0.5f).normalized());
    vbo_pos_.bind();
    mesh_prog_.enableAttributeArray(0);
    mesh_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3);
    vbo_nrm_.bind();
    mesh_prog_.enableAttributeArray(1);
    mesh_prog_.setAttributeBuffer(1, GL_FLOAT, 0, 3);
    vbo_col_.bind();
    mesh_prog_.enableAttributeArray(2);
    mesh_prog_.setAttributeBuffer(2, GL_FLOAT, 0, 3);
    ibo_.bind();
    glDrawElements(GL_TRIANGLES, index_count_, GL_UNSIGNED_INT, nullptr);
    mesh_prog_.disableAttributeArray(0);
    mesh_prog_.disableAttributeArray(1);
    mesh_prog_.disableAttributeArray(2);
    ibo_.release();
    vbo_col_.release();
    glDisable(GL_POLYGON_OFFSET_FILL);

    // оси и стрелки нагрузок — поверх
    const auto s = scene_->size();
    const double l = std::max({s[0], s[1], s[2]});
    std::vector<float> lines, tris;
    const double ax = std::clamp(0.25 * l, 8.0, 30.0);
    push_arrow(lines, tris, {0, 0, 0}, {1, 0, 0}, ax, ax * 0.18, ax * 0.09, app::palette::kAxisX);
    push_arrow(lines, tris, {0, 0, 0}, {0, 1, 0}, ax, ax * 0.18, ax * 0.09, app::palette::kAxisY);
    push_arrow(lines, tris, {0, 0, 0}, {0, 0, 1}, ax, ax * 0.18, ax * 0.09, app::palette::kAxisZ);
    for (const auto& mk : markers_) {
      const auto c = scene_->center(mk.keys, deform_);
      const double vl = std::sqrt(mk.vec[0] * mk.vec[0] + mk.vec[1] * mk.vec[1] + mk.vec[2] * mk.vec[2]);
      if (!c || vl < 1e-12) continue;
      const double len = std::max(6.0, 0.22 * l);
      const app::Vec3 d = normalized(mk.vec);
      push_arrow(lines, tris, add(*c, d, -len), d, len, len * 0.22, len * 0.11, mk.color);
    }
    line_prog_.bind();
    line_prog_.setUniformValue("u_mvp", mvp);
    glLineWidth(2.0f);
    draw_lines(lines, false);
    glLineWidth(1.0f);
    if (!tris.empty()) {
      QOpenGLBuffer buf(QOpenGLBuffer::VertexBuffer);
      buf.create();
      buf.bind();
      buf.allocate(tris.data(), static_cast<int>(tris.size() * sizeof(float)));
      line_prog_.enableAttributeArray(0);
      line_prog_.enableAttributeArray(1);
      line_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
      line_prog_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
      glEnable(GL_DEPTH_TEST);
      glClear(GL_DEPTH_BUFFER_BIT);  // наконечники поверх детали, но друг друга закрывают правильно
      glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(tris.size() / 6));
      buf.release();
      buf.destroy();
    }
    line_prog_.disableAttributeArray(0);
    line_prog_.disableAttributeArray(1);
    line_prog_.release();
  }
  draw_overlay_2d();
}

void Viewer::draw_overlay_2d() {
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor fg(0x16, 0x18, 0x1b), muted(0x61, 0x64, 0x6b);
  if (!scene_) {
    p.setPen(muted);
    QFont f = font();
    scale_font(f, 1.15);
    p.setFont(f);
    p.drawText(rect().adjusted(40, 40, -40, -40), Qt::AlignCenter | Qt::TextWordWrap, empty_text_);
    return;
  }
  // слабое место
  if (critical_) {
    if (auto sp = project(*critical_)) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0x16, 0x18, 0x1b, 240));
      p.drawEllipse(*sp, 8, 8);
      p.setBrush(QColor(255, 255, 255, 245));
      p.drawEllipse(*sp, 4, 4);
    }
  }
  // легенда
  const auto lg = scene_->legend();
  if (lg.kind != app::Legend::Kind::None) {
    QFont title = font();
    title.setBold(true);
    scale_font(title, 0.85);
    QFont body = font();
    scale_font(body, 0.9);
    const QFontMetrics fmt_title(title), fmt_body(body);
    const int pad = 10, row = fmt_body.height() + 4, sw = 12;
    int w = fmt_title.horizontalAdvance(QString::fromStdString(lg.title).toUpper()) + 2 * pad;
    int h = pad + fmt_title.height() + 6;
    if (lg.kind == app::Legend::Kind::Ramp) {
      w = std::max(w, 200);
      h += 10 + 4 + fmt_body.height() + pad;
    } else {
      for (const auto& e : lg.entries)
        w = std::max(w, 2 * pad + sw + 8 + fmt_body.horizontalAdvance(QString::fromStdString(e.label)));
      h += static_cast<int>(lg.entries.size()) * row + pad - 4;
    }
    const QRect box(12, height() - h - 12, w, h);
    p.setPen(QColor(0xdf, 0xe0, 0xda));
    p.setBrush(QColor(255, 255, 255, 225));
    p.drawRoundedRect(box, 6, 6);
    p.setFont(title);
    p.setPen(muted);
    p.drawText(box.left() + pad, box.top() + pad + fmt_title.ascent(), QString::fromStdString(lg.title).toUpper());
    int y = box.top() + pad + fmt_title.height() + 6;
    p.setFont(body);
    if (lg.kind == app::Legend::Kind::Ramp) {
      QLinearGradient g(box.left() + pad, 0, box.right() - pad, 0);
      for (std::size_t i = 0; i < lg.ramp.size(); ++i)
        g.setColorAt(static_cast<double>(i) / static_cast<double>(lg.ramp.size() - 1), qcolor(lg.ramp[i]));
      p.setPen(Qt::NoPen);
      p.setBrush(g);
      p.drawRoundedRect(QRect(box.left() + pad, y, box.width() - 2 * pad, 10), 3, 3);
      y += 14 + fmt_body.ascent();
      p.setPen(fg);
      p.drawText(box.left() + pad, y, "0");
      const QString mid = QString::fromStdString(app::fmt(lg.max / 2));
      p.drawText(box.center().x() - fmt_body.horizontalAdvance(mid) / 2, y, mid);
      const QString mx = QString::fromStdString(app::fmt(lg.max) + lg.unit);
      p.drawText(box.right() - pad - fmt_body.horizontalAdvance(mx), y, mx);
    } else {
      for (const auto& e : lg.entries) {
        p.setPen(Qt::NoPen);
        p.setBrush(qcolor(e.color));
        p.drawRoundedRect(QRect(box.left() + pad, y + (row - sw) / 2 - 1, sw, sw), 2, 2);
        p.setPen(fg);
        p.drawText(box.left() + pad + sw + 8, y + fmt_body.ascent() + 1, QString::fromStdString(e.label));
        y += row;
      }
    }
  }
  // подсказка об осях
  {
    QFont f = font();
    scale_font(f, 0.8);
    p.setFont(f);
    p.setPen(muted);
    const QString a = "X Y Z — оси принтера, мм от угла детали";
    const QString b = "вращение — мышь, сдвиг — правая кнопка или Shift, масштаб — колесо";
    const QFontMetrics fm(f);
    p.drawText(width() - 12 - fm.horizontalAdvance(b), height() - 12 - fm.descent(), b);
    p.drawText(width() - 12 - fm.horizontalAdvance(a), height() - 12 - fm.descent() - fm.height(), a);
  }
  // подсказка под курсором
  if (hover_pos_ && !hover_text_.empty()) {
    QFont f = font();
    scale_font(f, 0.9);
    p.setFont(f);
    const QFontMetrics fm(f);
    int w = 0;
    for (const auto& s : hover_text_) w = std::max(w, fm.horizontalAdvance(QString::fromStdString(s)));
    const int h = static_cast<int>(hover_text_.size()) * fm.height() + 12;
    w += 16;
    int x = static_cast<int>(hover_pos_->x()) + 14, y = static_cast<int>(hover_pos_->y()) - h - 10;
    x = std::min(x, width() - w - 8);
    y = std::max(y, 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x16, 0x18, 0x1b, 235));
    p.drawRoundedRect(QRect(x, y, w, h), 5, 5);
    p.setPen(QColor(0xf4, 0xf4, 0xf1));
    int ty = y + 6 + fm.ascent();
    for (const auto& s : hover_text_) {
      p.drawText(x + 8, ty, QString::fromStdString(s));
      ty += fm.height();
    }
  }
}

void Viewer::rotate(double dx, double dy) {
  theta_ -= dx * 0.008;
  phi_ = std::clamp(phi_ - dy * 0.008, 0.02, std::numbers::pi - 0.02);
  update_camera();
}

void Viewer::pan(double dx, double dy) {
  const double k = 2 * radius_ * std::tan(fov_ / 2 * std::numbers::pi / 180) / std::max(1, height());
  const QMatrix4x4 inv = view_matrix().inverted();
  const QVector3D right = inv.column(0).toVector3D(), up = inv.column(1).toVector3D();
  target_ += right * static_cast<float>(-dx * k) + up * static_cast<float>(dy * k);
  update_camera();
}

void Viewer::mousePressEvent(QMouseEvent* e) {
  last_ = press_pos_ = e->position();
  press_time_ = QDateTime::currentMSecsSinceEpoch();
  moved_ = 0;
  press_mods_ = e->modifiers();
  const bool pan_mode = e->button() == Qt::RightButton || e->button() == Qt::MiddleButton ||
                        (e->modifiers() & Qt::ShiftModifier);
  drag_ = pan_mode ? Drag::Pan : Drag::Rotate;
  hover_text_.clear();
  update();
}

void Viewer::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  const double dx = pos.x() - last_.x(), dy = pos.y() - last_.y();
  last_ = pos;
  if (drag_ != Drag::None) {
    moved_ += std::abs(dx) + std::abs(dy);
    // Shift при нажатии означает «добавить к выбору», если мышь почти не сдвинулась
    if (moved_ >= 5) {
      if (drag_ == Drag::Rotate)
        rotate(dx, dy);
      else
        pan(dx, dy);
    }
    return;
  }
  hover_pos_ = pos;
  if (scene_) {
    const auto [o, d] = ray_at(pos);
    const auto hit = scene_->pick(o, d, deform_);
    hover_text_ = hit ? scene_->describe(hit->elem) : std::vector<std::string>{};
  }
  update();
}

void Viewer::mouseReleaseEvent(QMouseEvent* e) {
  const bool click = drag_ != Drag::None && moved_ < 5 &&
                     QDateTime::currentMSecsSinceEpoch() - press_time_ < 600 && e->button() == Qt::LeftButton;
  drag_ = Drag::None;
  if (click && scene_ && on_pick) {
    const auto [o, d] = ray_at(e->position());
    on_pick(scene_->pick(o, d, deform_), press_mods_ | e->modifiers());
  }
}

void Viewer::click_at(double fx, double fy, Qt::KeyboardModifiers mods) {
  if (!scene_ || !on_pick) return;
  const auto [o, d] = ray_at(QPointF(fx * width(), fy * height()));
  on_pick(scene_->pick(o, d, deform_), mods);
}

void Viewer::wheelEvent(QWheelEvent* e) {
  const double steps = e->angleDelta().y();
  radius_ = std::clamp(radius_ * std::exp(-steps * 0.001), 1.0, 5000.0);
  update_camera();
  e->accept();
}

void Viewer::leaveEvent(QEvent*) {
  hover_pos_.reset();
  hover_text_.clear();
  update();
}

}  // namespace kika::gui
