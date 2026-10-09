// Перенос fdmfea/analysis.py (Model, MaterialField, Analysis) и text_summary из fdmfea/export.py.

#include "kika/analysis/analysis.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <format>
#include <iterator>
#include <map>
#include <mutex>
#include <numbers>
#include <system_error>

#include "util/parallel.hpp"

namespace kika::analysis {

namespace {

using json::Value;
using linalg::Mat6;
using linalg::Vec6;

constexpr int kBins = voxel::kBins;
constexpr double kG = 9.81;  // Н на кг

double seconds_since(std::chrono::steady_clock::time_point t0) {
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
}

void report(const Progress& p, std::string_view stage, double frac, std::string_view text) {
  if (p) p(stage, frac, text);
}

std::string duration_name(const std::string& d) {
  if (d == "short") return "кратковременная";
  if (d == "long") return "длительная (ползучесть)";
  if (d == "cyclic") return "циклическая (усталость)";
  return d;
}

double norm3(const Vec3& v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

Vec3 round3(const Vec3& v, int digits) {
  return {py_round(v[0], digits), py_round(v[1], digits), py_round(v[2], digits)};
}

Value vec_json(const Vec3& v) { return Value::array_of(v); }

// Число для текста отчёта: до 100 — три значащие цифры, дальше — целое с пробелами по разрядам.
std::string format_amount(double v) {
  if (v < 100) return std::format("{:.3g}", v);
  std::string s = std::format("{:.0f}", v);
  const std::size_t start = (!s.empty() && s[0] == '-') ? 1 : 0;
  for (std::size_t i = s.size(); i > start + 3; i -= 3) s.insert(i - 3, " ");
  return s;
}

std::string decimal_comma(std::string s) {
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

double von_mises(const Vec6& s) {
  return std::sqrt(0.5 * ((s[0] - s[1]) * (s[0] - s[1]) + (s[1] - s[2]) * (s[1] - s[2]) +
                          (s[2] - s[0]) * (s[2] - s[0])) +
                   3 * (s[3] * s[3] + s[4] * s[4] + s[5] * s[5]));
}

// ---------------------------------------------------------------------------
// Жёсткость каждого элемента и матрицы для пересчёта напряжений в оси валиков.
struct MaterialField {
  double kz = 0.6;
  std::array<Mat6, kBins> ms{}, m0{}, m1{};
  std::vector<double> hs;  // n × kBins (пустые гистограммы стенок заменены равномерными)
  const std::vector<double>* hp = nullptr;
  const std::vector<double>* rs = nullptr;
  const std::vector<double>* rp = nullptr;
  std::vector<fem::CVec> cvec;
  material::HoffmanCoeffs hc{};

  Mat6 c_full(std::size_t e) const { return fem::vec_to_c(cvec[e]); }
};

Mat6 diag6(const std::array<double, 6>& d) {
  Mat6 m;
  for (std::size_t i = 0; i < 6; ++i) m(i, i) = d[i];
  return m;
}

MaterialField make_field(const Model& model, const material::Material& m) {
  const auto& vm = model.vm;
  MaterialField mf;
  const Mat6 cd = material::stiffness(m);
  mf.kz = material::infill_kz(vm.infill_pattern);
  const Mat6 d0 = diag6({m.E1, 0.0, m.E3 * mf.kz, 0.0, m.G13 * mf.kz, 0.0});
  const Mat6 d1 = diag6({0.0, m.E2, 0.0, m.G23, 0.0, m.G12});
  std::array<fem::CVec, kBins> cr_d{}, cr_0{}, cr_1{};
  for (int b = 0; b < kBins; ++b) {
    const double th = b * std::numbers::pi / kBins;
    const Mat6 t = material::strain_to_local(th);
    cr_d[static_cast<std::size_t>(b)] = fem::c_to_vec(material::rotate_z(cd, th));
    cr_0[static_cast<std::size_t>(b)] = fem::c_to_vec(material::rotate_z(d0, th));
    cr_1[static_cast<std::size_t>(b)] = fem::c_to_vec(material::rotate_z(d1, th));
    mf.ms[static_cast<std::size_t>(b)] = cd * t;
    mf.m0[static_cast<std::size_t>(b)] = d0 * t;
    mf.m1[static_cast<std::size_t>(b)] = d1 * t;
  }
  const std::size_t n = vm.size();
  mf.hs = vm.hist_shell;
  for (std::size_t e = 0; e < n; ++e) {
    double sum = 0;
    for (int b = 0; b < kBins; ++b) sum += mf.hs[e * kBins + static_cast<std::size_t>(b)];
    if (sum <= 0 && vm.rho_shell[e] > 0)
      for (int b = 0; b < kBins; ++b) mf.hs[e * kBins + static_cast<std::size_t>(b)] = 1.0 / kBins;
  }
  mf.hp = &vm.hist_sparse;
  mf.rs = &vm.rho_shell;
  mf.rp = &vm.rho_sparse;
  // минимальная жёсткость, чтобы не было вырожденных элементов
  fem::CVec iso = fem::c_to_vec(cd);
  for (auto& v : iso) v *= 1e-4;
  mf.cvec.resize(n);
  util::parallel_for(0, n, 2048, [&](std::size_t lo, std::size_t hi) {
    for (std::size_t e = lo; e < hi; ++e) {
      const double rs = vm.rho_shell[e], rp = vm.rho_sparse[e];
      for (std::size_t k = 0; k < fem::kPairs; ++k) {
        double a = 0, b0 = 0, b1 = 0;
        for (std::size_t b = 0; b < kBins; ++b) {
          a += mf.hs[e * kBins + b] * cr_d[b][k];
          b0 += vm.hist_sparse[e * kBins + b] * cr_0[b][k];
          b1 += vm.hist_sparse[e * kBins + b] * cr_1[b][k];
        }
        mf.cvec[e][k] = rs * a + rp * b0 + (rp * rp) * b1 + iso[k];
      }
    }
  });
  mf.hc = material::hoffman_coeffs(m);
  return mf;
}

// ---------------------------------------------------------------------------
// Закрепления и нагрузки одного расчётного случая.
struct Impact {
  double kg = 0, height = 0;
  Vec3 dir{};
  std::vector<std::int64_t> faces;
};

struct Bc {
  std::vector<char> fixed;
  std::vector<double> uval;
  std::vector<double> f_force;
  std::vector<Vec3> body;
  std::vector<std::vector<std::int64_t>> bc_nodes;
  std::vector<std::int64_t> fix_nodes, disp_nodes;
  std::optional<Impact> impact;
  bool thermal = false;
  std::vector<LoadDesc> desc;
  std::vector<std::pair<std::string, std::vector<std::int64_t>>> sel_faces;
  std::vector<std::string> pre_warnings;
};

LoadDesc make_desc(const std::string& type, double magnitude, const std::string& unit) {
  LoadDesc d;
  d.type = type;
  d.magnitude = magnitude;
  d.unit = unit;
  d.json["type"] = type;
  d.json["magnitude"] = magnitude;
  d.json["unit"] = unit;
  return d;
}

Vec3 add3(const Vec3& a, const Vec3& b) { return {a[0] + b[0], a[1] + b[1], a[2] + b[2]}; }

Vec3 weighted_center(const fem::Mesh& mesh, const NodeWeights& nw) {
  Vec3 c{0, 0, 0};
  double ws = 0;
  for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
    const auto& x = mesh.xyz[static_cast<std::size_t>(nw.nodes[i])];
    for (std::size_t a = 0; a < 3; ++a) c[a] += x[a] * nw.w[i];
  }
  for (double w : nw.w) ws += w;
  return {c[0] / ws, c[1] / ws, c[2] / ws};
}

// сила с точкой приложения: та же сила в центре граней плюс момент
void add_point_moment(const fem::Mesh& mesh, const std::vector<std::int64_t>& faces, const Vec3& point,
                      const Vec3& origin, const Vec3& f, std::vector<double>& f_out) {
  const Vec3 p = add3(point, origin);
  const Vec3 c = weighted_center(mesh, face_node_weights(mesh, faces));
  const Vec3 r{p[0] - c[0], p[1] - c[1], p[2] - c[2]};
  const Vec3 m{r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]};
  distribute_moment(mesh, faces, m, f_out);
}

std::vector<std::int64_t> unique_sorted(std::vector<std::int64_t> v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
  return v;
}

Bc fixtures_and_loads(const Model& model, const Case& cs, const Vec3& origin) {
  const auto& mesh = model.mesh;
  const auto nn = static_cast<std::size_t>(mesh.n_nodes);
  Bc bc;
  bc.fixed.assign(3 * nn, 0);
  bc.uval.assign(3 * nn, 0.0);
  bc.f_force.assign(3 * nn, 0.0);
  bc.thermal = cs.thermal_expansion;
  std::vector<std::int64_t> fix_nodes, disp_nodes;
  for (std::size_t k = 0; k < cs.fixtures.size(); ++k) {
    const auto& fx = cs.fixtures[k];
    auto faces = select_faces(mesh, fx.where, origin, "Закрепление " + std::to_string(k + 1));
    const auto nw = face_node_weights(mesh, faces);
    for (auto n : nw.nodes)
      for (std::size_t c = 0; c < 3; ++c)
        if (fx.components[c]) bc.fixed[static_cast<std::size_t>(3 * n) + c] = 1;
    bc.bc_nodes.push_back(nw.nodes);
    fix_nodes.insert(fix_nodes.end(), nw.nodes.begin(), nw.nodes.end());
    bc.sel_faces.emplace_back("fix" + std::to_string(k), std::move(faces));
  }
  for (std::size_t k = 0; k < cs.loads.size(); ++k) {
    const Load& ld = cs.loads[k];
    if (ld.type == LoadType::Gravity) {
      bc.body.push_back(ld.g);
      LoadDesc d;
      d.type = ld.type_name;
      d.magnitude = norm3(ld.g);
      d.unit = "g";
      d.json["type"] = ld.type_name;
      d.json["g"] = vec_json(ld.g);
      d.json["magnitude"] = d.magnitude;
      d.json["unit"] = "g";
      bc.desc.push_back(std::move(d));
      continue;
    }
    auto faces = select_faces(mesh, ld.where, origin, ld.label);
    const auto nodes = face_node_weights(mesh, faces).nodes;
    bc.bc_nodes.push_back(nodes);
    switch (ld.type) {
      case LoadType::Force:
      case LoadType::Bearing: {
        const Vec3 f = ld.vector ? *ld.vector : [&] {
          const Vec3 dir = resolve_direction(ld.direction, &mesh, faces);
          return Vec3{ld.value * dir[0], ld.value * dir[1], ld.value * dir[2]};
        }();
        if (ld.type == LoadType::Force) {
          distribute_force(mesh, faces, f, bc.f_force);
          if (ld.point) add_point_moment(mesh, faces, *ld.point, origin, f, bc.f_force);
          auto d = make_desc(ld.type_name, norm3(f), "Н");
          d.json["vector"] = vec_json(f);
          bc.desc.push_back(std::move(d));
        } else {
          distribute_bearing(mesh, faces, f, bc.f_force);
          auto d = make_desc("bearing", norm3(f), "Н");
          d.json["vector"] = vec_json(f);
          bc.desc.push_back(std::move(d));
        }
        break;
      }
      case LoadType::Mass: {
        const Vec3 dir = resolve_direction(ld.direction, &mesh, faces);
        const double w = ld.kg * kG;
        const Vec3 f{w * dir[0], w * dir[1], w * dir[2]};
        distribute_force(mesh, faces, f, bc.f_force);
        if (ld.point) add_point_moment(mesh, faces, *ld.point, origin, f, bc.f_force);
        auto d = make_desc("mass", ld.kg, "кг");
        d.json["vector"] = vec_json(f);
        bc.desc.push_back(std::move(d));
        break;
      }
      case LoadType::Pressure: {
        distribute_pressure(mesh, faces, ld.value, bc.f_force);
        double area = 0;
        for (auto f : faces) area += mesh.face_area[static_cast<std::size_t>(f)];
        auto d = make_desc(ld.type_name, ld.value, "МПа");
        d.json["area"] = area;
        bc.desc.push_back(std::move(d));
        break;
      }
      case LoadType::Moment: {
        Vec3 m{0, 0, 0};
        if (ld.axis) {
          const Vec3 a = resolve_direction(*ld.axis);
          m = {a[0] * ld.value, a[1] * ld.value, a[2] * ld.value};
        } else if (ld.vector) {
          m = *ld.vector;
        }
        std::optional<Vec3> ctr;
        if (ld.center) ctr = add3(*ld.center, origin);
        distribute_moment(mesh, faces, m, bc.f_force, ctr);
        auto d = make_desc("moment", norm3(m), "Н·мм");
        d.json["vector"] = vec_json(m);
        bc.desc.push_back(std::move(d));
        break;
      }
      case LoadType::Impact: {
        const Vec3 dir = resolve_direction(ld.direction, &mesh, faces);
        const double w = ld.kg * kG;
        const Vec3 f{w * dir[0], w * dir[1], w * dir[2]};
        distribute_force(mesh, faces, f, bc.f_force);
        bc.impact = Impact{ld.kg, ld.height, dir, faces};
        auto d = make_desc("impact", ld.kg, "кг");
        d.json["height"] = ld.height;
        d.json["vector"] = vec_json(f);
        bc.desc.push_back(std::move(d));
        break;
      }
      case LoadType::Displacement: {
        Value vec = Value::array();
        Vec3 mag{0, 0, 0};
        for (std::size_t c = 0; c < 3; ++c) {
          if (ld.displacement[c]) {
            for (auto n : nodes) {
              bc.fixed[static_cast<std::size_t>(3 * n) + c] = 1;
              bc.uval[static_cast<std::size_t>(3 * n) + c] = *ld.displacement[c];
            }
            mag[c] = *ld.displacement[c];
            vec.push_back(*ld.displacement[c]);
          } else {
            vec.push_back(nullptr);
          }
        }
        disp_nodes.insert(disp_nodes.end(), nodes.begin(), nodes.end());
        auto d = make_desc("displacement", norm3(mag), "мм");
        d.json["vector"] = std::move(vec);
        bc.desc.push_back(std::move(d));
        break;
      }
      case LoadType::Gravity:
        break;
    }
    bc.sel_faces.emplace_back("load" + std::to_string(k), std::move(faces));
  }
  // проверка закреплений
  for (std::size_t c = 0; c < 3; ++c) {
    bool any = false;
    for (std::size_t i = c; i < bc.fixed.size() && !any; i += 3) any = bc.fixed[i] != 0;
    if (!any)
      bc.pre_warnings.push_back(std::string("Нет закрепления по оси ") + "XYZ"[c] +
                                ": деталь может «уплыть» — добавлены слабые пружины, проверьте результат.");
  }
  bc.fix_nodes = unique_sorted(std::move(fix_nodes));
  bc.disp_nodes = unique_sorted(std::move(disp_nodes));
  return bc;
}

// ---------------------------------------------------------------------------
// Напряжения, запас прочности и вид разрушения по элементам.
struct Post {
  std::vector<double> sf, vmis, sigma;
  std::vector<std::int8_t> mode;
  double eps_max = 0;
};

Post postprocess(const Model& model, const MaterialField& mf, const material::Material& m,
                 const std::vector<double>& u, const std::optional<Vec6>& eps_th,
                 const std::vector<fem::CVec>& cvec) {
  const auto& mesh = model.mesh;
  const std::size_t ne = static_cast<std::size_t>(mesh.n_elems);
  Post r;
  r.sf.assign(ne, 1e6);
  r.mode.assign(ne, 0);
  r.vmis.assign(ne, 0.0);
  r.sigma.assign(ne * 6, 0.0);
  constexpr double thr = 0.02;
  std::mutex mx;
  util::parallel_for(0, ne, 512, [&](std::size_t lo, std::size_t hi) {
    double eps_max = 0;
    std::array<double, 24> ue{};
    std::array<double, 48> ec{};
    std::array<double, 6> em{};
    for (std::size_t e = lo; e < hi; ++e) {
      const auto& basis = mesh.bases[static_cast<std::size_t>(mesh.elem_size_idx[e])];
      const auto dofs = fem::elem_dofs(mesh, e);
      for (std::size_t i = 0; i < 24; ++i) ue[i] = u[static_cast<std::size_t>(dofs[i])];
      basis.strains(cvec[e], ue.data(), ec.data(), em.data());
      if (eps_th) {
        for (std::size_t g = 0; g < 8; ++g)
          for (std::size_t i = 0; i < 6; ++i) ec[g * 6 + i] -= (*eps_th)[i];
        for (std::size_t i = 0; i < 6; ++i) em[i] -= (*eps_th)[i];
      }
      for (double v : ec) eps_max = std::max(eps_max, std::abs(v));
      const Mat6 c = fem::vec_to_c(cvec[e]);
      for (std::size_t i = 0; i < 6; ++i) {
        double s = 0;
        for (std::size_t j = 0; j < 6; ++j) s += c(i, j) * em[j];
        r.sigma[e * 6 + i] = s;
      }
      double best = 1e6;
      Vec6 best_sig{};
      // фаза «стенки / сплошное заполнение». Доли стенки меньше 15 % в вокселе — «перелив»
      // валика из соседнего вокселя, их не оцениваем.
      const double rs = (*mf.rs)[e];
      const double rp = (*mf.rp)[e];
      // Как в прототипе: замаскированные точки получают λ = 1e6, берётся первая точка
      // с наименьшим λ (точки Гаусса снаружи, направления внутри), и фаза меняет итог,
      // только если её минимум строго меньше. Поэтому замаскированные можно просто пропускать.
      auto scan = [&](auto&& stress, auto&& masked) {
        double v = 1e6;
        Vec6 vs{};
        for (std::size_t g = 0; g < 8; ++g) {
          const Vec6 eg{ec[g * 6], ec[g * 6 + 1], ec[g * 6 + 2], ec[g * 6 + 3], ec[g * 6 + 4], ec[g * 6 + 5]};
          for (std::size_t b = 0; b < kBins; ++b) {
            if (masked(b)) continue;
            const Vec6 s = stress(b, eg);
            const double lam = material::hoffman_sf(s, mf.hc);
            if (lam < v) {
              v = lam;
              vs = s;
            }
          }
        }
        if (v < best) {
          best = v;
          best_sig = vs;
        }
      };
      scan([&](std::size_t b, const Vec6& eg) { return mf.ms[b] * eg; },
           [&](std::size_t b) { return mf.hs[e * kBins + b] < thr || rs < 0.15; });
      if (rp >= 0.02) {
        scan(
            [&](std::size_t b, const Vec6& eg) {
              const Vec6 a = mf.m0[b] * eg;
              const Vec6 bb = mf.m1[b] * eg;
              return Vec6{a[0] + rp * bb[0], a[1] + rp * bb[1], a[2] + rp * bb[2],
                          a[3] + rp * bb[3], a[4] + rp * bb[4], a[5] + rp * bb[5]};
            },
            [&](std::size_t b) { return (*mf.hp)[e * kBins + b] < thr || rp < 0.02; });
      }
      r.sf[e] = best;
      r.mode[e] = static_cast<std::int8_t>(material::failure_mode(best_sig, m));
      r.vmis[e] = von_mises(best_sig);
    }
    std::lock_guard lk(mx);
    r.eps_max = std::max(r.eps_max, eps_max);
  });
  return r;
}

std::optional<Limit> limit_text(const std::vector<LoadDesc>& desc, double sf) {
  if (desc.size() != 1) return std::nullopt;
  const auto& d = desc[0];
  if (d.type == "impact") return std::nullopt;
  const double val = d.magnitude * sf;
  std::string nm = "Предельная перегрузка";
  if (d.type == "force") nm = "Предельная сила";
  if (d.type == "bearing") nm = "Предельная сила на отверстие";
  if (d.type == "mass") nm = "Предельный груз";
  if (d.type == "pressure") nm = "Предельное давление";
  if (d.type == "moment") nm = "Предельный момент";
  if (d.type == "displacement") nm = "Предельное перемещение";
  Limit l;
  l.short_text = decimal_comma("≈ " + format_amount(val) + " " + d.unit);
  l.text = nm + " " + l.short_text;
  l.label = nm;
  l.value = val;
  l.unit = d.unit;
  return l;
}

struct SolverCache {
  std::map<std::vector<char>, std::unique_ptr<fem::Solver>> solvers;
};

CaseResult run_case(const Model& model, const MaterialField& mf, const material::Material& m, const Job& job,
                    const Vec3& origin, const linalg::CsrMatrix& k, const std::vector<fem::CVec>& cvec_case,
                    const Case& cs, const Bc& bc, SolverCache& cache,
                    const std::function<void(double, std::string_view)>& prog) {
  const double target_sf = job.target_sf;
  const auto& mesh = model.mesh;
  const auto nn = static_cast<std::size_t>(mesh.n_nodes);
  const std::size_t ne = static_cast<std::size_t>(mesh.n_elems);
  std::vector<std::string> warnings = bc.pre_warnings;
  const auto tf = material::temperature_factor(m, cs.temperature);
  if (tf.warning) warnings.push_back(*tf.warning);
  double f_str = tf.factor;
  double m_case = tf.factor;
  std::optional<double> cycles;
  if (cs.duration == "long") {
    f_str *= m.creep_strength;
    m_case *= m.creep_modulus;
  } else if (cs.duration == "cyclic") {
    cycles = cs.cycles;
    f_str *= material::fatigue_factor(m, *cycles);
  }
  // массовые силы (вес / перегрузка)
  std::vector<double> f_body(3 * nn, 0.0);
  if (!bc.body.empty()) {
    const double rho_t = m.density * 1e-9;  // т/мм³
    std::vector<double> emass(ne);
    for (std::size_t e = 0; e < ne; ++e) emass[e] = ((*mf.rs)[e] + (*mf.rp)[e]) * mesh.elem_vol[e] * rho_t;
    for (const auto& gv : bc.body) {
      const Vec3 acc{gv[0] * kGravity, gv[1] * kGravity, gv[2] * kGravity};
      for (std::size_t a = 0; a < 8; ++a)
        for (std::size_t e = 0; e < ne; ++e) {
          const auto n = static_cast<std::size_t>(mesh.elem_nodes[e][a]);
          for (std::size_t c = 0; c < 3; ++c) f_body[3 * n + c] += emass[e] * acc[c] / 8.0;
        }
    }
  }
  // температурное расширение
  std::vector<double> f_th(3 * nn, 0.0);
  std::optional<Vec6> eps_th;
  if (bc.thermal && cs.temperature) {
    const double dt = *cs.temperature - material::kRefTemperature;
    const double e0 = m.cte * dt;
    eps_th = Vec6{e0, e0, e0, 0.0, 0.0, 0.0};
    for (std::size_t si = 0; si < mesh.bases.size(); ++si) {
      const auto& bint = mesh.bases[si].b_integral();
      for (std::size_t e = 0; e < ne; ++e) {
        if (static_cast<std::size_t>(mesh.elem_size_idx[e]) != si) continue;
        const Mat6 c = mf.c_full(e);
        const Vec6 sig = c * *eps_th;
        const auto dofs = fem::elem_dofs(mesh, e);
        for (std::size_t j = 0; j < 24; ++j) {
          double s = 0;
          for (std::size_t i = 0; i < 6; ++i) s += sig[i] * bint[i * 24 + j];
          f_th[static_cast<std::size_t>(dofs[j])] += s;
        }
      }
    }
  }
  std::vector<double> f_ext(3 * nn);
  for (std::size_t i = 0; i < f_ext.size(); ++i) f_ext[i] = bc.f_force[i] + f_body[i];

  auto& slot = cache.solvers[bc.fixed];
  if (!slot) {
    prog(0.05, "подготовка решателя");
    bool under = false;
    for (std::size_t c = 0; c < 3; ++c) {
      bool any = false;
      for (std::size_t i = c; i < bc.fixed.size() && !any; i += 3) any = bc.fixed[i] != 0;
      under = under || !any;
    }
    slot = std::make_unique<fem::Solver>(k, bc.fixed, mesh, job.solver_tol, under ? 1e-7 : 1e-12);
  }
  fem::Solver& solver = *slot;
  prog(0.35, "решение системы");
  const std::vector<double> kup = k.multiply(bc.uval);
  std::vector<double> rhs_free;
  rhs_free.reserve(solver.free_dofs().size());
  for (auto d : solver.free_dofs()) {
    const auto i = static_cast<std::size_t>(d);
    rhs_free.push_back((f_ext[i] / m_case + f_th[i]) - kup[i]);
  }
  fem::SolveInfo sinfo;
  const auto uf = solver.solve(rhs_free, &sinfo);
  std::vector<double> u = bc.uval;
  for (std::size_t i = 0; i < uf.size(); ++i) u[static_cast<std::size_t>(solver.free_dofs()[i])] = uf[i];

  double k_imp = 1.0;
  if (bc.impact) {
    const auto& imp = *bc.impact;
    const auto nw = face_node_weights(mesh, imp.faces);
    double num = 0, den = 0;
    for (std::size_t i = 0; i < nw.nodes.size(); ++i) {
      const auto n = static_cast<std::size_t>(nw.nodes[i]);
      num += (u[3 * n] * imp.dir[0] + u[3 * n + 1] * imp.dir[1] + u[3 * n + 2] * imp.dir[2]) * nw.w[i];
      den += nw.w[i];
    }
    double dst = num / den;
    if (dst <= 1e-9) {
      warnings.push_back("Удар: не удалось определить прогиб в точке удара.");
      dst = 1e-9;
    }
    k_imp = 1.0 + std::sqrt(1.0 + 2.0 * imp.height / dst);
    for (double& v : u) v *= k_imp;
  }
  prog(0.7, "напряжения и запас прочности");
  std::vector<double> u_static(u.size());
  for (std::size_t i = 0; i < u.size(); ++i) u_static[i] = u[i] / k_imp;
  Post post = postprocess(model, mf, m, u_static, eps_th, cvec_case);
  for (auto& v : post.sf) v = v * f_str / (m_case * k_imp);
  for (auto& v : post.vmis) v = v * m_case * k_imp;
  for (auto& v : post.sigma) v = v * m_case * k_imp;

  // реакции
  const std::vector<double> ku = k.multiply(u_static);
  std::vector<double> r(3 * nn);
  for (std::size_t i = 0; i < r.size(); ++i) r[i] = m_case * (ku[i] - f_th[i]) - f_ext[i];
  std::vector<std::int64_t> fnodes;
  std::set_difference(bc.fix_nodes.begin(), bc.fix_nodes.end(), bc.disp_nodes.begin(), bc.disp_nodes.end(),
                      std::back_inserter(fnodes));
  Vec3 reaction{0, 0, 0};
  for (auto n : fnodes)
    for (std::size_t c = 0; c < 3; ++c) reaction[c] += r[static_cast<std::size_t>(3 * n) + c];
  for (auto& v : reaction) v *= k_imp;
  std::optional<Vec3> disp_force;
  if (!bc.disp_nodes.empty()) {
    Vec3 df{0, 0, 0};
    for (auto n : bc.disp_nodes)
      for (std::size_t c = 0; c < 3; ++c) df[c] += r[static_cast<std::size_t>(3 * n) + c];
    disp_force = df;
  }
  Vec3 applied{0, 0, 0};
  for (std::size_t n = 0; n < nn; ++n)
    for (std::size_t c = 0; c < 3; ++c) applied[c] += f_ext[3 * n + c];
  for (std::size_t c = 0; c < 3; ++c) applied[c] = applied[c] * k_imp + (disp_force ? (*disp_force)[c] : 0.0);

  // элементы у мест приложения нагрузок и закреплений
  std::vector<char> bcn(nn, 0);
  for (const auto& nds : bc.bc_nodes)
    for (auto n : nds) bcn[static_cast<std::size_t>(n)] = 1;
  std::vector<char> bc_elem(ne, 0);
  bool any_core = false;
  for (std::size_t e = 0; e < ne; ++e) {
    for (auto n : mesh.elem_nodes[e])
      if (bcn[static_cast<std::size_t>(n)]) {
        bc_elem[e] = 1;
        break;
      }
    any_core = any_core || !bc_elem[e];
  }
  std::size_t i_min = 0, i_core = ne;
  for (std::size_t e = 0; e < ne; ++e) {
    if (post.sf[e] < post.sf[i_min]) i_min = e;
    const bool core = any_core ? !bc_elem[e] : true;
    if (core && (i_core == ne || post.sf[e] < post.sf[i_core])) i_core = e;
  }
  std::size_t i_d = 0;
  double dmax = -1;
  for (std::size_t n = 0; n < nn; ++n) {
    const double d = std::sqrt(u[3 * n] * u[3 * n] + u[3 * n + 1] * u[3 * n + 1] + u[3 * n + 2] * u[3 * n + 2]);
    if (d > dmax) {
      dmax = d;
      i_d = n;
    }
  }
  const double sf_core = post.sf[i_core];
  const std::string verdict = sf_core >= target_sf ? "ok" : (sf_core >= 1.0 ? "risk" : "fail");
  if (dmax > 0.05 * norm3(model.size))
    warnings.push_back(
        "Перемещения больше 5% размера детали — линейный расчёт неточен (реальная деталь будет вести себя "
        "нелинейно).");
  if (post.eps_max * m_case * k_imp > 0.5 * m.elongation && m.E1 > 100)
    warnings.push_back("Деформации близки к удлинению при разрыве материала — возможно хрупкое разрушение.");
  const Vec3 imbalance = add3(reaction, applied);
  if (norm3(imbalance) > std::max(0.02 * norm3(applied), 1e-3) && !bc.thermal)
    warnings.push_back("Реакции опор не уравновешивают нагрузку — модель закреплена недостаточно.");
  std::optional<Limit> limit;
  if (!bc.thermal) limit = limit_text(bc.desc, sf_core);
  if (bc.impact) {
    // допустимая высота падения: k_доп = запас при статической нагрузке весом груза
    const double k_allow = sf_core * k_imp;
    const auto nodes = face_node_weights(mesh, bc.impact->faces).nodes;
    double s = 0;
    for (auto n0 : nodes) {
      const auto n = static_cast<std::size_t>(n0);
      s += u[3 * n] * bc.impact->dir[0] + u[3 * n + 1] * bc.impact->dir[1] + u[3 * n + 2] * bc.impact->dir[2];
    }
    const double dst = s / static_cast<double>(nodes.size()) / k_imp;
    const double h_allow = k_allow > 2.0 ? std::max(0.0, ((k_allow - 1.0) * (k_allow - 1.0) - 1.0) * dst / 2.0) : 0.0;
    const std::string v = format_amount(h_allow);
    Limit l;
    l.text = decimal_comma("Допустимая высота падения груза ≈ " + v + " мм");
    l.short_text = decimal_comma("падение с ≈ " + v + " мм");
    l.label = "Допустимая высота";
    l.value = h_allow;
    l.unit = "мм";
    limit = l;
  }
  if (disp_force && limit) limit->force = round3(*disp_force, 3);

  CaseSummary sm;
  sm.name = cs.name;
  sm.duration = cs.duration;
  sm.duration_name = duration_name(cs.duration);
  sm.cycles = cycles;
  sm.temperature = cs.temperature;
  sm.strength_factor = py_round(f_str, 3);
  sm.modulus_factor = py_round(m_case, 3);
  if (bc.impact) sm.impact_factor = py_round(k_imp, 3);
  const auto c_min = model.vm.center(i_min);
  const auto c_core = model.vm.center(i_core);
  sm.sf_min = post.sf[i_min];
  sm.sf_min_xyz = round3({c_min[0] - origin[0], c_min[1] - origin[1], c_min[2] - origin[2]}, 2);
  sm.sf = sf_core;
  sm.sf_xyz = round3({c_core[0] - origin[0], c_core[1] - origin[1], c_core[2] - origin[2]}, 2);
  sm.mode_id = post.mode[i_core];
  sm.mode = std::string(material::failure_mode_name(static_cast<material::FailureMode>(sm.mode_id)));
  sm.sf_min_at_bc = bc_elem[i_min] && post.sf[i_min] < sf_core * 0.95;
  sm.max_disp = dmax;
  const auto& xd = mesh.xyz[i_d];
  sm.max_disp_xyz = round3({xd[0] - origin[0], xd[1] - origin[1], xd[2] - origin[2]}, 2);
  sm.max_disp_vec = {u[3 * i_d], u[3 * i_d + 1], u[3 * i_d + 2]};
  sm.max_stress = *std::max_element(post.vmis.begin(), post.vmis.end());
  sm.verdict = verdict;
  sm.target_sf = target_sf;
  sm.reaction = round3(reaction, 3);
  sm.applied = round3(applied, 3);
  sm.loads = bc.desc;
  sm.limit = limit;
  sm.solver = sinfo;
  sm.warnings = warnings;
  if (disp_force) sm.disp_force = round3(*disp_force, 3);
  double vfail = 0, vall = 0;
  for (std::size_t e = 0; e < ne; ++e) {
    vfail += (post.sf[e] < 1.0 ? 1.0 : 0.0) * mesh.elem_vol[e];
    vall += mesh.elem_vol[e];
  }
  sm.fail_volume_frac = vfail / vall;
  prog(1.0, "готово");

  CaseResult res;
  res.summary = std::move(sm);
  res.u = std::move(u);
  res.sf = std::move(post.sf);
  res.mode = std::move(post.mode);
  res.von_mises = std::move(post.vmis);
  res.sigma = std::move(post.sigma);
  res.bc_elem = std::move(bc_elem);
  res.sel_faces = bc.sel_faces;
  return res;
}

}  // namespace

double py_round(double x, int digits) {
  if (!std::isfinite(x)) return x;
  if (std::abs(x) >= 1e16) return x;
  char buf[400];
  const auto r = std::to_chars(buf, buf + sizeof buf, x, std::chars_format::fixed, digits);
  double out = x;
  std::from_chars(buf, r.ptr, out);
  return out;
}

double Model::thin_fraction() const {
  double num = 0, den = 0;
  for (std::size_t e = 0; e < mesh.nbr.size(); ++e) {
    const auto& nb = mesh.nbr[e];
    const bool thin = (nb[0] < 0 && nb[1] < 0) || (nb[2] < 0 && nb[3] < 0) || (nb[4] < 0 && nb[5] < 0);
    const double w = vm.rho(e) * mesh.elem_vol[e];
    if (thin) num += w;
    den += w;
  }
  return num / std::max(den, 1e-12);
}

Model build_model(gcode::Toolpaths tp, const ModelOptions& options, const Progress& progress) {
  const auto t0 = std::chrono::steady_clock::now();
  Model m;
  m.tp = std::move(tp);
  report(progress, "voxel", 0.3, "Строю воксельную модель");
  voxel::Options vo;
  vo.voxel = options.voxel;
  vo.max_elems = options.max_elems;
  m.vm = voxel::voxelize(m.tp, vo);
  report(progress, "mesh", 0.7, "Строю сетку");
  m.mesh = fem::build_mesh(m.vm);
  const auto& vm = m.vm;
  const Vec3 lo{vm.x0 + vm.sx, vm.y0 + vm.sy, vm.z_edges.front()};
  const Vec3 hi{vm.x0 + (vm.nx - 1) * vm.sx, vm.y0 + (vm.ny - 1) * vm.sy, vm.z_edges.back()};
  m.origin = lo;
  m.size = {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
  m.material_guess = material::guess_material(m.tp.info.filament_type);
  m.build_time = seconds_since(t0);
  report(progress, "ready", 1.0, "Модель готова");
  return m;
}

json::Value model_summary(const Model& model) {
  const auto& info = model.tp.info;
  const auto& st = model.vm.stats;
  Value warns = Value::array_of(info.warnings);
  const double tf = model.thin_fraction();
  if (tf > 0.12)
    warns.push_back(std::format("{}% материала — в стенках толщиной в один воксель. Жёсткость таких стенок на "
                                "изгиб завышается: увеличьте детальность сетки.",
                                static_cast<long long>(py_round(tf * 100, 0))));
  Value s = Value::object();
  s["slicer"] = info.slicer;
  s["filament_type"] = info.filament_type;
  s["material_guess"] = model.material_guess ? Value(*model.material_guess) : Value(nullptr);
  s["layer_height"] = py_round(model.vm.layer_height, 3);
  s["infill_density"] = py_round(model.vm.infill_density, 3);
  s["infill_pattern"] = model.vm.infill_pattern;
  s["wall_loops"] = info.wall_loops ? Value(*info.wall_loops) : Value(nullptr);
  s["size"] = vec_json(round3(model.size, 2));
  s["voxel"] = py_round(model.vm.s, 3);
  s["elements"] = model.vm.size();
  s["nodes"] = model.mesh.n_nodes;
  s["layers"] = info.n_layers;
  s["volume_mm3"] = py_round(st.deposited_volume, 1);
  s["model_volume_mm3"] = py_round(st.model_volume, 1);
  s["removed_fraction"] = py_round(st.removed_fraction, 4);
  s["warnings"] = std::move(warns);
  s["build_time"] = py_round(model.build_time, 2);
  return s;
}

json::Value to_json(const CaseSummary& s) {
  Value v = Value::object();
  auto opt = [](const std::optional<double>& x) { return x ? Value(*x) : Value(nullptr); };
  v["name"] = s.name;
  v["duration"] = s.duration;
  v["duration_name"] = s.duration_name;
  v["cycles"] = opt(s.cycles);
  v["temperature"] = opt(s.temperature);
  v["strength_factor"] = s.strength_factor;
  v["modulus_factor"] = s.modulus_factor;
  v["impact_factor"] = opt(s.impact_factor);
  v["sf_min"] = s.sf_min;
  v["sf_min_xyz"] = vec_json(s.sf_min_xyz);
  v["sf"] = s.sf;
  v["sf_xyz"] = vec_json(s.sf_xyz);
  v["mode"] = s.mode;
  v["mode_id"] = s.mode_id;
  v["sf_min_at_bc"] = s.sf_min_at_bc;
  v["max_disp"] = s.max_disp;
  v["max_disp_xyz"] = vec_json(s.max_disp_xyz);
  v["max_disp_vec"] = vec_json(s.max_disp_vec);
  v["max_stress"] = s.max_stress;
  v["verdict"] = s.verdict;
  v["target_sf"] = s.target_sf;
  v["reaction"] = vec_json(s.reaction);
  v["applied"] = vec_json(s.applied);
  Value loads = Value::array();
  for (const auto& d : s.loads) loads.push_back(d.json);
  v["loads"] = std::move(loads);
  if (s.limit) {
    Value l = Value::object();
    l["text"] = s.limit->text;
    l["short"] = s.limit->short_text;
    l["label"] = s.limit->label;
    l["value"] = s.limit->value;
    l["unit"] = s.limit->unit;
    if (s.limit->force) l["force"] = vec_json(*s.limit->force);
    v["limit"] = std::move(l);
  } else {
    v["limit"] = nullptr;
  }
  Value sv = Value::object();
  sv["method"] = s.solver.method;
  sv["iters"] = s.solver.iterations;
  sv["rel_res"] = s.solver.rel_residual;
  sv["levels"] = s.solver.levels;
  sv["time"] = s.solver.time;
  sv["setup_time"] = s.solver.setup_time;
  v["solver"] = std::move(sv);
  v["warnings"] = Value::array_of(s.warnings);
  v["disp_force"] = s.disp_force ? vec_json(*s.disp_force) : Value(nullptr);
  v["fail_volume_frac"] = s.fail_volume_frac;
  return v;
}

AnalysisResult run_analysis(const Model& model, const Job& job, const Progress& progress) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto& mesh = model.mesh;
  AnalysisResult res;
  if (job.material) {
    res.material = *job.material;
  } else {
    const material::Material* m = nullptr;
    if (model.material_guess) m = material::find_material(*model.material_guess);
    if (!m) m = material::find_material("PLA");
    res.material = *m;
  }
  res.target_sf = job.target_sf;
  if (job.cases.empty()) throw JobError("В задании нет ни одного расчётного случая (cases).");
  const Vec3 origin = job.part_coords ? model.origin : Vec3{0, 0, 0};

  // сначала разбираем все нагрузки — ошибки в задании видны сразу
  report(progress, "material", 0.01, "Проверяю нагрузки и закрепления");
  std::vector<Bc> bcs;
  for (const auto& cs : job.cases) bcs.push_back(fixtures_and_loads(model, cs, origin));
  report(progress, "material", 0.03, "Задаю анизотропию материала");
  const MaterialField mf = make_field(model, res.material);
  // Частично заполненные воксели на опорных и нагруженных гранях считаем сплошными:
  // реальная поверхность лежит внутри вокселя, «размазанный» мягкий слой дал бы
  // ложную податливость в месте закрепления.
  std::vector<std::int64_t> dens;
  for (const auto& bc : bcs)
    for (const auto& [name, faces] : bc.sel_faces)
      for (auto f : faces) dens.push_back(mesh.face_elem[static_cast<std::size_t>(f)]);
  dens = unique_sorted(std::move(dens));
  std::vector<fem::CVec> cvec = mf.cvec;
  for (auto e0 : dens) {
    const auto e = static_cast<std::size_t>(e0);
    const double rho = (*mf.rs)[e] + (*mf.rp)[e];
    if (!(rho < 0.98)) continue;
    const double scale = 1.0 / std::clamp(rho, 0.1, 1.0);
    for (auto& v : cvec[e]) v *= scale;
  }
  report(progress, "assemble", 0.05, "Собираю матрицу жёсткости");
  const linalg::CsrMatrix k = fem::assemble(mesh, cvec);
  SolverCache cache;
  const std::size_t nc = job.cases.size();
  for (std::size_t ci = 0; ci < nc; ++ci) {
    const double base = 0.25 + 0.75 * static_cast<double>(ci) / static_cast<double>(nc);
    const double span = 0.75 / static_cast<double>(nc);
    const std::string& name = job.cases[ci].name;
    report(progress, "case", base, name + ": нагрузки");
    auto prog = [&](double f, std::string_view text) {
      report(progress, "case", base + span * f, name + ": " + std::string(text));
    };
    res.cases.push_back(
        run_case(model, mf, res.material, job, origin, k, cvec, job.cases[ci], bcs[ci], cache, prog));
  }
  res.total_time = seconds_since(t0);
  report(progress, "done", 1.0, "Готово");
  return res;
}

std::string text_summary(const AnalysisResult& r) {
  std::vector<std::string> lines;
  lines.push_back(std::format("Материал: {}   требуемый запас: {:g}", r.material.name, r.target_sf));
  for (const auto& c : r.cases) {
    const auto& s = c.summary;
    const char* verdict = s.verdict == "ok" ? "ВЫДЕРЖИТ" : (s.verdict == "risk" ? "МАЛО ЗАПАСА" : "РАЗРУШИТСЯ");
    lines.push_back(std::format("\n▶ {}  [{}]", s.name, s.duration_name));
    lines.push_back(std::format("   Итог: {}   запас прочности {:.2f}  (в точке {} мм)", verdict, s.sf,
                                json::dump(vec_json(s.sf_xyz))));
    lines.push_back(std::format("   Вид разрушения: {}", s.mode));
    lines.push_back(std::format("   Наибольшее перемещение: {:.3f} мм", s.max_disp));
    if (s.limit) lines.push_back("   " + s.limit->text);
    if (s.impact_factor && *s.impact_factor != 0.0)
      lines.push_back(std::format("   Коэффициент удара: {:.2f}", *s.impact_factor));
    if (s.disp_force)
      lines.push_back(std::format("   Усилие для заданного перемещения: {:.1f} Н", norm3(*s.disp_force)));
    if (s.sf_min_at_bc)
      lines.push_back(std::format(
          "   Прим.: у места закрепления/нагрузки локальный пик {:.2f} — обычно это особенность модели, "
          "проверьте конструкцию узла.",
          s.sf_min));
    for (const auto& w : s.warnings) lines.push_back("   ⚠ " + w);
  }
  std::string out;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (i) out += "\n";
    out += lines[i];
  }
  return out;
}

}  // namespace kika::analysis
