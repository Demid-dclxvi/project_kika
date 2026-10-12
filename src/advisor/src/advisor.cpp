// Подбор положения и настроек печати по двум целям из трёх.

#include "kika/advisor/advisor.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>

#include "kika/analysis/job.hpp"
#include "kika/gcode/parser.hpp"

namespace kika::advisor {

namespace {

using geometry::Transform;
using slicer::PrintJob;
using slicer::Support;

std::string num(double v, int digits) {
  std::string s = std::format("{:.{}f}", v, digits);
  std::replace(s.begin(), s.end(), '.', ',');
  return s;
}

std::string minutes(double s) {
  const int m = static_cast<int>(std::lround(s / 60));
  return m >= 60 ? std::format("{} ч {} мин", m / 60, m % 60) : std::format("{} мин", std::max(1, m));
}

bool has(const std::vector<std::string>& ws, std::string_view part) {
  for (const auto& w : ws)
    if (w.find(part) != std::string::npos) return true;
  return false;
}

std::string orientation_label(const std::array<double, 3>& r) {
  if (r == std::array<double, 3>{0, 0, 0}) return "как в файле модели";
  std::string s = "поворот";
  const char axes[] = {'X', 'Y', 'Z'};
  for (std::size_t a = 0; a < 3; ++a)
    if (r[a] != 0) s += std::format(" {} {}°", axes[a], num(r[a], 0));
  return s;
}

std::string walls_word(int n) {
  const int m10 = n % 10, m100 = n % 100;
  if (m10 == 1 && m100 != 11) return "стенка";
  if (m10 >= 2 && m10 <= 4 && (m100 < 12 || m100 > 14)) return "стенки";
  return "стенок";
}

// Первая буква — строчная (русские названия рисунков в UTF-8: А–П → а–п, Р–Я → р–я).
std::string lower_first(std::string s) {
  if (s.size() >= 2 && static_cast<unsigned char>(s[0]) == 0xD0) {
    const auto b = static_cast<unsigned char>(s[1]);
    if (b >= 0x90 && b <= 0x9F) {
      s[1] = static_cast<char>(b + 0x20);
    } else if (b >= 0xA0 && b <= 0xAF) {
      s[0] = static_cast<char>(0xD1);
      s[1] = static_cast<char>(b - 0x20);
    }
  }
  return s;
}

std::string settings_label(const slicer::Settings& s) {
  std::string out = std::format("слой {} · {} {} · {} % {}", num(s.layer_height, 2), s.wall_loops, walls_word(s.wall_loops),
                                num(s.infill_density * 100, 0), lower_first(std::string(slicer::pattern_name(s.pattern))));
  if (s.support != Support::None)
    out += s.support == Support::Tree ? " · древовидные поддержки" : " · обычные поддержки";
  return out;
}

struct Evaluator {
  const Request& rq;
  const Progress& progress;
  double target = 2.0;
  bool loads = false;
  std::size_t done = 0, total = 1;

  void tick(std::string_view text) {
    if (progress) progress(std::min(0.99, static_cast<double>(done) / static_cast<double>(std::max<std::size_t>(total, 1))), text);
  }

  Candidate run(PrintJob pj, bool analyse, int elems = 0) {
    Candidate c;
    auto& s = pj.settings;
    const auto mesh = slicer::placed(*rq.mesh, pj);
    auto r = slicer::slice(mesh, s);
    // нависание без опоры — с поддержками (деревья экономнее, обычные быстрее печатаются)
    if (s.support == Support::None && has(r.warnings, "Нависание без опоры")) {
      s.support = rq.goals.economy ? Support::Tree : Support::Normal;
      r = slicer::slice(mesh, s);
    }
    c.print = pj;
    c.orientation = orientation_label(pj.rotate);
    c.settings = settings_label(s);
    c.time_s = r.print_time_s;
    c.grams = r.filament_g;
    c.support_g = r.support_g;
    c.warnings = r.warnings;
    c.printable = !has(r.warnings, "вершиной или ребром") && !has(r.warnings, "касается стола") &&
                  !has(r.warnings, "без поддержки") && !has(r.warnings, "не помещается");
    if (analyse && loads) {
      analysis::ModelOptions mo;
      mo.max_elems = elems > 0 ? elems : rq.max_elems;
      const auto model = analysis::build_model(gcode::parse(r.gcode), mo);
      const app::Scene scene(model);
      app::UiJob job = rq.job;
      const Transform turn = slicer::turn_between(rq.current.rotate, pj.rotate);
      app::rotate_job(job, *rq.scene, scene, turn.r);
      if (app::can_run(job)) {
        const auto res = analysis::run_analysis(model, analysis::parse_job(app::to_job_json(job, "", "", mo.max_elems)),
                                                [this](std::string_view, double, std::string_view) { tick("Расчёт"); });
        double mn = std::numeric_limits<double>::infinity();
        for (const auto& cs : res.cases) mn = std::min(mn, cs.summary.sf);
        if (std::isfinite(mn)) c.sf = mn;
      } else {
        c.warnings.push_back("После поворота часть поверхностей закреплений и нагрузок потерялась — запас не посчитан.");
      }
    }
    c.feasible = !loads || (c.sf && *c.sf >= target);
    ++done;
    return c;
  }
};

// Оценка внутри режима: время и пластик — относительно лучших среди вариантов, запас —
// относительно нужного. Неподходящие (не печатаются или мало запаса) — в конце.
void score(std::vector<Candidate>& cs, const Goals& g, double target) {
  if (cs.empty()) return;
  double tmin = 1e300, mmin = 1e300;
  for (const auto& c : cs) {
    tmin = std::min(tmin, c.time_s);
    mmin = std::min(mmin, c.grams);
  }
  for (auto& c : cs) {
    const double t = c.time_s / std::max(tmin, 1e-9), m = c.grams / std::max(mmin, 1e-9);
    const double s = c.sf ? *c.sf / target : 1.0;
    double v;
    if (g.speed && g.economy)
      v = 1.0 / (t * m);
    else if (g.speed)
      v = s / t;
    else
      v = s / m;
    if (!c.feasible) v = 1e-3 * s;     // мало запаса: среди таких — у кого запас больше
    if (!c.printable) v *= 1e-3;       // не печатается в таком положении
    c.score = v;
  }
  std::stable_sort(cs.begin(), cs.end(), [](const Candidate& a, const Candidate& b) { return a.score > b.score; });
}

struct Variant {
  int walls;
  double infill;  // %
};

}  // namespace

std::string goals_key(const Goals& g) {
  std::string k;
  for (auto [on, name] : {std::pair{g.speed, "speed"}, std::pair{g.economy, "economy"}, std::pair{g.strength, "strength"}})
    if (on) k += (k.empty() ? "" : ",") + std::string(name);
  return k;
}

std::optional<Goals> goals_from_key(std::string_view key) {
  Goals g{false, false, false};
  std::size_t p = 0;
  while (p <= key.size()) {
    const std::size_t e = std::min(key.find(',', p), key.size());
    const std::string_view w = key.substr(p, e - p);
    if (w == "speed" || w == "скорость")
      g.speed = true;
    else if (w == "economy" || w == "экономия")
      g.economy = true;
    else if (w == "strength" || w == "прочность")
      g.strength = true;
    else
      return std::nullopt;
    p = e + 1;
  }
  if (g.count() != 2) return std::nullopt;
  return g;
}

std::string goals_name(const Goals& g) {
  if (g.speed && g.economy) return "быстро и экономно";
  if (g.speed && g.strength) return "быстро и прочно";
  return "экономно и прочно";
}

std::vector<std::array<double, 3>> orientations(const std::array<double, 3>& current) {
  const Transform cur = slicer::rotation_of(current);
  std::vector<std::array<double, 3>> out;
  const std::pair<int, double> turns[] = {{0, 0}, {0, 90}, {0, -90}, {0, 180}, {1, 90}, {1, -90}};
  for (const auto& [axis, deg] : turns) out.push_back(slicer::euler_xyz(Transform::rotation(axis, deg) * cur));
  return out;
}

Result advise(const Request& rq, const Progress& progress) {
  Result res;
  if (!rq.mesh) return res;
  const Goals g = rq.goals;
  Evaluator ev{rq, progress};
  ev.target = rq.job.target_sf;
  ev.loads = rq.model && rq.scene && app::can_run(rq.job);
  res.strength_evaluated = ev.loads;

  // базовые настройки для сравнения положений
  PrintJob base = rq.current;
  auto& bs = base.settings;
  const double nozzle = bs.printer.nozzle;
  const double fast_layer = std::round(std::min(0.28, 0.7 * nozzle) * 100) / 100;  // до сотых мм
  bs.layer_height = g.speed ? fast_layer : 0.2;
  bs.first_layer_height = 0.2;
  bs.wall_loops = 3;
  bs.infill_density = 0.2;
  bs.pattern = slicer::Pattern::Grid;
  bs.support = Support::None;

  const auto orients = orientations(rq.current.rotate);
  std::vector<Variant> variants;
  if (g.speed && g.economy)
    variants = {{2, 10}, {2, 15}, {3, 15}, {2, 25}, {3, 20}, {4, 20}, {3, 35}, {4, 40}, {6, 30}};
  else if (g.speed)
    variants = {{2, 15}, {3, 20}, {4, 20}, {4, 35}, {6, 25}, {6, 45}, {8, 40}};
  else
    variants = {{2, 10}, {3, 10}, {3, 15}, {4, 15}, {4, 25}, {6, 15}, {6, 25}};
  const std::vector<slicer::Pattern> extra_patterns =
      g.strength ? (g.speed ? std::vector{slicer::Pattern::Triangles, slicer::Pattern::Cubic}
                            : std::vector{slicer::Pattern::Triangles, slicer::Pattern::Gyroid, slicer::Pattern::Cubic})
                 : std::vector<slicer::Pattern>{};
  const bool refine = ev.loads && rq.refine_elems > rq.max_elems;
  constexpr std::size_t kRefineOk = 3, kRefineMax = 4;  // уточнить, пока не найдутся три подходящих
  ev.total = 1 + orients.size() + variants.size() + extra_patterns.size() + (refine ? kRefineMax + 1 : 0);

  // 0. как сейчас — для сравнения
  ev.tick("Как сейчас");
  Candidate now = ev.run(rq.current, true);
  now.orientation = "сейчас: " + now.orientation;

  // 1. положения
  std::vector<Candidate> all;
  for (std::size_t k = 0; k < orients.size(); ++k) {
    ev.tick(std::format("Положение {} из {}", k + 1, orients.size()));
    PrintJob pj = base;
    pj.rotate = orients[k];
    all.push_back(ev.run(pj, true));
  }
  res.orientations = all;
  score(res.orientations, g, ev.target);
  const Candidate best_orient = res.orientations.front();

  // 2. настройки на лучшем положении
  PrintJob on = best_orient.print;
  on.settings.support = Support::None;  // поддержки включатся сами, если нужны
  std::vector<Candidate> tuned;
  if (g.speed && g.economy) {
    // самый дешёвый вариант, который выдерживает: режем всё (быстро), считаем по порядку цены
    std::vector<Candidate> sliced;
    for (const auto& v : variants) {
      ev.tick("Настройки");
      PrintJob pj = on;
      pj.settings.wall_loops = v.walls;
      pj.settings.infill_density = v.infill / 100;
      sliced.push_back(ev.run(pj, false));
    }
    std::stable_sort(sliced.begin(), sliced.end(),
                     [](const Candidate& a, const Candidate& b) { return a.time_s * a.grams < b.time_s * b.grams; });
    int after_ok = 0;
    for (auto& c : sliced) {
      if (ev.loads && after_ok < 2) {
        ev.tick("Расчёт варианта");
        Candidate full = ev.run(c.print, true);
        --ev.done;  // уже учтён при нарезке
        c = full;
        // с запасом на ошибку грубой сетки: уточнение может немного снизить запас
        if (c.feasible && *c.sf >= 1.1 * ev.target) ++after_ok;
      } else if (ev.loads) {
        c.sf.reset();
        c.feasible = false;  // не считали — не рекомендуем
        c.warnings.push_back("Запас не считался: нашлись варианты дешевле, которые выдерживают.");
      }
      tuned.push_back(c);
    }
  } else {
    for (const auto& v : variants) {
      ev.tick("Настройки");
      PrintJob pj = on;
      pj.settings.wall_loops = v.walls;
      pj.settings.infill_density = v.infill / 100;
      tuned.push_back(ev.run(pj, true));
    }
    score(tuned, g, ev.target);
    // рисунок заполнения — на лучшем сочетании стенок и заполнения
    const PrintJob top = tuned.front().print;
    for (const auto pat : extra_patterns) {
      ev.tick("Рисунок заполнения");
      PrintJob pj = top;
      pj.settings.pattern = pat;
      pj.settings.support = Support::None;
      tuned.push_back(ev.run(pj, true));
    }
  }
  all.insert(all.end(), tuned.begin(), tuned.end());
  score(all, g, ev.target);
  // без повторов по сути (одинаковые положение и настройки)
  std::vector<Candidate> uniq;
  for (auto& c : all) {
    bool dup = false;
    for (const auto& u : uniq)
      dup = dup || (u.orientation == c.orientation && u.settings == c.settings);
    if (!dup) uniq.push_back(std::move(c));
  }
  // 3. уточнение: запас лучших — на подробной сетке; подходящие уточнённые — первыми
  if (refine) {
    std::size_t checked = 0, ok = 0;
    for (auto& c : uniq) {
      if (checked >= kRefineMax || ok >= kRefineOk) break;
      if (!c.printable || !c.sf) continue;
      ev.tick("Уточнение запаса");
      const Candidate f = ev.run(c.print, true, rq.refine_elems);
      c.sf = f.sf;
      c.feasible = f.feasible;
      c.refined = true;
      ++checked;
      ok += c.feasible ? 1 : 0;
    }
    if (now.sf) {
      ev.tick("Уточнение запаса");
      const Candidate f = ev.run(rq.current, true, rq.refine_elems);
      now.sf = f.sf;
      now.feasible = f.feasible;
      now.refined = true;
    }
    score(uniq, g, ev.target);
    std::stable_partition(uniq.begin(), uniq.end(), [](const Candidate& c) { return c.refined && c.feasible && c.printable; });
  }
  res.ranked = std::move(uniq);
  if (res.ranked.size() > 6) res.ranked.resize(6);
  res.current = now;

  // объяснение
  const Candidate& best = res.ranked.front();
  std::string s = std::format("Цель — {}. Лучше всего: {}; {}.", goals_name(g), best.orientation, best.settings);
  auto metrics = [&](const Candidate& c) {
    std::string m = std::format("печать ≈ {}, пластик {} г", minutes(c.time_s), num(c.grams, 0));
    if (c.support_g > 0) m += std::format(" (из них поддержки {} г)", num(c.support_g, 1));
    if (c.sf) m = std::format("запас {}{} (нужно {}), ", c.refined ? "" : "≈ ", num(*c.sf, 2), num(ev.target, 1)) + m;
    return m;
  };
  s += " Ожидается: " + metrics(best) + ".";
  s += " Сейчас: " + metrics(now) + ".";
  if (!ev.loads)
    s += g.strength ? " Прочность не оценивалась: задайте закрепления и нагрузки и подберите заново."
                    : " Прочность не проверялась: закрепления и нагрузки не заданы.";
  else if (!best.feasible)
    s += " Ни один вариант не дал нужного запаса — нужны другая форма детали, другой пластик или меньше нагрузка.";
  else if (best.refined)
    s += std::format(" Варианты сравнивались на грубой сетке, запас лучших уточнён на сетке до {} тыс. элементов.",
                     rq.refine_elems / 1000);
  else
    s += " Запас посчитан на грубой сетке, чтобы сравнить варианты; выбранный вариант посчитайте заново на полной сетке.";
  res.summary = s;
  if (progress) progress(1.0, "Готово");
  return res;
}

}  // namespace kika::advisor
