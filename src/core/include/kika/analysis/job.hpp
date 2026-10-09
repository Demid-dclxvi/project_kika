#pragma once
// Задание на расчёт: материал, закрепления, расчётные случаи с нагрузками.
// Формат — тот же JSON, что у прототипа (prototype/examples/*.job.json).

#include <array>
#include <optional>
#include <string>
#include <vector>

#include "kika/analysis/loads.hpp"
#include "kika/material/material.hpp"
#include "kika/util/json.hpp"

namespace kika::analysis {

// Ошибка в задании (по-русски, с указанием места).
class JobError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

struct Fixture {
  std::string name;
  Region where;
  std::array<bool, 3> components{true, true, true};  // закреплённые оси x, y, z
};

enum class LoadType { Force, Mass, Pressure, Moment, Bearing, Impact, Displacement, Gravity };

struct Load {
  LoadType type = LoadType::Force;
  std::string type_name = "force";  // как записано в задании (в нижнем регистре)
  std::string label;                // «Нагрузка 1 (force)» — для сообщений
  Region where;                     // у силы тяжести нет
  std::optional<Vec3> vector;       // сила/болт: вектор силы; момент: вектор момента
  double value = 0.0;               // сила/болт: величина; давление, МПа; момент, Н·мм
  Direction direction;              // сила/болт/груз/удар
  std::optional<Vec3> point;        // сила/груз: точка приложения (переносит силу с моментом)
  double kg = 0.0;                  // груз, удар
  double height = 0.0;              // удар: высота падения, мм
  std::optional<Direction> axis;    // момент: ось (вместе с value)
  std::optional<Vec3> center;       // момент: центр
  std::array<std::optional<double>, 3> displacement{};  // заданное перемещение (null — свободно)
  Vec3 g{0, 0, -1};                 // сила тяжести / перегрузка, в g
};

struct Case {
  std::string name;              // для вывода («Случай N», если не задано)
  std::string name_in_job;       // как записано (для сообщений об ошибках)
  std::vector<Fixture> fixtures;
  std::vector<Load> loads;
  std::string duration = "short";  // short, long, cyclic
  std::optional<double> temperature;
  double cycles = 1e5;
  bool thermal_expansion = false;
};

struct Job {
  std::optional<material::Material> material;  // нет — по типу пластика из G-code, иначе PLA
  double target_sf = 2.0;
  bool part_coords = true;  // координаты от угла габарита детали (иначе — координаты принтера)
  std::vector<Case> cases;
  // для командной строки
  std::optional<std::string> gcode;
  std::optional<std::string> title;
  std::optional<std::string> subtitle;
  std::optional<double> voxel;
  std::optional<long long> max_elems;
  // Точность решателя (относительная невязка). Не из файла задания: задаётся программой,
  // например для сверки с прототипом при более строгой точности.
  double solver_tol = 1e-7;
};

// Разбор задания. Бросает JobError / SelectionError с понятным сообщением.
Job parse_job(const json::Value& v);

// Материал: название из базы или объект {"base": "PETG", "E1": …} с переопределениями.
material::Material material_from_json(const json::Value& spec);

}  // namespace kika::analysis
