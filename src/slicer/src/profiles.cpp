// Таблица настроек нарезки: JSON профилей и задания, окно настроек.

#include "kika/slicer/profiles.hpp"

#include <cmath>
#include <format>
#include <optional>
#include <type_traits>

#include "kika/analysis/job.hpp"

namespace kika::slicer {

namespace {

using json::Value;

template <class T>
T& part(Settings& s);
template <>
Printer& part<Printer>(Settings& s) { return s.printer; }
template <>
Filament& part<Filament>(Settings& s) { return s.filament; }
template <>
Settings& part<Settings>(Settings& s) { return s; }

template <class T>
const T& cpart(const Settings& s) { return part<T>(const_cast<Settings&>(s)); }

template <class T>
constexpr Section section_of() {
  if constexpr (std::is_same_v<T, Printer>) return Section::Printer;
  else if constexpr (std::is_same_v<T, Filament>) return Section::Filament;
  else return Section::Process;
}

// Значение для показа: без «хвостов» вида 15,000000000000002.
double tidy(double v) { return std::round(v * 1e9) / 1e9; }

class Builder {
 public:
  explicit Builder(std::vector<Field>& out) : out_(out) {}
  void page(std::string p) {
    page_ = std::move(p);
    group_.clear();
  }
  void group(std::string g) { group_ = std::move(g); }

  template <class T>
  Field& number(std::string key, std::string label, std::string unit, double T::*m, double lo, double hi,
                double scale = 1, std::string tip = {}) {
    Field f = base<T>(std::move(key), std::move(label), std::move(unit), std::move(tip));
    f.kind = FieldKind::Number;
    f.lo = lo;
    f.hi = hi;
    f.scale = scale;
    f.get = [m, scale](const Settings& s) { return Value(tidy(cpart<T>(s).*m * scale)); };
    f.set = [m, scale](Settings& s, const Value& v) { part<T>(s).*m = v.as_double() / scale; };
    return push(std::move(f));
  }

  template <class T>
  Field& integer(std::string key, std::string label, std::string unit, int T::*m, int lo, int hi, std::string tip = {}) {
    Field f = base<T>(std::move(key), std::move(label), std::move(unit), std::move(tip));
    f.kind = FieldKind::Integer;
    f.lo = lo;
    f.hi = hi;
    f.get = [m](const Settings& s) { return Value(cpart<T>(s).*m); };
    f.set = [m](Settings& s, const Value& v) { part<T>(s).*m = static_cast<int>(std::lround(v.as_double())); };
    return push(std::move(f));
  }

  template <class T>
  Field& boolean(std::string key, std::string label, bool T::*m, std::string tip = {}) {
    Field f = base<T>(std::move(key), std::move(label), {}, std::move(tip));
    f.kind = FieldKind::Bool;
    f.get = [m](const Settings& s) { return Value(cpart<T>(s).*m); };
    f.set = [m](Settings& s, const Value& v) { part<T>(s).*m = v.as_bool(); };
    return push(std::move(f));
  }

  template <class T>
  Field& text(std::string key, std::string label, std::string T::*m, FieldKind kind = FieldKind::Text, std::string tip = {}) {
    Field f = base<T>(std::move(key), std::move(label), {}, std::move(tip));
    f.kind = kind;
    f.get = [m](const Settings& s) { return Value(cpart<T>(s).*m); };
    f.set = [m](Settings& s, const Value& v) { part<T>(s).*m = v.as_string(); };
    return push(std::move(f));
  }

  // Вариант из перечисления: E — тип, keys — все значения, key/name/from — функции перевода.
  template <class T, class E>
  Field& choice(std::string key, std::string label, E T::*m, const std::vector<E>& all, std::string_view (*to_key)(E),
                std::string_view (*to_name)(E), std::optional<E> (*from)(std::string_view), std::string tip = {}) {
    Field f = base<T>(std::move(key), std::move(label), {}, std::move(tip));
    f.kind = FieldKind::Choice;
    for (E e : all) f.choices.push_back({std::string(to_key(e)), std::string(to_name(e))});
    f.canonical = [from, to_key](std::string_view k) -> std::string {
      const auto e = from(k);
      return e ? std::string(to_key(*e)) : std::string();
    };
    f.get = [m, to_key](const Settings& s) { return Value(std::string(to_key(cpart<T>(s).*m))); };
    f.set = [m, from](Settings& s, const Value& v) {
      if (const auto e = from(v.as_string())) part<T>(s).*m = *e;
    };
    return push(std::move(f));
  }

 private:
  template <class T>
  Field base(std::string key, std::string label, std::string unit, std::string tip) {
    Field f;
    f.section = section_of<T>();
    f.key = std::move(key);
    f.page = page_;
    f.group = group_;
    f.label = std::move(label);
    f.unit = std::move(unit);
    f.tip = std::move(tip);
    return f;
  }
  Field& push(Field f) {
    out_.push_back(std::move(f));
    return out_.back();
  }

  std::vector<Field>& out_;
  std::string page_, group_;
};

std::string_view flavor_name(Flavor f) { return f == Flavor::Klipper ? "Klipper" : "Marlin"; }

std::vector<Field> build() {
  std::vector<Field> v;
  v.reserve(120);
  Builder b(v);
  using P = Printer;
  using F = Filament;
  using S = Settings;

  // Страницы и разделы — как в OrcaSlicer (названия и порядок; код и профили Orca не используются).
  // ------------------------------------------------------------------ принтер
  b.page("Основное");
  b.group("Принтер");
  b.text<P>("name", "Название", &P::name);
  b.choice<P, Flavor>("flavor", "Прошивка", &P::flavor, {Flavor::Klipper, Flavor::Marlin}, flavor_key, flavor_name,
                      flavor_from_key, "Klipper: разметка деталей, номера слоёв на экране, pressure advance");
  b.group("Область печати");
  b.number<P>("bed_x", "Стол по X", "мм", &P::bed_x, 50, 2000);
  b.number<P>("bed_y", "Стол по Y", "мм", &P::bed_y, 50, 2000);
  b.number<P>("max_z", "Высота печати", "мм", &P::max_z, 20, 2000);
  b.text<P>("bed_exclude", "Запретная область стола", &P::bed_exclude, FieldKind::Text,
            "Углы многоугольника «x×y» через запятую, мм: 0x0,11x0,11x16,0x16; несколько — через «;». Деталь там "
            "ставить нельзя (механизм реза, чистка сопла)")
      .check_text = [](std::string_view t) {
    std::string err;
    parse_bed_exclude(t, &err);
    return err;
  };
  b.group("Прочее");
  b.number<P>("z_offset", "Смещение по Z", "мм", &P::z_offset, -5, 5, 1, "Прибавляется ко всем высотам в G-code; обычно 0");
  b.boolean<P>("aux_fan", "Есть боковой обдув (M106 P2)", &P::aux_fan);
  b.boolean<P>("air_filtration", "Есть очистка воздуха камеры (M106 P3)", &P::air_filtration,
               "Вентилятор фильтра камеры; включает его пластик (страница «Охлаждение»)");
  b.boolean<P>("thumbnails", "Миниатюра в G-code", &P::thumbnails, "Картинка детали для экрана принтера и Moonraker");
  b.page("Экструдер");
  b.group("Сопло");
  b.number<P>("nozzle", "Диаметр сопла", "мм", &P::nozzle, 0.1, 2.0);
  b.choice<P, NozzleType>("nozzle_type", "Материал сопла", &P::nozzle_type,
                          {NozzleType::Brass, NozzleType::Stainless, NozzleType::Hardened}, nozzle_type_key, nozzle_type_name,
                          nozzle_type_from_key, "Пластик с волокном (CF, GF) быстро стирает латунное сопло");
  b.number<P>("filament_diameter", "Диаметр прутка", "мм", &P::filament_diameter, 1.0, 3.5);
  b.number<P>("min_layer_height", "Слой не тоньше", "мм", &P::min_layer_height, 0.01, 2.0, 1,
              "Пределы высоты слоя для этого сопла: за ними — предупреждение");
  b.number<P>("max_layer_height", "Слой не толще", "мм", &P::max_layer_height, 0.02, 2.0);
  b.group("Откат");
  b.number<P>("retract_length", "Длина отката", "мм", &P::retract_length, 0, 10);
  b.number<P>("retract_speed", "Скорость отката", "мм/с", &P::retract_speed, 5, 150);
  b.number<P>("deretract_speed", "Скорость подачи после отката", "мм/с", &P::deretract_speed, 0, 150, 1, "0 — как откат");
  b.number<P>("retract_restart_extra", "Добавка при подаче", "мм", &P::retract_restart_extra, -2, 5, 1,
              "Подать больше (или меньше), чем откатили: против пустот после перемещений")
      .advanced = true;
  b.number<P>("retract_min_travel", "Откат, если перемещение длиннее", "мм", &P::retract_min_travel, 0, 20);
  b.boolean<P>("retract_layer_change", "Откат при смене слоя", &P::retract_layer_change);
  b.boolean<P>("wipe", "Протирка при откате", &P::wipe,
               "Сопло при откате возвращается по только что напечатанной линии — подтёк остаётся на ней");
  b.number<P>("wipe_distance", "Длина протирки", "мм", &P::wipe_distance, 0, 10);
  b.number<P>("z_hop", "Подъём сопла при откате", "мм", &P::z_hop, 0, 5, 1, "0 — без подъёма");
  b.group("Пределы температуры");
  b.number<P>("max_nozzle_temp", "Сопло не горячее", "°C", &P::max_nozzle_temp, 150, 500);
  b.number<P>("max_bed_temp", "Стол не горячее", "°C", &P::max_bed_temp, 0, 200);
  b.number<P>("max_chamber_temp", "Камера не горячее", "°C", &P::max_chamber_temp, 0, 120, 1,
              "0 — камера без подогрева: температура камеры пластика не задаётся");
  b.page("Движение");
  b.group("Пределы движения");
  b.number<P>("max_speed", "Скорость не выше", "мм/с", &P::max_speed, 20, 2000);
  b.number<P>("max_accel", "Ускорение не выше", "мм/с²", &P::max_accel, 100, 100000);
  b.number<P>("corner_speed", "Скорость в прямом углу", "мм/с", &P::corner_speed, 1, 50, 1,
              "square_corner_velocity у Klipper, рывок (jerk) у Marlin — для оценки времени");
  b.number<P>("max_speed_z", "Скорость по Z", "мм/с", &P::max_speed_z, 1, 100);
  b.page("G-code");
  b.group("G-code принтера");
  b.text<P>("start_gcode", "Стартовый код", &P::start_gcode, FieldKind::Code,
            "Подстановки: {first_layer_nozzle_temp}, {first_layer_bed_temp}, {nozzle_temp}, {bed_temp}, {chamber_temp}, "
            "{bed_x}, {bed_y}, {max_z}, {filament_type}, {total_layers}");
  b.text<P>("end_gcode", "Финальный код", &P::end_gcode, FieldKind::Code,
            "Подстановки как в стартовом и ещё {max_layer_z}, {lift_z} (верх детали + 3 мм), {park_z} (не ниже "
            "половины высоты печати)");
  b.text<P>("before_layer_gcode", "Код перед сменой слоя", &P::before_layer_gcode, FieldKind::Code,
            "До подъёма на новый слой. Подстановки: {layer_num} (номер нового слоя, с 1), {layer_z}");
  b.text<P>("layer_gcode", "Код в начале слоя", &P::layer_gcode, FieldKind::Code, "Подстановки: {layer_num} (с 1), {layer_z}");
  b.text<P>("pause_gcode", "Код паузы", &P::pause_gcode, FieldKind::Code,
            "Для пауз на слоях (настройки печати, «Прочее»): Klipper — PAUSE, Marlin — M601");

  // ------------------------------------------------------------------ пластик
  b.page("Пластик");
  b.group("Пластик");
  b.text<F>("name", "Название", &F::name);
  b.text<F>("type", "Тип", &F::type, FieldKind::Text, "Как в базе материалов: PLA, PETG, ABS, ASA, PC, PA, TPU, …-CF");
  b.text<F>("vendor", "Производитель", &F::vendor);
  b.boolean<F>("abrasive", "С волокном (абразивный)", &F::abrasive,
               "Углеволокно, стекловолокно: нужно сопло из закалённой стали");
  b.number<F>("density", "Плотность", "г/см³", &F::density, 0.5, 3.0);
  b.number<F>("cost", "Цена за кг", "", &F::cost, 0, 1e6, 1, "0 — не считать стоимость");
  b.group("Поток");
  b.number<F>("flow", "Поток", "%", &F::flow, 50, 150, 100, "Поправка экструзии: меньше 100 % — меньше пластика");
  b.number<F>("shrinkage", "Усадка по XY", "%", &F::shrinkage, 80, 120, 100,
              "Размер после остывания в процентах от напечатанного: 99,5 % — деталь печатается на 0,5 % крупнее");
  b.number<F>("max_volumetric", "Расход не больше", "мм³/с", &F::max_volumetric, 0.5, 100, 1,
              "Сколько пластика сопло успевает расплавить; скорость линий снижается под этот предел");
  b.number<F>("max_speed", "Скорость не выше", "мм/с", &F::max_speed, 5, 2000);
  b.number<F>("pressure_advance", "Pressure advance", "", &F::pressure_advance, -1, 2, 1,
              "Klipper: SET_PRESSURE_ADVANCE; −1 — не задавать (останется настройка принтера)");
  b.group("Температура");
  b.number<F>("first_layer_nozzle_temp", "Сопло, первый слой", "°C", &F::first_layer_nozzle_temp, 0, 500);
  b.number<F>("nozzle_temp", "Сопло", "°C", &F::nozzle_temp, 0, 500);
  b.number<F>("first_layer_bed_temp", "Стол, первый слой", "°C", &F::first_layer_bed_temp, 0, 200);
  b.number<F>("bed_temp", "Стол", "°C", &F::bed_temp, 0, 200);
  b.number<F>("chamber_temp", "Камера", "°C", &F::chamber_temp, 0, 120, 1, "0 — не греть (только у принтеров с подогревом камеры)");
  b.number<F>("nozzle_temp_min", "Рекомендуемое сопло от", "°C", &F::nozzle_temp_min, 0, 500, 1,
              "Диапазон производителя: температура вне его — предупреждение; 0 — не проверять");
  b.number<F>("nozzle_temp_max", "Рекомендуемое сопло до", "°C", &F::nozzle_temp_max, 0, 500);
  b.page("Охлаждение");
  b.group("Обдув модели");
  b.number<F>("fan_min", "Обдув, наименьший", "%", &F::fan_min, 0, 100, 100);
  b.number<F>("fan_max", "Обдув, наибольший", "%", &F::fan_max, 0, 100, 100);
  b.integer<F>("fan_off_layers", "Без обдува первые", "слоёв", &F::fan_off_layers, 0, 20);
  b.integer<F>("full_fan_layer", "Полный обдув со слоя", "", &F::full_fan_layer, 0, 100,
               "Обдув нарастает плавно от первого слоя с обдувом до этого; 0 — сразу полный");
  b.number<F>("fan_full_time", "Обдув сильнее, если слой короче", "с", &F::fan_full_time, 0, 300, 1,
              "От наименьшего обдува (слой дольше) до наибольшего (слой короче времени замедления)");
  b.number<F>("overhang_fan", "Обдув нависаний и мостов", "%", &F::overhang_fan, 0, 100, 100);
  b.number<F>("aux_fan", "Боковой обдув", "%", &F::aux_fan, 0, 100, 100, "Только если у принтера есть боковой вентилятор");
  b.group("Замедление коротких слоёв");
  b.number<F>("slowdown_time", "Замедлять, если слой короче", "с", &F::slowdown_time, 0, 120, 1,
              "Чтобы слой успел остыть; 0 — не замедлять");
  b.number<F>("slowdown_speed", "Замедлять не ниже", "мм/с", &F::slowdown_speed, 1, 200);
  b.group("Очистка воздуха камеры");
  b.boolean<F>("air_filtration", "Включать очистку воздуха", &F::air_filtration,
               "Вентилятор фильтра камеры (M106 P3), если он есть у принтера: для ABS, ASA, PC, PA — от запаха");
  b.number<F>("exhaust_print", "Во время печати", "%", &F::exhaust_print, 0, 100, 100);
  b.number<F>("exhaust_end", "После печати", "%", &F::exhaust_end, 0, 100, 100, "Остаётся включённым после печати");
  b.page("Откат");
  b.group("Откат для этого пластика");
  b.number<F>("retract_length", "Длина отката", "мм", &F::retract_length, -1, 10, 1, "−1 — как у принтера");
  b.number<F>("retract_speed", "Скорость отката", "мм/с", &F::retract_speed, -1, 150, 1, "−1 — как у принтера");
  b.page("G-code");
  b.group("G-code пластика");
  b.text<F>("start_gcode", "Стартовый код пластика", &F::start_gcode, FieldKind::Code,
            "После стартового кода принтера; подстановки — как в нём");
  b.text<F>("end_gcode", "Финальный код пластика", &F::end_gcode, FieldKind::Code, "Перед финальным кодом принтера");

  // ------------------------------------------------------------------ печать
  const std::string same = "0 — как ширина по умолчанию";
  b.page("Качество");
  b.group("Высота слоя");
  b.number<S>("layer_height", "Высота слоя", "мм", &S::layer_height, 0.04, 0.6);
  b.number<S>("first_layer_height", "Первый слой", "мм", &S::first_layer_height, 0.04, 0.6);
  b.group("Ширина линий");
  b.number<S>("line_width", "По умолчанию", "мм", &S::line_width, 0.2, 1.2);
  b.number<S>("first_layer_width", "Первый слой", "мм", &S::first_layer_width, 0, 1.2, 1,
              "Все линии первого слоя; 0 — как на остальных слоях");
  b.number<S>("outer_wall_width", "Наружная стенка", "мм", &S::outer_wall_width, 0, 1.2, 1, same).advanced = true;
  b.number<S>("inner_wall_width", "Внутренние стенки", "мм", &S::inner_wall_width, 0, 1.2, 1, same).advanced = true;
  b.number<S>("top_width", "Верх", "мм", &S::top_width, 0, 1.2, 1, same).advanced = true;
  b.number<S>("sparse_width", "Заполнение", "мм", &S::sparse_width, 0, 1.2, 1, same).advanced = true;
  b.number<S>("solid_width", "Сплошное заполнение", "мм", &S::solid_width, 0, 1.2, 1, same).advanced = true;
  b.number<S>("support_width", "Поддержки", "мм", &S::support_width, 0, 1.2, 1, same).advanced = true;
  b.group("Шов");
  b.choice<S, Seam>("seam", "Положение шва", &S::seam, {Seam::Aligned, Seam::Nearest, Seam::Rear, Seam::Random}, seam_key,
                    seam_name, seam_from_key, "Где начинается и кончается каждая стенка");
  b.number<S>("seam_gap", "Зазор шва", "% сопла", &S::seam_gap, 0, 100, 100,
              "Замкнутая стенка обрывается чуть раньше начала — шов без наплыва");
  b.group("Точность");
  b.number<S>("xy_hole_comp", "Поправка отверстий", "мм", &S::xy_hole_comp, -1, 1, 1,
              "Отверстия шире на столько (каждая сторона); обычно отверстия выходят уже модели")
      .advanced = true;
  b.number<S>("xy_contour_comp", "Поправка контура", "мм", &S::xy_contour_comp, -1, 1, 1,
              "Наружный контур шире на столько (каждая сторона); минус — уже")
      .advanced = true;
  b.number<S>("elephant_foot", "Поправка «слоновьей ноги»", "мм", &S::elephant_foot, 0, 1, 1,
              "Первый слой уже на столько: его придавливает к столу, и низ детали выходит шире");
  b.group("Глажка");
  b.choice<S, Ironing>("ironing", "Глажка", &S::ironing, {Ironing::None, Ironing::Top, Ironing::Topmost}, ironing_key,
                       ironing_name, ironing_from_key, "Сопло ещё раз проходит по верху с малым потоком — гладкая поверхность");
  b.number<S>("ironing_flow", "Поток глажки", "%", &S::ironing_flow, 0, 100, 100, "Доля пластика обычного слоя на той же площади");
  b.number<S>("ironing_spacing", "Шаг глажки", "мм", &S::ironing_spacing, 0.03, 1);
  b.number<S>("speed_ironing", "Скорость глажки", "мм/с", &S::speed_ironing, 5, 500);
  b.group("Стенки и поверхности");
  b.boolean<S>("outer_wall_first", "Сначала наружная стенка", &S::outer_wall_first,
               "Точнее наружные размеры, но хуже нависания");
  b.group("Мосты");
  b.number<S>("bridge_flow", "Поток на мостах", "%", &S::bridge_flow, 30, 200, 100,
              "Сплошные слои над пустотой; меньше 100 % — тоньше нить, меньше провисает");

  b.page("Прочность");
  b.group("Стенки");
  b.integer<S>("walls", "Стенок", "", &S::wall_loops, 1, 20);
  b.group("Верх и низ");
  b.choice<S, SurfacePattern>("top_pattern", "Рисунок верха", &S::top_pattern, surface_patterns(), surface_key, surface_name,
                              surface_from_key);
  b.integer<S>("top_layers", "Сплошных слоёв сверху", "", &S::top_layers, 0, 50);
  b.number<S>("top_thickness", "Толщина верха не меньше", "мм", &S::top_thickness, 0, 20, 1,
              "Слоёв сверху больше, если их общая толщина меньше этой; 0 — только по числу слоёв");
  b.choice<S, SurfacePattern>("bottom_pattern", "Рисунок низа", &S::bottom_pattern, surface_patterns(), surface_key,
                              surface_name, surface_from_key);
  b.integer<S>("bottom_layers", "Сплошных слоёв снизу", "", &S::bottom_layers, 0, 50);
  b.number<S>("bottom_thickness", "Толщина низа не меньше", "мм", &S::bottom_thickness, 0, 20, 1,
              "Слоёв снизу больше, если их общая толщина меньше этой; 0 — только по числу слоёв");
  b.choice<S, SurfacePattern>("solid_pattern", "Рисунок сплошного заполнения", &S::solid_pattern, surface_patterns(),
                              surface_key, surface_name, surface_from_key)
      .advanced = true;
  b.group("Заполнение");
  b.number<S>("infill", "Плотность", "%", &S::infill_density, 0, 100, 100);
  b.choice<S, Pattern>("pattern", "Рисунок", &S::pattern, patterns(), pattern_key, pattern_name, pattern_from_key);
  b.group("Дополнительно");
  b.number<S>("infill_angle", "Угол линий", "°", &S::infill_angle, -360, 360);
  b.boolean<S>("alternate_solid", "Сплошные слои поперёк через слой", &S::alternate_solid,
               "Иначе все сплошные линии — под углом заполнения")
      .advanced = true;
  b.number<S>("infill_overlap", "Заход заполнения на стенки", "%", &S::infill_overlap, 0, 50, 100).advanced = true;
  b.number<S>("min_sparse_area", "Порог сплошного заполнения", "мм²", &S::min_sparse_area, 0, 1000, 1,
              "Кусок заполнения меньше этой площади печатается сплошным — мелкие перемычки крепче")
      .advanced = true;

  b.page("Скорость");
  b.group("Первый слой");
  b.number<S>("speed_first_layer", "Первый слой", "мм/с", &S::speed_first_layer, 5, 500);
  b.number<S>("speed_first_layer_infill", "Заполнение первого слоя", "мм/с", &S::speed_first_layer_infill, 5, 500);
  b.number<S>("speed_first_layer_travel", "Перемещения первого слоя", "мм/с", &S::speed_first_layer_travel, 0, 2000, 1,
              "0 — как на остальных слоях");
  b.group("Остальные слои");
  b.number<S>("speed_outer_wall", "Наружная стенка", "мм/с", &S::speed_outer_wall, 5, 2000);
  b.number<S>("speed_inner_wall", "Внутренние стенки", "мм/с", &S::speed_inner_wall, 5, 2000);
  b.number<S>("small_perimeter_speed", "Мелкие периметры", "%", &S::small_perimeter_speed, 5, 100, 100,
              "Доля скорости наружной стенки — для коротких замкнутых стенок (обычно отверстия)");
  b.number<S>("small_perimeter_threshold", "Мелкий периметр — радиус до", "мм", &S::small_perimeter_threshold, 0, 100, 1,
              "Стенка короче окружности такого радиуса печатается медленнее; 0 — не замедлять")
      .advanced = true;
  b.number<S>("speed_sparse", "Заполнение", "мм/с", &S::speed_sparse, 5, 2000);
  b.number<S>("speed_solid", "Сплошное заполнение", "мм/с", &S::speed_solid, 5, 2000);
  b.number<S>("speed_top", "Верх", "мм/с", &S::speed_top, 5, 2000);
  b.number<S>("speed_support", "Поддержки", "мм/с", &S::speed_support, 5, 1000);
  b.number<S>("speed_support_interface", "Плотные слои поддержек", "мм/с", &S::speed_support_interface, 5, 1000);
  b.group("Нависания и мосты");
  b.boolean<S>("overhang_slowdown", "Замедлять на нависаниях", &S::overhang_slowdown,
               "Стенки над пустотой — со скоростью нависаний: меньше провисают");
  b.number<S>("speed_overhang", "Нависающие стенки", "мм/с", &S::speed_overhang, 5, 500);
  b.number<S>("speed_bridge", "Мосты", "мм/с", &S::speed_bridge, 5, 500);
  b.group("Перемещения");
  b.number<S>("speed_travel", "Перемещения", "мм/с", &S::speed_travel, 20, 2000);
  b.group("Ускорения");
  b.number<S>("accel_default", "Печать", "мм/с²", &S::accel_default, 100, 100000);
  b.number<S>("accel_travel", "Перемещения", "мм/с²", &S::accel_travel, 0, 100000, 1, "0 — как печать");
  b.number<S>("accel_first_layer", "Первый слой", "мм/с²", &S::accel_first_layer, 0, 100000, 1, "0 — как печать");
  b.number<S>("accel_outer_wall", "Наружная стенка", "мм/с²", &S::accel_outer_wall, 0, 100000, 1, "0 — как печать");
  b.number<S>("accel_inner_wall", "Внутренние стенки", "мм/с²", &S::accel_inner_wall, 0, 100000, 1, "0 — как печать").advanced =
      true;
  b.number<S>("accel_top", "Верх", "мм/с²", &S::accel_top, 0, 100000, 1, "0 — как печать").advanced = true;
  b.number<S>("accel_sparse", "Заполнение", "мм/с²", &S::accel_sparse, 0, 100000, 1, "0 — как печать").advanced = true;

  b.page("Поддержки");
  b.group("Поддержки");
  b.choice<S, Support>("support", "Поддержки", &S::support, {Support::None, Support::Normal, Support::Tree}, support_key,
                       support_name, support_from_key);
  b.number<S>("support_angle", "Нависание положе", "°", &S::support_angle, 5, 85, 1, "Угол к горизонтали: всё положе — с поддержкой");
  b.boolean<S>("support_on_bed_only", "Только от стола", &S::support_on_bed_only, "Поддержки не опираются на деталь");
  b.group("Дополнительно");
  b.number<S>("support_z_gap", "Зазор до детали по высоте", "мм", &S::support_z_gap, 0, 2);
  b.number<S>("support_xy_gap", "Зазор до стенок", "мм", &S::support_xy_gap, 0, 5);
  b.integer<S>("support_interface_layers", "Плотных слоёв под деталью", "", &S::support_interface_layers, 0, 10);
  b.number<S>("support_spacing", "Шаг линий", "мм", &S::support_spacing, 0.5, 10);
  b.number<S>("support_interface_spacing", "Шаг плотных линий", "мм", &S::support_interface_spacing, 0.2, 5);
  b.group("Подложка");
  b.integer<S>("raft_layers", "Слоёв подложки", "", &S::raft_layers, 0, 20,
               "Подложка (raft) под всей деталью: держит на столе пластики, склонные к отрыву углов; 0 — без неё");
  b.number<S>("raft_contact_distance", "Зазор до детали", "мм", &S::raft_contact_distance, 0, 1);
  b.number<S>("raft_expansion", "Подложка шире детали на", "мм", &S::raft_expansion, 0, 20);

  b.page("Прочее");
  b.group("Юбка");
  b.boolean<S>("skirt", "Юбка", &S::skirt, "Контур вокруг детали на первых слоях: прочищает сопло");
  b.integer<S>("skirt_loops", "Контуров юбки", "", &S::skirt_loops, 1, 10);
  b.integer<S>("skirt_layers", "Высота юбки", "слоёв", &S::skirt_layers, 1, 100);
  b.number<S>("skirt_distance", "Юбка от детали", "мм", &S::skirt_distance, 0, 50);
  b.group("Кайма");
  b.choice<S, BrimType>("brim_type", "Где кайма", &S::brim_type, {BrimType::Outer, BrimType::Inner, BrimType::Both, BrimType::None},
                        brim_key, brim_name, brim_from_key, "Внутри отверстий кайма держит тонкие стенки вокруг них");
  b.number<S>("brim_width", "Кайма", "мм", &S::brim_width, 0, 50, 1, "Полоса вокруг первого слоя, держит деталь на столе; 0 — без каймы");
  b.number<S>("brim_gap", "Зазор каймы", "мм", &S::brim_gap, 0, 5);
  b.group("Особые режимы");
  b.boolean<S>("spiral_vase", "Ваза (спираль)", &S::spiral_vase,
               "Одна наружная стенка спиралью без швов: сопло всё время поднимается. Без верха, заполнения и "
               "поддержек — для ваз и колпаков");
  b.group("Нечёткая поверхность");
  b.choice<S, FuzzySkin>("fuzzy_skin", "Нечёткая поверхность", &S::fuzzy_skin,
                         {FuzzySkin::None, FuzzySkin::Contour, FuzzySkin::External, FuzzySkin::All}, fuzzy_key, fuzzy_name,
                         fuzzy_from_key, "Стенка дрожит внутрь и наружу: шершавая поверхность, слоёв не видно (кроме первого слоя)");
  b.number<S>("fuzzy_thickness", "Толщина", "мм", &S::fuzzy_thickness, 0.01, 2, 1,
              "Полоса, в которой гуляет стенка; лучше не шире наружной стенки");
  b.number<S>("fuzzy_point_dist", "Шаг точек", "мм", &S::fuzzy_point_dist, 0.1, 10);
  b.group("G-code");
  b.boolean<S>("reduce_retraction", "Без отката внутри детали", &S::reduce_retraction,
               "Перемещение целиком над деталью — без отката: подтёк останется внутри");
  b.boolean<S>("label_objects", "Размечать детали (Klipper)", &S::label_objects,
               "EXCLUDE_OBJECT: деталь можно отменить во время печати, карта стола снимается только под ней");
  b.text<S>("pause_layers", "Пауза перед слоями", &S::pause_layers, FieldKind::Text,
            "Номера слоёв через запятую: принтер встанет на паузу перед ними (вложить гайку, сменить пластик). Удобно "
            "ставить во вкладке «Предпросмотр»")
      .check_text = [](std::string_view t) {
    std::string err;
    parse_layer_list(t, &err);
    return err;
  };
  b.text<S>("filename_format", "Имя файла", &S::filename_format, FieldKind::Text,
            "Подстановки: {input_filename_base}, {filament_type}, {layer_height}, {nozzle_diameter}, {printer_model}, "
            "{print_time}, {total_weight}, {timestamp}");
  b.group("Заметки");
  b.text<S>("notes", "Заметки", &S::notes, FieldKind::Code, "Для себя: пишутся в G-code комментарием");
  return v;
}

std::string fmt_number(double v) {
  std::string s = std::format("{:.6g}", v);
  for (char& c : s)
    if (c == '.') c = ',';
  return s;
}

}  // namespace

std::string_view section_key(Section s) {
  switch (s) {
    case Section::Printer: return "printer";
    case Section::Filament: return "filament";
    case Section::Process: return "process";
  }
  return "process";
}

std::string_view section_name(Section s) {
  switch (s) {
    case Section::Printer: return "Принтер";
    case Section::Filament: return "Пластик";
    case Section::Process: return "Печать";
  }
  return "Печать";
}

const std::vector<Field>& fields() {
  static const std::vector<Field> list = build();
  return list;
}

std::vector<std::string> pages(Section section) {
  std::vector<std::string> out;
  for (const auto& f : fields())
    if (f.section == section && (out.empty() || out.back() != f.page)) out.push_back(f.page);
  return out;
}

const Field* find_field(Section section, std::string_view key) {
  for (const auto& f : fields())
    if (f.section == section && f.key == key) return &f;
  return nullptr;
}

std::string check_field(const Field& f, const Value& v, std::string_view where) {
  const std::string name = std::string(where) + (where.empty() ? "" : ".") + f.key;
  switch (f.kind) {
    case FieldKind::Number:
    case FieldKind::Integer: {
      if (!v.is_number()) return name + ": ожидается число";
      const double x = v.as_double();
      if (!(x >= f.lo && x <= f.hi))
        return name + ": значение " + json::dump(v) + " вне допустимого (" + json::dump(Value(f.lo)) + "…" +
               json::dump(Value(f.hi)) + ")";
      if (f.kind == FieldKind::Integer && std::abs(x - std::round(x)) > 1e-9) return name + ": ожидается целое число";
      return {};
    }
    case FieldKind::Bool:
      if (!v.is_bool()) return name + ": ожидается true или false";
      return {};
    case FieldKind::Text:
    case FieldKind::Code:
      if (!v.is_string()) return name + ": ожидается строка";
      if (f.check_text)
        if (const auto err = f.check_text(v.as_string()); !err.empty()) return name + ": " + err;
      return {};
    case FieldKind::Choice: {
      if (!v.is_string()) return name + ": ожидается строка";
      if (f.canonical && !f.canonical(v.as_string()).empty()) return {};
      std::string known;
      for (const auto& c : f.choices) known += (known.empty() ? "" : ", ") + c.key;
      return name + ": неизвестное значение «" + v.as_string() + "», есть: " + known;
    }
  }
  return {};
}

Value section_to_json(const Settings& s, Section section, const Settings* base) {
  Value o = Value::object();
  for (const auto& f : fields()) {
    if (f.section != section) continue;
    Value v = f.get(s);
    if (base && json::dump(f.get(*base)) == json::dump(v)) continue;
    o[f.key] = std::move(v);
  }
  return o;
}

void section_from_json(Settings& s, Section section, const Value& o, std::string_view where,
                       const std::vector<std::string>& skip) {
  if (!o.is_object()) throw analysis::JobError(std::string(where) + ": ожидается объект {…}");
  for (const auto& [key, v] : o.as_object()) {
    if (v.is_null()) continue;
    bool skipped = false;
    for (const auto& k : skip) skipped = skipped || k == key;
    if (skipped) continue;
    const Field* f = find_field(section, key);
    if (!f) throw analysis::JobError(std::string(where) + ": неизвестная настройка «" + key + "»");
    if (const auto err = check_field(*f, v, where); !err.empty()) throw analysis::JobError(err);
    f->set(s, v);
  }
}

std::string field_text(const Field& f, const Settings& s) {
  const Value v = f.get(s);
  switch (f.kind) {
    case FieldKind::Number:
    case FieldKind::Integer: return fmt_number(v.as_double()) + (f.unit.empty() ? "" : " " + f.unit);
    case FieldKind::Bool: return v.as_bool() ? "да" : "нет";
    case FieldKind::Text:
    case FieldKind::Code: return v.as_string();
    case FieldKind::Choice:
      for (const auto& c : f.choices)
        if (c.key == v.as_string()) return c.name;
      return v.as_string();
  }
  return {};
}

}  // namespace kika::slicer
