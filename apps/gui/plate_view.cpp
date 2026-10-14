// Стол принтера в 3D: OpenGL 2.x, как и вид расчёта (работает и на программном рендеринге).

#include "plate_view.hpp"

#include <QFontMetrics>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace kika::gui {

namespace {

using geometry::Vec3;

// Деталь: матрица постановки в вершинном шейдере (перетаскивание не перезагружает сетку).
const char* kModelVs = R"(
attribute vec3 a_pos;
attribute vec3 a_nrm;
uniform mat4 u_mvp;
uniform mat4 u_view;
uniform mat4 u_model;
uniform mat3 u_nmat;
varying vec3 v_n;
void main() {
  gl_Position = u_mvp * u_model * vec4(a_pos, 1.0);
  v_n = (u_view * vec4(u_nmat * a_nrm, 0.0)).xyz;
}
)";

const char* kModelFs = R"(
#ifdef GL_ES
precision mediump float;
#endif
varying vec3 v_n;
uniform vec3 u_col;
uniform vec3 u_up;
uniform vec3 u_l1;
uniform vec3 u_l2;
void main() {
  vec3 n = normalize(v_n);
  if (!gl_FrontFacing) n = -n;
  float h = 0.5 * dot(n, u_up) + 0.5;
  vec3 hemi = mix(vec3(0.55, 0.55, 0.52), vec3(1.0), h) * 0.72;
  float d = max(dot(n, u_l1), 0.0) * 0.55 + max(dot(n, u_l2), 0.0) * 0.25;
  gl_FragColor = vec4(min(u_col * (hemi + vec3(d)), vec3(1.0)), 1.0);
}
)";

// Нити предпросмотра: цвет и нормаль — байты на вершину.
const char* kRoadVs = R"(
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

const char* kRoadFs = R"(
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
  float h = 0.6 + 0.4 * abs(dot(n, u_up));
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

void push_v(std::vector<float>& v, double x, double y, double z, const app::Rgb& c) {
  v.insert(v.end(), {static_cast<float>(x), static_cast<float>(y), static_cast<float>(z), c[0], c[1], c[2]});
}

void push_l(std::vector<float>& v, const Vec3& a, const Vec3& b, const app::Rgb& c) {
  push_v(v, a[0], a[1], a[2], c);
  push_v(v, b[0], b[1], b[2], c);
}

QMatrix4x4 qmat(const geometry::Transform& t) {
  return QMatrix4x4(static_cast<float>(t.r[0]), static_cast<float>(t.r[1]), static_cast<float>(t.r[2]), static_cast<float>(t.t[0]),
                    static_cast<float>(t.r[3]), static_cast<float>(t.r[4]), static_cast<float>(t.r[5]), static_cast<float>(t.t[1]),
                    static_cast<float>(t.r[6]), static_cast<float>(t.r[7]), static_cast<float>(t.r[8]), static_cast<float>(t.t[2]),
                    0, 0, 0, 1);
}

// Обратная к линейной части 3 × 3 (по строкам).
std::array<double, 9> inverse3(const std::array<double, 9>& m) {
  const double c0 = m[4] * m[8] - m[5] * m[7], c1 = m[5] * m[6] - m[3] * m[8], c2 = m[3] * m[7] - m[4] * m[6];
  const double det = m[0] * c0 + m[1] * c1 + m[2] * c2;
  const double k = std::abs(det) > 1e-300 ? 1.0 / det : 0.0;
  return {c0 * k, (m[2] * m[7] - m[1] * m[8]) * k, (m[1] * m[5] - m[2] * m[4]) * k,
          c1 * k, (m[0] * m[8] - m[2] * m[6]) * k, (m[2] * m[3] - m[0] * m[5]) * k,
          c2 * k, (m[1] * m[6] - m[0] * m[7]) * k, (m[0] * m[4] - m[1] * m[3]) * k};
}

Vec3 mul3(const std::array<double, 9>& m, const Vec3& v) {
  return {m[0] * v[0] + m[1] * v[1] + m[2] * v[2], m[3] * v[0] + m[4] * v[1] + m[5] * v[2], m[6] * v[0] + m[7] * v[1] + m[8] * v[2]};
}

double ray_tri(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const Vec3 p{d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
  const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::abs(det) < 1e-18) return -1;
  const double inv = 1.0 / det;
  const Vec3 s{o[0] - a[0], o[1] - a[1], o[2] - a[2]};
  const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return -1;
  const Vec3 q{s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
  const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return -1;
  return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

void scale_font(QFont& f, double k) {
  if (f.pixelSize() > 0)
    f.setPixelSize(std::max(1, static_cast<int>(std::lround(f.pixelSize() * k))));
  else
    f.setPointSizeF(f.pointSizeF() * k);
}

}  // namespace

PlateView::PlateView(Mode mode, QWidget* parent) : QOpenGLWidget(parent), mode_(mode) {
  setMouseTracking(true);
  setFocusPolicy(Qt::ClickFocus);
  setMinimumSize(320, 240);
  build_bed();
}

PlateView::~PlateView() {
  makeCurrent();
  for (auto* b : {&m_pos_, &m_nrm_, &p_pos_, &p_nrm_, &p_col_, &p_ibo_, &p_travel_}) b->destroy();
  doneCurrent();
}

void PlateView::set_printer(const slicer::Printer& p) {
  const bool same = p.bed_x == printer_.bed_x && p.bed_y == printer_.bed_y && p.max_z == printer_.max_z;
  printer_ = p;
  build_bed();
  if (!same) fitted_ = false;
  update();
}

void PlateView::set_model(std::shared_ptr<const geometry::TriangleMesh> mesh) {
  if (mesh.get() == mesh_.get()) return;
  mesh_ = std::move(mesh);
  model_dirty_ = true;
  update();
}

void PlateView::set_placement(const geometry::Transform& place, bool fits) {
  place_ = place;
  fits_ = fits;
  drag_dx_ = drag_dy_ = 0;
  update();
}

void PlateView::set_face_pick(bool on) {
  face_pick_ = on;
  setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
  update();
}

bool PlateView::click_at(double fx, double fy) {
  if (!face_pick_ || !on_face) return false;
  if (!fitted_) view("iso");
  const auto hit = pick_model(QPointF(fx * width(), fy * height()));
  if (!hit) return false;
  on_face(hit->second);
  return true;
}

void PlateView::set_preview(std::shared_ptr<const app::Preview> p, const app::PreviewView& v) {
  preview_ = std::move(p);
  pview_ = v;
  if (preview_) app::build_preview_mesh(*preview_, pview_, pmesh_);
  else pmesh_ = {};
  preview_dirty_ = true;
  update();
}

void PlateView::set_layers(std::size_t lo, std::size_t hi, std::optional<std::size_t> upto) {
  lo_ = lo;
  hi_ = hi;
  upto_ = upto;
  update();
}

void PlateView::set_empty_text(const QString& text) {
  empty_text_ = text;
  update();
}

void PlateView::set_info(const QStringList& lines) {
  info_ = lines;
  update();
}

void PlateView::view(const std::string& name) {
  fitted_ = true;
  const double bx = printer_.bed_x, by = printer_.bed_y, bz = printer_.max_z;
  target_ = QVector3D(static_cast<float>(bx / 2), static_cast<float>(by / 2), static_cast<float>(std::min(bz, 60.0) / 2));
  double r = std::sqrt(bx * bx + by * by) / 2;
  // «деталь» — камера на деталь (или нарезку), углы — как у изометрии
  if (name == "part") {
    std::optional<std::pair<Vec3, Vec3>> box;
    if (mode_ == Mode::Prepare && mesh_ && !mesh_->empty()) {
      Vec3 lo{1e300, 1e300, 1e300}, hi{-1e300, -1e300, -1e300};
      for (const auto& v : mesh_->vertices) {
        const auto q = place_.apply(v);
        for (std::size_t a = 0; a < 3; ++a) {
          lo[a] = std::min(lo[a], q[a]);
          hi[a] = std::max(hi[a], q[a]);
        }
      }
      box = std::pair{lo, hi};
    } else if (mode_ == Mode::Preview && preview_ && !preview_->empty()) {
      box = std::pair{Vec3{preview_->min[0], preview_->min[1], preview_->min[2]},
                      Vec3{preview_->max[0], preview_->max[1], preview_->max[2]}};
    }
    if (box) {
      const auto& [lo, hi] = *box;
      target_ = QVector3D(static_cast<float>((lo[0] + hi[0]) / 2), static_cast<float>((lo[1] + hi[1]) / 2),
                          static_cast<float>((lo[2] + hi[2]) / 2));
      r = std::max(5.0, 0.5 * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                        (hi[2] - lo[2]) * (hi[2] - lo[2])));
    }
  }
  const double aspect = height() > 0 ? static_cast<double>(width()) / height() : 1.4;
  radius_ = r / std::sin(fov_ / 2 * std::numbers::pi / 180) * (name == "part" ? 1.25 : 1.05) / std::min(1.0, aspect * 0.9);
  if (name == "top") {
    theta_ = -std::numbers::pi / 2;
    phi_ = 0.02;
  } else if (name == "front") {
    theta_ = -std::numbers::pi / 2;
    phi_ = std::numbers::pi / 2 - 0.05;
  } else if (name == "right") {
    theta_ = 0;
    phi_ = std::numbers::pi / 2 - 0.05;
  } else {
    theta_ = -std::numbers::pi / 2 - 0.55;
    phi_ = 0.95;
  }
  update();
}

void PlateView::build_bed() {
  bed_fill_.clear();
  bed_lines_.clear();
  volume_lines_.clear();
  const double bx = printer_.bed_x, by = printer_.bed_y, bz = printer_.max_z;
  const app::Rgb fill = app::rgb(0xDCDED6), g1 = app::rgb(0xCACCC3), g5 = app::rgb(0xB4B7AD), edge = app::rgb(0x8F938A);
  const double z = -0.02;
  for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{bx, 0.0}, std::pair{bx, by}, std::pair{0.0, 0.0}, std::pair{bx, by},
                             std::pair{0.0, by}})
    push_v(bed_fill_, x, y, z - 0.05, fill);
  for (double x = 10; x < bx - 1e-6; x += 10)
    push_l(bed_lines_, {x, 0, z}, {x, by, z}, std::fmod(x, 50) < 1e-6 ? g5 : g1);
  for (double y = 10; y < by - 1e-6; y += 10)
    push_l(bed_lines_, {0, y, z}, {bx, y, z}, std::fmod(y, 50) < 1e-6 ? g5 : g1);
  push_l(bed_lines_, {0, 0, z}, {bx, 0, z}, edge);
  push_l(bed_lines_, {bx, 0, z}, {bx, by, z}, edge);
  push_l(bed_lines_, {bx, by, z}, {0, by, z}, edge);
  push_l(bed_lines_, {0, by, z}, {0, 0, z}, edge);
  // область печати — тонкий каркас
  const app::Rgb vc = app::rgb(0xB9BCB2);
  for (const auto& [x, y] : {std::pair{0.0, 0.0}, std::pair{bx, 0.0}, std::pair{bx, by}, std::pair{0.0, by}})
    push_l(volume_lines_, {x, y, 0}, {x, y, bz}, vc);
  push_l(volume_lines_, {0, 0, bz}, {bx, 0, bz}, vc);
  push_l(volume_lines_, {bx, 0, bz}, {bx, by, bz}, vc);
  push_l(volume_lines_, {bx, by, bz}, {0, by, bz}, vc);
  push_l(volume_lines_, {0, by, bz}, {0, 0, bz}, vc);
  // оси у начала координат стола
  const double a = std::clamp(0.12 * std::min(bx, by), 10.0, 40.0);
  push_l(volume_lines_, {0, 0, 0.05}, {a, 0, 0.05}, app::palette::kAxisX);
  push_l(volume_lines_, {0, 0, 0.05}, {0, a, 0.05}, app::palette::kAxisY);
  push_l(volume_lines_, {0, 0, 0.05}, {0, 0, a}, app::palette::kAxisZ);
}

QMatrix4x4 PlateView::view_matrix() const {
  const double s = std::sin(phi_);
  const QVector3D eye = target_ + QVector3D(static_cast<float>(s * std::cos(theta_)), static_cast<float>(s * std::sin(theta_)),
                                            static_cast<float>(std::cos(phi_))) *
                                      static_cast<float>(radius_);
  QMatrix4x4 v;
  v.lookAt(eye, target_, QVector3D(0, 0, 1));
  return v;
}

QMatrix4x4 PlateView::projection_matrix() const {
  QMatrix4x4 p;
  const float aspect = height() > 0 ? static_cast<float>(width()) / static_cast<float>(height()) : 1.0f;
  p.perspective(static_cast<float>(fov_), aspect, static_cast<float>(radius_ / 200), static_cast<float>(radius_ * 20));
  return p;
}

std::pair<Vec3, Vec3> PlateView::ray_at(QPointF pos) const {
  const QMatrix4x4 inv = (projection_matrix() * view_matrix()).inverted();
  const float x = static_cast<float>(2.0 * pos.x() / std::max(1, width()) - 1.0);
  const float y = static_cast<float>(1.0 - 2.0 * pos.y() / std::max(1, height()));
  const QVector3D p0 = (inv * QVector4D(x, y, -1, 1)).toVector3DAffine(), p1 = (inv * QVector4D(x, y, 1, 1)).toVector3DAffine();
  const QVector3D d = (p1 - p0).normalized();
  return {{p0.x(), p0.y(), p0.z()}, {d.x(), d.y(), d.z()}};
}

std::optional<QPointF> PlateView::project(const Vec3& p) const {
  const QVector4D c = projection_matrix() * view_matrix() *
                      QVector4D(static_cast<float>(p[0]), static_cast<float>(p[1]), static_cast<float>(p[2]), 1);
  if (c.w() <= 0) return std::nullopt;
  return QPointF((c.x() / c.w() + 1.0) / 2.0 * static_cast<double>(width()), (1.0 - c.y() / c.w()) / 2.0 * static_cast<double>(height()));
}

std::optional<std::pair<Vec3, Vec3>> PlateView::pick_model(QPointF pos) const {
  if (!mesh_ || mesh_->empty() || mode_ != Mode::Prepare) return std::nullopt;
  const auto [o, d] = ray_at(pos);
  // луч — в систему детали: точка = L⁻¹(мир − t)
  const auto inv = inverse3(place_.r);
  const Vec3 om = mul3(inv, {o[0] - place_.t[0] - drag_dx_, o[1] - place_.t[1] - drag_dy_, o[2] - place_.t[2]});
  const Vec3 dm = mul3(inv, d);
  double best = 1e300;
  std::size_t hit = 0;
  const auto& v = mesh_->vertices;
  for (std::size_t t = 0; t < mesh_->triangles.size(); ++t) {
    const auto& tr = mesh_->triangles[t];
    const double s = ray_tri(om, dm, v[tr[0]], v[tr[1]], v[tr[2]]);
    if (s > 0 && s < best) {
      best = s;
      hit = t;
    }
  }
  if (best >= 1e300) return std::nullopt;
  const auto& tr = mesh_->triangles[hit];
  const Vec3 &a = v[tr[0]], &b = v[tr[1]], &c = v[tr[2]];
  const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const Vec3 nm{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
  // нормаль в мир: (L⁻¹)ᵀ · n
  const std::array<double, 9> it{inv[0], inv[3], inv[6], inv[1], inv[4], inv[7], inv[2], inv[5], inv[8]};
  Vec3 nw = mul3(it, nm);
  const double l = std::sqrt(nw[0] * nw[0] + nw[1] * nw[1] + nw[2] * nw[2]);
  if (l < 1e-18) return std::nullopt;
  for (auto& x : nw) x /= l;
  const Vec3 p{o[0] + best * d[0], o[1] + best * d[1], o[2] + best * d[2]};
  return std::pair{p, nw};
}

void PlateView::initializeGL() {
  initializeOpenGLFunctions();
  mesh_prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, mode_ == Mode::Prepare ? kModelVs : kRoadVs);
  mesh_prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, mode_ == Mode::Prepare ? kModelFs : kRoadFs);
  mesh_prog_.bindAttributeLocation("a_pos", 0);
  mesh_prog_.bindAttributeLocation("a_nrm", 1);
  mesh_prog_.bindAttributeLocation("a_col", 2);
  mesh_prog_.link();
  line_prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, kLineVs);
  line_prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, kLineFs);
  line_prog_.bindAttributeLocation("a_pos", 0);
  line_prog_.bindAttributeLocation("a_col", 1);
  line_prog_.link();
  for (auto* b : {&m_pos_, &m_nrm_, &p_pos_, &p_nrm_, &p_col_, &p_ibo_, &p_travel_}) {
    b->create();
    b->setUsagePattern(QOpenGLBuffer::StaticDraw);
  }
  gl_ready_ = true;
  model_dirty_ = preview_dirty_ = true;
}

void PlateView::upload_model() {
  model_vertices_ = 0;
  if (!mesh_ || mesh_->empty()) return;
  // плоские грани: у каждого треугольника свои вершины и нормаль
  const auto& v = mesh_->vertices;
  std::vector<float> pos, nrm;
  pos.reserve(mesh_->triangles.size() * 9);
  nrm.reserve(mesh_->triangles.size() * 9);
  for (const auto& tr : mesh_->triangles) {
    const Vec3 &a = v[tr[0]], &b = v[tr[1]], &c = v[tr[2]];
    const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
    Vec3 n{e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const double l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (l > 0)
      for (auto& x : n) x /= l;
    for (const Vec3* p : {&a, &b, &c})
      for (std::size_t i = 0; i < 3; ++i) {
        pos.push_back(static_cast<float>((*p)[i]));
        nrm.push_back(static_cast<float>(n[i]));
      }
  }
  m_pos_.bind();
  m_pos_.allocate(pos.data(), static_cast<int>(pos.size() * sizeof(float)));
  m_nrm_.bind();
  m_nrm_.allocate(nrm.data(), static_cast<int>(nrm.size() * sizeof(float)));
  model_vertices_ = static_cast<int>(pos.size() / 3);
}

void PlateView::upload_preview() {
  const auto& m = pmesh_.mesh;
  p_pos_.bind();
  p_pos_.allocate(m.pos.data(), static_cast<int>(m.pos.size() * sizeof(float)));
  p_nrm_.bind();
  p_nrm_.allocate(m.nrm.data(), static_cast<int>(m.nrm.size()));
  p_col_.bind();
  p_col_.allocate(m.col.data(), static_cast<int>(m.col.size()));
  p_ibo_.bind();
  p_ibo_.allocate(m.idx.data(), static_cast<int>(m.idx.size() * sizeof(std::uint32_t)));
  p_travel_.bind();
  p_travel_.allocate(pmesh_.travel.data(), static_cast<int>(pmesh_.travel.size() * sizeof(float)));
}

void PlateView::draw_lines(const std::vector<float>& verts, bool depth, float width) {
  if (verts.empty()) return;
  QOpenGLBuffer buf(QOpenGLBuffer::VertexBuffer);
  buf.create();
  buf.bind();
  buf.allocate(verts.data(), static_cast<int>(verts.size() * sizeof(float)));
  if (depth)
    glEnable(GL_DEPTH_TEST);
  else
    glDisable(GL_DEPTH_TEST);
  glLineWidth(width);
  line_prog_.enableAttributeArray(0);
  line_prog_.enableAttributeArray(1);
  line_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
  line_prog_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
  glDrawArrays(GL_LINES, 0, static_cast<GLsizei>(verts.size() / 6));
  line_prog_.disableAttributeArray(0);
  line_prog_.disableAttributeArray(1);
  glLineWidth(1.0f);
  buf.release();
  buf.destroy();
}

void PlateView::paintGL() {
  const auto bg = app::palette::kBackground;
  glClearColor(bg[0], bg[1], bg[2], 1);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  if (!gl_ready_) return;
  if (!fitted_) view("iso");
  const QMatrix4x4 view = view_matrix();
  const QMatrix4x4 mvp = projection_matrix() * view;
  const QVector3D up = (view * QVector4D(0, 0, 1, 0)).toVector3D().normalized();
  glEnable(GL_DEPTH_TEST);
  glDepthFunc(GL_LEQUAL);
  glDisable(GL_CULL_FACE);
  // стол
  line_prog_.bind();
  line_prog_.setUniformValue("u_mvp", mvp);
  if (!bed_fill_.empty()) {
    QOpenGLBuffer buf(QOpenGLBuffer::VertexBuffer);
    buf.create();
    buf.bind();
    buf.allocate(bed_fill_.data(), static_cast<int>(bed_fill_.size() * sizeof(float)));
    line_prog_.enableAttributeArray(0);
    line_prog_.enableAttributeArray(1);
    line_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3, 6 * sizeof(float));
    line_prog_.setAttributeBuffer(1, GL_FLOAT, 3 * sizeof(float), 3, 6 * sizeof(float));
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(bed_fill_.size() / 6));
    line_prog_.disableAttributeArray(0);
    line_prog_.disableAttributeArray(1);
    buf.release();
    buf.destroy();
  }
  draw_lines(bed_lines_, true);
  draw_lines(volume_lines_, true);

  glEnable(GL_POLYGON_OFFSET_FILL);
  glPolygonOffset(1.0f, 1.0f);
  mesh_prog_.bind();
  mesh_prog_.setUniformValue("u_mvp", mvp);
  mesh_prog_.setUniformValue("u_view", view);
  mesh_prog_.setUniformValue("u_up", up);
  // свет сверху спереди и слабый сзади — в координатах стола: верх детали светлый при любом повороте камеры
  mesh_prog_.setUniformValue("u_l1", (view * QVector4D(0.35f, -0.6f, 1.0f, 0)).toVector3D().normalized());
  mesh_prog_.setUniformValue("u_l2", (view * QVector4D(-0.6f, 0.7f, 0.45f, 0)).toVector3D().normalized());
  if (mode_ == Mode::Prepare) {
    if (model_dirty_) {
      upload_model();
      model_dirty_ = false;
    }
    if (model_vertices_ > 0) {
      geometry::Transform t = place_;
      t.t[0] += drag_dx_;
      t.t[1] += drag_dy_;
      mesh_prog_.setUniformValue("u_model", qmat(t));
      // нормали: (L⁻¹)ᵀ — верно и при неравномерном масштабе
      const auto inv = inverse3(place_.r);
      const float nm[9] = {static_cast<float>(inv[0]), static_cast<float>(inv[3]), static_cast<float>(inv[6]),
                           static_cast<float>(inv[1]), static_cast<float>(inv[4]), static_cast<float>(inv[7]),
                           static_cast<float>(inv[2]), static_cast<float>(inv[5]), static_cast<float>(inv[8])};
      // QMatrix3x3 — по строкам: здесь (L⁻¹)ᵀ
      mesh_prog_.setUniformValue("u_nmat", QMatrix3x3(nm));
      const app::Rgb c = !fits_ ? app::rgb(0xE07A5F) : (face_pick_ ? app::rgb(0x9DB7DD) : app::rgb(0xC7CBBE));
      mesh_prog_.setUniformValue("u_col", QVector3D(c[0], c[1], c[2]));
      m_pos_.bind();
      mesh_prog_.enableAttributeArray(0);
      mesh_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3);
      m_nrm_.bind();
      mesh_prog_.enableAttributeArray(1);
      mesh_prog_.setAttributeBuffer(1, GL_FLOAT, 0, 3);
      glDrawArrays(GL_TRIANGLES, 0, model_vertices_);
      mesh_prog_.disableAttributeArray(0);
      mesh_prog_.disableAttributeArray(1);
      m_nrm_.release();
    }
  } else if (preview_ && !preview_->empty()) {
    if (preview_dirty_) {
      upload_preview();
      preview_dirty_ = false;
    }
    const auto [a, b] = app::preview_range(*preview_, pmesh_, lo_, hi_, upto_);
    if (b > a) {
      p_pos_.bind();
      mesh_prog_.enableAttributeArray(0);
      mesh_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3);
      p_nrm_.bind();
      mesh_prog_.enableAttributeArray(1);
      mesh_prog_.setAttributeBuffer(1, GL_BYTE, 0, 3, 4);
      p_col_.bind();
      mesh_prog_.enableAttributeArray(2);
      mesh_prog_.setAttributeBuffer(2, GL_UNSIGNED_BYTE, 0, 3, 4);
      p_ibo_.bind();
      glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(b - a), GL_UNSIGNED_INT,
                     reinterpret_cast<const void*>(static_cast<std::uintptr_t>(a) * sizeof(std::uint32_t)));
      mesh_prog_.disableAttributeArray(0);
      mesh_prog_.disableAttributeArray(1);
      mesh_prog_.disableAttributeArray(2);
      p_ibo_.release();
      p_col_.release();
    }
    const auto [ta, tb] = app::travel_range(*preview_, pmesh_, lo_, hi_, upto_);
    if (tb > ta) {
      line_prog_.bind();
      line_prog_.setUniformValue("u_mvp", mvp);
      p_travel_.bind();
      line_prog_.enableAttributeArray(0);
      line_prog_.setAttributeBuffer(0, GL_FLOAT, 0, 3);
      const auto tc = app::line_type_color(app::LineType::Travel);
      line_prog_.setAttributeValue(1, tc[0], tc[1], tc[2]);
      glDrawArrays(GL_LINES, static_cast<GLint>(ta), static_cast<GLsizei>(tb - ta));
      line_prog_.disableAttributeArray(0);
      p_travel_.release();
    }
  }
  glDisable(GL_POLYGON_OFFSET_FILL);

  // подписи поверх
  QPainter p(this);
  p.setRenderHint(QPainter::Antialiasing);
  const QColor fg(0x16, 0x18, 0x1b), muted(0x61, 0x64, 0x6b);
  {
    QFont f = font();
    scale_font(f, 0.95);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(0x8f, 0x93, 0x8a));
    if (const auto at = project({4, -4, 0})) p.drawText(*at + QPointF(0, 16), QString::fromStdString(printer_.name));
  }
  // положение сопла на верхнем слое
  if (mode_ == Mode::Preview && preview_ && upto_ && *upto_ < preview_->moves.size()) {
    const auto& mv = preview_->moves[*upto_];
    if (const auto at = project({mv.x1, mv.y1, mv.z})) {
      p.setPen(Qt::NoPen);
      p.setBrush(QColor(0x16, 0x18, 0x1b, 235));
      p.drawEllipse(*at, 7, 7);
      p.setBrush(QColor(0xEB, 0x68, 0x34));
      p.drawEllipse(*at, 3.5, 3.5);
    }
  }
  bool empty = mode_ == Mode::Prepare ? !mesh_ : (!preview_ || preview_->empty());
  if (empty && !empty_text_.isEmpty()) {
    QFont f = font();
    scale_font(f, 1.1);
    p.setFont(f);
    const QRect box = rect().adjusted(width() / 6, height() / 3, -width() / 6, -height() / 3);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 215));
    const QRect tr = p.fontMetrics().boundingRect(box, Qt::AlignCenter | Qt::TextWordWrap, empty_text_).adjusted(-16, -12, 16, 12);
    p.drawRoundedRect(tr, 8, 8);
    p.setPen(muted);
    p.drawText(box, Qt::AlignCenter | Qt::TextWordWrap, empty_text_);
  }
  if (!info_.isEmpty()) {
    QFont f = font();
    scale_font(f, 0.92);
    p.setFont(f);
    const QFontMetrics fm(f);
    int w = 0;
    for (const auto& s : info_) w = std::max(w, fm.horizontalAdvance(s));
    const QRect box(12, 12, w + 20, static_cast<int>(info_.size()) * fm.height() + 14);
    p.setPen(QColor(0xdf, 0xe0, 0xda));
    p.setBrush(QColor(255, 255, 255, 225));
    p.drawRoundedRect(box, 6, 6);
    p.setPen(fg);
    int y = box.top() + 7 + fm.ascent();
    for (const auto& s : info_) {
      p.drawText(box.left() + 10, y, s);
      y += fm.height();
    }
  }
  {
    QFont f = font();
    scale_font(f, 0.8);
    p.setFont(f);
    p.setPen(muted);
    const QString hint = mode_ == Mode::Prepare
                             ? (face_pick_ ? "щёлкните по грани, которой деталь ляжет на стол"
                                           : "деталь — перетащить мышью; вращение — мышь по пустому месту, сдвиг — правая кнопка")
                             : "вращение — мышь, сдвиг — правая кнопка или Shift, масштаб — колесо";
    const QFontMetrics fm(f);
    p.drawText(width() - 12 - fm.horizontalAdvance(hint), height() - 12 - fm.descent(), hint);
  }
}

void PlateView::mousePressEvent(QMouseEvent* e) {
  last_ = press_ = e->position();
  moved_ = 0;
  const bool pan = e->button() == Qt::RightButton || e->button() == Qt::MiddleButton || (e->modifiers() & Qt::ShiftModifier);
  drag_ = pan ? Drag::Pan : Drag::Rotate;
  if (!pan && e->button() == Qt::LeftButton && mode_ == Mode::Prepare && !face_pick_) {
    if (const auto hit = pick_model(e->position())) {
      drag_ = Drag::Part;
      drag_origin_ = hit->first;
      drag_dx_ = drag_dy_ = 0;
    }
  }
}

void PlateView::mouseMoveEvent(QMouseEvent* e) {
  const QPointF pos = e->position();
  const double dx = pos.x() - last_.x(), dy = pos.y() - last_.y();
  last_ = pos;
  if (drag_ == Drag::None) return;
  moved_ += std::abs(dx) + std::abs(dy);
  if (moved_ < 4) return;
  if (drag_ == Drag::Rotate) {
    theta_ -= dx * 0.008;
    phi_ = std::clamp(phi_ - dy * 0.008, 0.02, std::numbers::pi / 2 + 0.6);
  } else if (drag_ == Drag::Pan) {
    const double k = 2 * radius_ * std::tan(fov_ / 2 * std::numbers::pi / 180) / std::max(1, height());
    const QMatrix4x4 inv = view_matrix().inverted();
    const QVector3D right = inv.column(0).toVector3D(), upv = inv.column(1).toVector3D();
    target_ += right * static_cast<float>(-dx * k) + upv * static_cast<float>(dy * k);
  } else if (drag_ == Drag::Part) {
    // точка под мышью на горизонтальной плоскости, где взяли деталь
    const auto [o, d] = ray_at(pos);
    if (std::abs(d[2]) > 1e-6) {
      const double t = (drag_origin_[2] - o[2]) / d[2];
      if (t > 0) {
        drag_dx_ = o[0] + t * d[0] - drag_origin_[0];
        drag_dy_ = o[1] + t * d[1] - drag_origin_[1];
        if (on_drag) on_drag(drag_dx_, drag_dy_, false);
      }
    }
  }
  update();
}

void PlateView::mouseReleaseEvent(QMouseEvent* e) {
  const Drag d = drag_;
  drag_ = Drag::None;
  if (d == Drag::Part && moved_ >= 4) {
    const double dx = drag_dx_, dy = drag_dy_;
    if (on_drag) on_drag(dx, dy, true);
    return;
  }
  drag_dx_ = drag_dy_ = 0;
  if (moved_ < 4 && e->button() == Qt::LeftButton && face_pick_ && on_face)
    if (const auto hit = pick_model(e->position())) on_face(hit->second);
  update();
}

void PlateView::wheelEvent(QWheelEvent* e) {
  radius_ = std::clamp(radius_ * std::exp(-e->angleDelta().y() * 0.001), 5.0, 8000.0);
  update();
  e->accept();
}

}  // namespace kika::gui
