#pragma once
// Подбор положения детали на столе и настроек печати: две цели из трёх — скорость, экономия,
// прочность («быстро, дёшево, прочно — выберите два»).
//
// Кандидаты режутся своим слайсером (время и пластик — из нарезки, вместе с поддержками) и
// считаются на грубой сетке (запас прочности по закреплениям и нагрузкам задания). Сначала
// выбирается положение (какой гранью вниз), потом на нём — слой, стенки, заполнение, рисунок.
// Прочность — всегда условие: если нагрузки заданы, подходят только варианты с запасом не ниже
// нужного; цель «прочность» — сверх того взять запас побольше.

#include <array>
#include <atomic>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/analysis/analysis.hpp"
#include "kika/app/job_model.hpp"
#include "kika/app/scene.hpp"
#include "kika/geometry/mesh.hpp"
#include "kika/slicer/print_job.hpp"

namespace kika::advisor {

struct Goals {
  bool speed = true;
  bool economy = true;
  bool strength = false;
  int count() const { return speed + economy + strength; }
};

// Ключ режима: "speed,economy" и т. п.; разбор — с проверкой, что выбрано ровно две цели.
std::string goals_key(const Goals& g);
std::optional<Goals> goals_from_key(std::string_view key);
std::string goals_name(const Goals& g);  // «быстро и дёшево»

struct Candidate {
  slicer::PrintJob print;            // поворот (от модели в файле) и настройки нарезки
  std::string orientation;           // «как в файле», «поворот X 90°»
  std::string settings;              // «слой 0,28 · 3 стенки · 20 % сетка · без поддержек»
  double time_s = 0;
  double grams = 0;                  // весь пластик, вместе с поддержками
  double support_g = 0;
  std::optional<double> sf;          // наименьший запас по случаям (нет нагрузок — пусто)
  bool printable = true;             // стоит на столе гранью, нависания поддержаны
  bool feasible = true;              // запас не ниже нужного (или нагрузок нет)
  double score = 0;                  // больше — лучше (внутри режима)
  std::vector<std::string> warnings;
};

struct Request {
  const geometry::TriangleMesh* mesh = nullptr;  // модель как в файле
  slicer::PrintJob current;                      // текущие поворот и настройки (принтер, пластик…)
  // Задание (закрепления и нагрузки) на текущей модели; без нагрузок прочность не оценивается.
  const analysis::Model* model = nullptr;
  const app::Scene* scene = nullptr;
  app::UiJob job;
  Goals goals;
  int max_elems = 20000;                         // грубая сетка для сравнения вариантов
};

struct Result {
  std::vector<Candidate> ranked;  // лучшие первыми; первый — рекомендация
  std::vector<Candidate> orientations;  // все положения с базовыми настройками
  bool strength_evaluated = false;
  std::string summary;            // объяснение по-русски
};

// Ход: доля 0…1 и текст. Бросить исключение из progress — прервать подбор.
using Progress = std::function<void(double fraction, std::string_view text)>;

Result advise(const Request& request, const Progress& progress = {});

// Шесть положений «какой гранью вниз» относительно текущего: углы X, Y, Z от модели в файле.
std::vector<std::array<double, 3>> orientations(const std::array<double, 3>& current);

}  // namespace kika::advisor
