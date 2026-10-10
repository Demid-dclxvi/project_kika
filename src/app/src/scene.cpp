// Перенос логики просмотрщика прототипа (fdmfea/web/viewer.js, app.js).

#include "kika/app/scene.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "kika/fem/element.hpp"
#include "kika/material/material.hpp"

namespace kika::app {

namespace {

constexpr int kDirs[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
constexpr int kOpposite[6] = {1, 0, 3, 2, 5, 4};

const std::array<Rgb, 7> kBlueRamp = {rgb(0xCDE2FB), rgb(0x9EC5F4), rgb(0x6DA7EC), rgb(0x3987E5),
                                      rgb(0x256ABF), rgb(0x184F95), rgb(0x0D366B)};
constexpr Rgb kCatBlue = rgb(0x2A78D6);
constexpr Rgb kCatOrange = rgb(0xEB6834);
constexpr Rgb kCatAqua = rgb(0x1BAF7A);
constexpr Rgb kCritical = rgb(0xD03B3B);
constexpr Rgb kSerious = rgb(0xEC835A);
constexpr Rgb kWarning = rgb(0xFAB219);

// вид разрушения → группа: вдоль нити, между нитями в слое, между слоями
constexpr int kModeGroup[8] = {0, 0, 1, 1, 2, 2, 2, 1};
const std::array<std::pair<const char*, Rgb>, 3> kModeGroups = {
    std::pair{"Вдоль нити", kCatBlue}, std::pair{"Между нитями в слое", kCatAqua},
    std::pair{"Между слоями", kCatOrange}};
const std::array<std::pair<const char*, Rgb>, 3> kRoleGroups = {
    std::pair{"Стенки", kCatBlue}, std::pair{"Сплошное заполнение", kCatOrange},
    std::pair{"Разреженное заполнение", kCatAqua}};

Rgb lerp(const Rgb& a, const Rgb& b, float t) {
  return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
}

Rgb ramp(double t) {
  t = std::clamp(t, 0.0, 1.0);
  const double k = t * static_cast<double>(kBlueRamp.size() - 1);
  const std::size_t i = std::min(kBlueRamp.size() - 2, static_cast<std::size_t>(std::floor(k)));
  return lerp(kBlueRamp[i], kBlueRamp[i + 1], static_cast<float>(k - static_cast<double>(i)));
}

struct SfStep {
  double hi;
  Rgb color;
  std::string label;
};

std::vector<SfStep> sf_steps(double t) {
  std::vector<SfStep> s;
  s.push_back({1.0, kCritical, "< 1"});
  if (t > 1.5) {
    s.push_back({1.5, kSerious, "1–1,5"});
    s.push_back({t, kWarning, "1,5–" + fmt(t, 1)});
  } else {
    s.push_back({t, kSerious, "1–" + fmt(t, 1)});
  }
  s.push_back({2 * t, rgb(0xB7D3F6), fmt(t, 1) + "–" + fmt(2 * t, 1)});
  s.push_back({4 * t, rgb(0x6DA7EC), fmt(2 * t, 1) + "–" + fmt(4 * t, 1)});
  s.push_back({std::numeric_limits<double>::infinity(), rgb(0x256ABF), "> " + fmt(4 * t, 1)});
  return s;
}

std::vector<std::int64_t> sorted_unique(std::vector<std::int64_t> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

// Пересечение луча с треугольником (Мёллер — Трумбор), обе стороны. t < 0 — нет пересечения.
double ray_triangle(const Vec3& o, const Vec3& d, const Vec3& a, const Vec3& b, const Vec3& c) {
  const Vec3 e1{b[0] - a[0], b[1] - a[1], b[2] - a[2]};
  const Vec3 e2{c[0] - a[0], c[1] - a[1], c[2] - a[2]};
  const Vec3 p{d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
  const double det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
  if (std::abs(det) < 1e-14) return -1;
  const double inv = 1.0 / det;
  const Vec3 s{o[0] - a[0], o[1] - a[1], o[2] - a[2]};
  const double u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
  if (u < 0 || u > 1) return -1;
  const Vec3 q{s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
  const double v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) * inv;
  if (v < 0 || u + v > 1) return -1;
  return (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * inv;
}

}  // namespace

std::string fmt(double v, int d) {
  if (!std::isfinite(v)) return "—";
  if (d < 0) {
    const double a = std::abs(v);
    d = a >= 100 ? 0 : (a >= 10 ? 1 : (a >= 1 ? 2 : 3));
  }
  std::string s = std::format("{:.{}f}", v, d);
  for (char& c : s)
    if (c == '.') c = ',';
  // «−0,0» показываем как «0,0»
  if (s.size() > 1 && s[0] == '-' && s.find_first_not_of("0,", 1) == std::string::npos) s.erase(0, 1);
  return s;
}

Scene::Scene(const analysis::Model& model) : model_(&model) {
  const auto& vm = model.vm;
  const auto& mesh = model.mesh;
  const std::size_t n = vm.size();
  ix_ = vm.ix;
  iy_ = vm.iy;
  iz_ = vm.iz;
  flat_ = mesh.elem_flat;
  nbr_.resize(n * 6);
  center_.resize(n);
  role_.resize(n);
  rho_.resize(n);
  const Vec3 o = model.origin;
  for (std::size_t e = 0; e < n; ++e) {
    for (std::size_t d = 0; d < 6; ++d) nbr_[e * 6 + d] = mesh.nbr[e][d];
    const auto c = vm.center(e);
    center_[e] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    switch (vm.role[e]) {
      case gcode::Role::OuterWall:
      case gcode::Role::InnerWall:
        role_[e] = 0;
        break;
      case gcode::Role::Solid:
        role_[e] = 1;
        break;
      default:
        role_[e] = 2;
    }
    rho_[e] = vm.rho(e);
  }
  npos_.resize(mesh.xyz.size());
  for (std::size_t i = 0; i < npos_.size(); ++i)
    npos_[i] = {mesh.xyz[i][0] - o[0], mesh.xyz[i][1] - o[1], mesh.xyz[i][2] - o[2]};
  set_section(std::nullopt);
}

std::optional<std::pair<std::int32_t, int>> Scene::face_of(std::int64_t key) const {
  if (key < 0) return std::nullopt;
  const std::int64_t fl = key / 6;
  const int d = static_cast<int>(key % 6);
  const auto it = std::lower_bound(flat_.begin(), flat_.end(), fl);
  if (it == flat_.end() || *it != fl) return std::nullopt;
  return std::pair{static_cast<std::int32_t>(it - flat_.begin()), d};
}

void Scene::set_section(std::optional<std::pair<int, double>> section) {
  const std::size_t n = n_elems();
  visible_.assign(n, 1);
  if (section) {
    const auto a = static_cast<std::size_t>(section->first);
    for (std::size_t e = 0; e < n; ++e) visible_[e] = center_[e][a] <= section->second ? 1 : 0;
  }
  faces_.clear();
  for (std::size_t e = 0; e < n; ++e) {
    if (!visible_[e]) continue;
    for (int d = 0; d < 6; ++d) {
      const std::int32_t b = nbr_[e * 6 + static_cast<std::size_t>(d)];
      if (b < 0 || !visible_[static_cast<std::size_t>(b)])
        faces_.push_back({static_cast<std::int32_t>(e), static_cast<std::int8_t>(d), b >= 0});
    }
  }
}

void Scene::set_results(const analysis::AnalysisResult* results) {
  results_ = results;
  case_ = 0;
  vm_max_.clear();
  if (results) target_ = results->target_sf;
}

void Scene::set_case(std::size_t c) {
  if (results_ && c < results_->cases.size()) case_ = c;
}

const analysis::CaseResult* Scene::current_case() const {
  if (!results_ || case_ >= results_->cases.size()) return nullptr;
  return &results_->cases[case_];
}

double Scene::stress_max() const {
  const auto* r = current_case();
  if (!r) return 1.0;
  if (vm_max_.size() != results_->cases.size()) vm_max_.assign(results_->cases.size(), -1.0);
  double& m = vm_max_[case_];
  if (m < 0) {
    std::vector<double> a;
    a.reserve(r->von_mises.size());
    for (double v : r->von_mises)
      if (std::isfinite(v)) a.push_back(v);
    if (a.empty()) {
      m = 1.0;
    } else {
      const auto k = std::min(a.size() - 1, static_cast<std::size_t>(std::floor(0.995 * static_cast<double>(a.size() - 1))));
      std::nth_element(a.begin(), a.begin() + static_cast<std::ptrdiff_t>(k), a.end());
      m = a[k] > 0 ? a[k] : 1.0;
    }
  }
  return m;
}

Rgb Scene::elem_color(std::size_t e) const {
  const auto* r = current_case();
  switch (field_) {
    case Field::SafetyFactor:
      if (r) {
        const double v = r->sf[e];
        for (const auto& s : sf_steps(target_))
          if (v < s.hi) return s.color;
        return rgb(0x256ABF);
      }
      break;
    case Field::Stress:
      if (r) return ramp(r->von_mises[e] / stress_max());
      break;
    case Field::Displacement:
      if (r) {
        const double mx = r->summary.max_disp > 0 ? r->summary.max_disp : 1.0;
        double s = 0;
        for (auto node : model_->mesh.elem_nodes[e]) {
          const auto i = static_cast<std::size_t>(node) * 3;
          s += std::sqrt(r->u[i] * r->u[i] + r->u[i + 1] * r->u[i + 1] + r->u[i + 2] * r->u[i + 2]);
        }
        return ramp(s / 8 / mx);
      }
      break;
    case Field::Mode:
      if (r) {
        if (r->sf[e] < 2 * target_) return kModeGroups[static_cast<std::size_t>(kModeGroup[r->mode[e] & 7])].second;
        return palette::kNeutral;
      }
      break;
    case Field::Structure:
      return kRoleGroups[role_[e]].second;
    case Field::Density:
      return ramp(rho_[e]);
    case Field::Model:
      break;
  }
  return palette::kPart;
}

std::vector<Rgb> Scene::face_colors(const std::vector<Overlay>& overlays) const {
  std::vector<Rgb> out(faces_.size());
  constexpr float cut_tint = 0.9f;
  for (std::size_t f = 0; f < faces_.size(); ++f) {
    const auto& vf = faces_[f];
    Rgb c = elem_color(static_cast<std::size_t>(vf.elem));
    if (vf.cut) {
      c = {c[0] * cut_tint, c[1] * cut_tint, c[2] * cut_tint};
    } else if (!overlays.empty()) {
      const std::int64_t k = key(static_cast<std::size_t>(vf.elem), vf.dir);
      for (auto it = overlays.rbegin(); it != overlays.rend(); ++it)
        if (std::binary_search(it->keys.begin(), it->keys.end(), k)) {
          c = lerp(c, it->color, it->alpha);
          break;
        }
    }
    out[f] = c;
  }
  return out;
}

Legend Scene::legend() const {
  Legend lg;
  const auto* r = current_case();
  switch (field_) {
    case Field::SafetyFactor:
      if (!r) break;
      lg.kind = Legend::Kind::Steps;
      lg.title = "Запас прочности";
      for (const auto& s : sf_steps(target_)) lg.entries.push_back({s.color, s.label});
      break;
    case Field::Stress:
      if (!r) break;
      lg.kind = Legend::Kind::Ramp;
      lg.title = "Напряжение в материале, МПа";
      lg.max = stress_max();
      break;
    case Field::Displacement:
      if (!r) break;
      lg.kind = Legend::Kind::Ramp;
      lg.title = "Перемещение, мм";
      lg.max = r->summary.max_disp > 0 ? r->summary.max_disp : 1.0;
      break;
    case Field::Mode:
      if (!r) break;
      lg.kind = Legend::Kind::Categories;
      lg.title = "Что разрушится первым";
      for (const auto& [name, color] : kModeGroups) lg.entries.push_back({color, name});
      lg.entries.push_back({palette::kNeutral, "Запас больше " + fmt(2 * target_, 1)});
      break;
    case Field::Structure:
      lg.kind = Legend::Kind::Categories;
      lg.title = "Структура печати";
      for (const auto& [name, color] : kRoleGroups) lg.entries.push_back({color, name});
      break;
    case Field::Density:
      lg.kind = Legend::Kind::Ramp;
      lg.title = "Заполненность вокселя";
      lg.max = 100;
      lg.unit = "%";
      break;
    case Field::Model:
      break;
  }
  if (lg.kind == Legend::Kind::Ramp) lg.ramp.assign(kBlueRamp.begin(), kBlueRamp.end());
  return lg;
}

Vec3 Scene::node_position(std::size_t node, double deform) const {
  Vec3 p = npos_[node];
  const auto* r = current_case();
  if (deform != 0.0 && r)
    for (std::size_t a = 0; a < 3; ++a) p[a] += deform * r->u[3 * node + a];
  return p;
}

void Scene::face_corners(std::size_t f, double deform, std::array<Vec3, 4>& out) const {
  const auto& vf = faces_[f];
  const auto& en = model_->mesh.elem_nodes[static_cast<std::size_t>(vf.elem)];
  for (std::size_t q = 0; q < 4; ++q)
    out[q] = node_position(static_cast<std::size_t>(en[static_cast<std::size_t>(fem::kFaceNodes[vf.dir][q])]), deform);
}

double Scene::auto_deform_scale() const {
  const auto* r = current_case();
  if (!r) return 0.0;
  const double md = r->summary.max_disp;
  const Vec3 s = size();
  const double l = std::max({s[0], s[1], s[2]});
  return md > 0 ? 0.08 * l / md : 0.0;
}

std::pair<std::int32_t, int> Scene::step_face(std::int32_t e, int d, int t) const {
  const std::int32_t e1 = nbr_[static_cast<std::size_t>(e) * 6 + static_cast<std::size_t>(t)];
  if (e1 >= 0) {
    const std::int32_t e2 = nbr_[static_cast<std::size_t>(e1) * 6 + static_cast<std::size_t>(d)];
    if (e2 >= 0) return {e2, kOpposite[t]};  // вогнутый угол
    return {e1, d};                           // плоско
  }
  return {e, t};  // выпуклый угол
}

std::vector<std::int64_t> Scene::flood(std::int32_t e0, int d0, bool plane, int axis) const {
  std::vector<std::int64_t> out;
  std::vector<char> seen(n_elems() * 6, 0);
  std::vector<std::pair<std::int32_t, int>> stack{{e0, d0}};
  constexpr std::size_t kLimit = 400000;
  const int a0 = d0 >> 1;
  while (!stack.empty() && out.size() < kLimit) {
    const auto [e, d] = stack.back();
    stack.pop_back();
    const std::size_t s = static_cast<std::size_t>(e) * 6 + static_cast<std::size_t>(d);
    if (seen[s]) continue;
    seen[s] = 1;
    if (!boundary(static_cast<std::size_t>(e), d)) continue;
    if (plane && d != d0) continue;
    if (!plane && (d >> 1) == axis) continue;
    out.push_back(key(static_cast<std::size_t>(e), d));
    const int a = d >> 1;
    for (int t = 0; t < 6; ++t) {
      if ((t >> 1) == a) continue;
      std::int32_t e2 = -1;
      int d2 = d;
      if (plane) {
        e2 = nbr_[static_cast<std::size_t>(e) * 6 + static_cast<std::size_t>(t)];
        if (e2 < 0) continue;
        // копланарность: та же плоскость (одинаковый индекс по оси нормали)
        const auto ue2 = static_cast<std::size_t>(e2), ue0 = static_cast<std::size_t>(e0);
        const bool same = a0 == 0 ? ix_[ue2] == ix_[ue0] : (a0 == 1 ? iy_[ue2] == iy_[ue0] : iz_[ue2] == iz_[ue0]);
        if (!same) continue;
      } else {
        std::tie(e2, d2) = step_face(e, d, t);
      }
      if (!seen[static_cast<std::size_t>(e2) * 6 + static_cast<std::size_t>(d2)]) stack.emplace_back(e2, d2);
    }
  }
  return sorted_unique(std::move(out));
}

std::vector<std::int64_t> Scene::flood_plane(std::int32_t e, int d) const { return flood(e, d, true, 0); }

std::vector<std::int64_t> Scene::flood_axis(std::int32_t e, int d, int axis) const { return flood(e, d, false, axis); }

std::vector<std::int64_t> Scene::hole_region(std::int32_t e, int d) const {
  const int a0 = d >> 1;
  std::vector<std::int64_t> best;
  double best_area = 0;
  bool found = false;
  for (int a = 0; a < 3; ++a) {
    if (a == a0) continue;
    auto r = flood_axis(e, d, a);
    const double ar = area(r);
    if (r.size() >= 4 && (!found || ar < best_area)) {
      best = std::move(r);
      best_area = ar;
      found = true;
    }
  }
  return found ? best : flood_plane(e, d);
}

std::vector<std::int64_t> Scene::brush(const Vec3& point, double radius) const {
  std::vector<std::int64_t> out;
  const double r2 = radius * radius;
  const auto& vm = model_->vm;
  for (std::size_t e = 0; e < n_elems(); ++e) {
    const double dx = center_[e][0] - point[0], dy = center_[e][1] - point[1], dz = center_[e][2] - point[2];
    if (dx * dx + dy * dy + dz * dz > r2 * 1.5 + 4) continue;
    const double h = vm.dz(e);
    for (int d = 0; d < 6; ++d) {
      if (!boundary(e, d)) continue;
      const double fx = dx + kDirs[d][0] * vm.sx / 2, fy = dy + kDirs[d][1] * vm.sy / 2, fz = dz + kDirs[d][2] * h / 2;
      if (fx * fx + fy * fy + fz * fz <= r2) out.push_back(key(e, d));
    }
  }
  return out;  // уже по возрастанию: элементы идут по возрастанию плоского номера
}

std::vector<std::int64_t> Scene::side(int dir) const {
  const auto a = static_cast<std::size_t>(dir >> 1);
  const double sg = (dir & 1) ? 1.0 : -1.0;
  double ext = -std::numeric_limits<double>::infinity();
  for (std::size_t e = 0; e < n_elems(); ++e)
    if (boundary(e, dir)) ext = std::max(ext, sg * center_[e][a]);
  const auto& vm = model_->vm;
  const double tol = 0.6 * (a == 0 ? vm.sx : (a == 1 ? vm.sy : 1.0));
  std::vector<std::int64_t> out;
  for (std::size_t e = 0; e < n_elems(); ++e)
    if (boundary(e, dir) && sg * center_[e][a] >= ext - tol) out.push_back(key(e, dir));
  return out;
}

double Scene::area(const std::vector<std::int64_t>& keys) const {
  const auto& vm = model_->vm;
  const std::int64_t nxy = static_cast<std::int64_t>(vm.nx) * vm.ny;
  double a = 0;
  for (auto k : keys) {
    const std::int64_t fl = k / 6;
    const auto d = k % 6;
    const auto iz = static_cast<std::size_t>(fl / nxy);
    if (iz + 1 >= vm.z_edges.size()) continue;
    const double dz = vm.z_edges[iz + 1] - vm.z_edges[iz];
    a += d < 2 ? vm.sy * dz : (d < 4 ? vm.sx * dz : vm.sx * vm.sy);
  }
  return a;
}

std::optional<Vec3> Scene::center(const std::vector<std::int64_t>& keys, double deform) const {
  Vec3 s{0, 0, 0};
  std::size_t cnt = 0;
  for (auto k : keys) {
    const auto f = face_of(k);
    if (!f) continue;
    const auto& en = model_->mesh.elem_nodes[static_cast<std::size_t>(f->first)];
    for (std::size_t q = 0; q < 4; ++q) {
      const Vec3 p =
          node_position(static_cast<std::size_t>(en[static_cast<std::size_t>(fem::kFaceNodes[f->second][q])]), deform);
      for (std::size_t a = 0; a < 3; ++a) s[a] += p[a];
      ++cnt;
    }
  }
  if (!cnt) return std::nullopt;
  for (auto& v : s) v /= static_cast<double>(cnt);
  return s;
}

Vec3 Scene::normal_sum(const std::vector<std::int64_t>& keys) const {
  Vec3 n{0, 0, 0};
  for (auto k : keys) {
    const auto d = static_cast<std::size_t>(k % 6);
    for (std::size_t a = 0; a < 3; ++a) n[a] += kDirs[d][a];
  }
  return n;
}

std::vector<FacePoint> Scene::to_points(const std::vector<std::int64_t>& keys) const {
  const auto& vm = model_->vm;
  const Vec3 o = model_->origin;
  std::vector<FacePoint> out;
  out.reserve(keys.size());
  for (auto k : keys) {
    const std::int64_t fl = k / 6;
    const int d = static_cast<int>(k % 6);
    const std::int64_t ix = fl % vm.nx, iy = (fl / vm.nx) % vm.ny, iz = fl / (static_cast<std::int64_t>(vm.nx) * vm.ny);
    if (iz + 1 >= static_cast<std::int64_t>(vm.z_edges.size())) continue;
    const double z0 = vm.z_edges[static_cast<std::size_t>(iz)], z1 = vm.z_edges[static_cast<std::size_t>(iz + 1)];
    out.push_back({{vm.x0 + (static_cast<double>(ix) + 0.5 + kDirs[d][0] / 2.0) * vm.sx - o[0],
                    vm.y0 + (static_cast<double>(iy) + 0.5 + kDirs[d][1] / 2.0) * vm.sy - o[1],
                    0.5 * (z0 + z1) + kDirs[d][2] * (z1 - z0) / 2 - o[2]},
                   d});
  }
  return out;
}

std::vector<std::int64_t> Scene::from_points(const std::vector<FacePoint>& points) const {
  const auto& vm = model_->vm;
  const Vec3 o = model_->origin;
  const auto& ze = vm.z_edges;
  auto z_index = [&](double z) {
    std::size_t lo = 0, hi = ze.size() - 2;
    while (lo < hi) {
      const std::size_t m = (lo + hi + 1) / 2;
      if (ze[m] <= z)
        lo = m;
      else
        hi = m - 1;
    }
    return static_cast<std::int64_t>(lo);
  };
  std::vector<std::int64_t> out;
  constexpr double eps = 1e-3;
  for (const auto& pt : points) {
    const int d = pt.dir;
    const auto a = static_cast<std::size_t>(d >> 1);
    const double sg = (d & 1) ? 1.0 : -1.0;
    // точка на грани; чуть-чуть сдвигаем внутрь детали (против нормали)
    Vec3 q{pt.p[0] + o[0], pt.p[1] + o[1], pt.p[2] + o[2]};
    q[a] -= sg * eps;
    const std::array<std::int64_t, 3> base{static_cast<std::int64_t>(std::floor((q[0] - vm.x0) / vm.sx)),
                                           static_cast<std::int64_t>(std::floor((q[1] - vm.y0) / vm.sy)),
                                           z_index(q[2])};
    for (int t : {0, 1, -1, 2, -2}) {
      auto c = base;
      c[a] += t;
      if (c[0] < 0 || c[1] < 0 || c[2] < 0 || c[0] >= vm.nx || c[1] >= vm.ny || c[2] >= vm.nz) continue;
      const std::int64_t fl = (c[2] * vm.ny + c[1]) * vm.nx + c[0];
      const auto it = std::lower_bound(flat_.begin(), flat_.end(), fl);
      if (it == flat_.end() || *it != fl) continue;
      const auto e = static_cast<std::size_t>(it - flat_.begin());
      if (boundary(e, d)) {
        out.push_back(key(e, d));
        break;
      }
    }
  }
  return sorted_unique(std::move(out));
}

std::optional<Hit> Scene::pick(const Vec3& origin, const Vec3& dir, double deform) const {
  std::optional<Hit> best;
  std::array<Vec3, 4> c;
  for (std::size_t f = 0; f < faces_.size(); ++f) {
    face_corners(f, deform, c);
    double t = ray_triangle(origin, dir, c[0], c[1], c[2]);
    const double t2 = ray_triangle(origin, dir, c[0], c[2], c[3]);
    if (t2 > 0 && (t < 0 || t2 < t)) t = t2;
    if (t > 1e-9 && (!best || t < best->t)) {
      Hit h;
      h.face = f;
      h.elem = faces_[f].elem;
      h.dir = faces_[f].dir;
      h.cut = faces_[f].cut;
      h.key = key(static_cast<std::size_t>(h.elem), h.dir);
      h.point = {origin[0] + t * dir[0], origin[1] + t * dir[1], origin[2] + t * dir[2]};
      h.t = t;
      best = h;
    }
  }
  return best;
}

std::vector<std::string> Scene::describe(std::int32_t e) const {
  const auto ue = static_cast<std::size_t>(e);
  const Vec3& c = center_[ue];
  std::vector<std::string> lines;
  lines.push_back("x " + fmt(c[0], 1) + " · y " + fmt(c[1], 1) + " · z " + fmt(c[2], 1) + " мм");
  const auto* r = current_case();
  if (r && (field_ == Field::SafetyFactor || field_ == Field::Stress || field_ == Field::Displacement ||
            field_ == Field::Mode)) {
    lines.push_back("запас " + fmt(std::min(r->sf[ue], 999.0)) + " · " + fmt(r->von_mises[ue]) + " МПа");
    lines.push_back(std::string(material::failure_mode_name(static_cast<material::FailureMode>(r->mode[ue] & 7))));
  } else {
    lines.push_back(std::string(kRoleGroups[role_[ue]].first) + " · " +
                    std::to_string(static_cast<int>(std::lround(rho_[ue] * 100))) + "%");
  }
  return lines;
}

}  // namespace kika::app
