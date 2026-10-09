#pragma once
// Расчёт: модель по G-code, поле материала, расчётные случаи, запас прочности.
// Перенос fdmfea/analysis.py (Model, MaterialField, Analysis) и text_summary из export.py.

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "kika/analysis/job.hpp"
#include "kika/analysis/loads.hpp"
#include "kika/fem/assembly.hpp"
#include "kika/fem/mesh.hpp"
#include "kika/gcode/toolpaths.hpp"
#include "kika/material/material.hpp"
#include "kika/util/json.hpp"
#include "kika/voxel/voxel_model.hpp"

namespace kika::analysis {

// Ход расчёта: этап, доля выполненного 0…1, текст для человека.
using Progress = std::function<void(std::string_view stage, double fraction, std::string_view text)>;

// G-code → воксели → сетка. Строится один раз, расчётов по ней можно сделать сколько угодно.
struct Model {
  gcode::Toolpaths tp;
  voxel::VoxelModel vm;
  fem::Mesh mesh;
  Vec3 origin{};  // начало системы детали: минимальный угол габарита по наружной кромке валиков
  Vec3 size{};
  std::optional<std::string> material_guess;
  double build_time = 0.0;

  // Доля материала в элементах толщиной в один воксель (тонкие стенки).
  double thin_fraction() const;
};

struct ModelOptions {
  std::optional<double> voxel;
  int max_elems = 120000;
};

Model build_model(gcode::Toolpaths tp, const ModelOptions& options = {}, const Progress& progress = {});

// Сводка по модели — как Model.summary() прототипа.
json::Value model_summary(const Model& model);

// Описание нагрузки для отчёта.
struct LoadDesc {
  std::string type;
  double magnitude = 0.0;
  std::string unit;
  json::Value json;  // все поля в порядке прототипа
};

// Предельная нагрузка (линейный расчёт: нагрузка × запас) или допустимая высота падения.
struct Limit {
  std::string text;
  std::string short_text;
  std::string label;
  double value = 0.0;
  std::string unit;
  std::optional<Vec3> force;  // усилие для заданного перемещения
};

struct CaseSummary {
  std::string name;
  std::string duration;
  std::string duration_name;
  std::optional<double> cycles;
  std::optional<double> temperature;
  double strength_factor = 1.0;
  double modulus_factor = 1.0;
  std::optional<double> impact_factor;
  double sf_min = 0.0;  // наименьший запас по всей детали
  Vec3 sf_min_xyz{};
  double sf = 0.0;  // наименьший запас вне мест закрепления и приложения нагрузок
  Vec3 sf_xyz{};
  std::string mode;  // вид разрушения в точке наименьшего запаса
  int mode_id = 0;
  bool sf_min_at_bc = false;
  double max_disp = 0.0;
  Vec3 max_disp_xyz{};
  Vec3 max_disp_vec{};
  double max_stress = 0.0;
  std::string verdict;  // ok, risk, fail
  double target_sf = 2.0;
  Vec3 reaction{};
  Vec3 applied{};
  std::vector<LoadDesc> loads;
  std::optional<Limit> limit;
  fem::SolveInfo solver;
  std::vector<std::string> warnings;
  std::optional<Vec3> disp_force;
  double fail_volume_frac = 0.0;
};

json::Value to_json(const CaseSummary& s);

struct CaseResult {
  CaseSummary summary;
  std::vector<double> u;          // перемещения узлов, 3N
  std::vector<double> sf;         // запас прочности по элементам
  std::vector<std::int8_t> mode;  // вид разрушения по элементам
  std::vector<double> von_mises;  // эквивалентное напряжение в критической точке
  std::vector<double> sigma;      // среднее напряжение в элементе, 6 на элемент (глобальные оси)
  std::vector<char> bc_elem;      // элемент касается закрепления или нагрузки
  std::vector<std::pair<std::string, std::vector<std::int64_t>>> sel_faces;
};

struct AnalysisResult {
  material::Material material;
  double target_sf = 2.0;
  std::vector<CaseResult> cases;
  double total_time = 0.0;
};

// Расчёт всех случаев задания. Ошибки в задании — JobError / SelectionError до начала счёта.
AnalysisResult run_analysis(const Model& model, const Job& job, const Progress& progress = {});

// Итог по-русски, как text_summary прототипа.
std::string text_summary(const AnalysisResult& result);

// Округление как round(x, n) в Python (до ближайшего, половины — к чётному по точному значению).
double py_round(double x, int digits);

}  // namespace kika::analysis
