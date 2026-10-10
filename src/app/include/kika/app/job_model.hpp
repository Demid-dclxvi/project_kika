#pragma once
// Задание, как его редактирует окно: закрепления и нагрузки — наборы граней, выбранные мышью.
// Перевод в задание JSON (тот же формат, что у kika run) и обратно. Перенос из fdmfea/web/app.js
// (toBackendJob, fromBackendJob) и server.py (_resolve_job).

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "kika/analysis/analysis.hpp"
#include "kika/app/scene.hpp"
#include "kika/util/json.hpp"

namespace kika::app {

struct LoadTypeInfo {
  const char* key;
  const char* name;
  const char* unit;
  bool faces;  // нужна поверхность приложения
};

// Виды нагрузок в порядке списка в окне.
const std::vector<LoadTypeInfo>& load_types();
const LoadTypeInfo& load_type(const std::string& key);

// Направления: ключ и подпись.
const std::vector<std::pair<const char*, const char*>>& directions();       // для силы, груза, удара
const std::vector<std::pair<const char*, const char*>>& axis_directions();  // только оси и свой вектор
const std::vector<std::pair<const char*, const char*>>& durations();

struct UiFixture {
  int id = 0;
  std::string name = "Закрепление";
  std::vector<std::int64_t> faces;  // ключи граней по возрастанию
  std::string components = "xyz";
};

struct UiLoad {
  int id = 0;
  std::string type = "force";
  std::vector<std::int64_t> faces;
  double value = 10;           // сила, Н; масса, кг; давление, МПа; момент, Н·мм; перегрузка, g
  std::string dir = "-z";      // направление; "custom" — вектор vec
  Vec3 vec{0, 0, -1};
  std::string axis = "+z";     // ось момента
  double height = 300;         // удар: высота падения, мм
  std::array<std::optional<double>, 3> disp{};  // заданное перемещение (пусто — свободно)
};

struct UiCase {
  int id = 0;
  std::string name = "Случай";
  std::string duration = "short";
  double cycles = 1e5;
  std::optional<double> temperature = 23.0;
  bool thermal = false;
  std::vector<UiLoad> loads;
};

struct UiJob {
  std::string material = "PLA";
  std::vector<std::pair<std::string, double>> overrides;  // свои значения свойств материала
  double target_sf = 2.0;
  std::vector<UiFixture> fixtures;
  std::vector<UiCase> cases;
  int next_id = 1;

  int new_id() { return next_id++; }
};

// Новое задание: один пустой случай «Основная нагрузка».
UiJob new_job();
UiLoad default_load(const std::string& type, int id);

// Можно считать: есть деталь, закрепление с гранями и нагрузка.
bool can_run(const UiJob& job);

// Единичный вектор для стрелки нагрузки (пусто — стрелки нет).
std::optional<Vec3> load_arrow(const UiLoad& load, const Scene& scene);

// Материал задания (база + свои значения).
material::Material job_material(const UiJob& job);

// Задание JSON для расчёта и для файла (формат kika run). max_elems и gcode — чтобы
// kika run построил ту же сетку и нашёл деталь.
json::Value to_job_json(const UiJob& job, const std::string& title, const std::string& gcode_name,
                        long long max_elems);

// Задание из файла: области side/box/hole… переводятся в грани текущей сетки.
// warnings — что из задания окно не поддерживает (например, свои закрепления у случая).
UiJob from_job_json(const json::Value& job, const analysis::Model& model, std::vector<std::string>* warnings = nullptr);

// Перенос всех граней задания на другую сетку (смена детальности).
void remap_faces(UiJob& job, const Scene& from, const Scene& to);

}  // namespace kika::app
