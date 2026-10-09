#pragma once
// Материалы: ортотропные свойства напечатанного пластика и поправочные коэффициенты.
// Перенос fdmfea/materials.py.
//
// Оси валика: 1 — вдоль нити, 2 — поперёк нити в плоскости слоя, 3 — по Z.
// Нотация Фойгта [11, 22, 33, 23, 13, 12], сдвиги инженерные (γ = 2ε).
// Модули и прочности — МПа, плотность — г/см³, КЛТР — 1/°C, температуры — °C.

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "kika/linalg/dense.hpp"

namespace kika::material {

using linalg::Mat6;
using linalg::Vec6;

inline constexpr double kRefTemperature = 23.0;  // °C, при ней заданы свойства

struct Material {
  std::string key;
  std::string name;
  std::string note;
  double density = 0;
  double E1 = 0, E2 = 0, E3 = 0;
  double G12 = 0, G13 = 0, G23 = 0;
  double nu12 = 0, nu13 = 0, nu23 = 0;
  double Xt = 0, Xc = 0, Yt = 0, Yc = 0, Zt = 0, Zc = 0;
  double S12 = 0, S13 = 0, S23 = 0;
  double cte = 7e-5;
  double hdt = 60.0;
  double tg = 70.0;
  double creep_strength = 0.4;  // доля прочности при длительной нагрузке
  double creep_modulus = 0.55;  // доля модуля при длительной нагрузке
  double fatigue_k = 10.0;      // показатель кривой усталости S–N
  double elongation = 0.05;     // удлинение при разрыве
};

// Встроенная база (из prototype/fdmfea/materials.json).
const std::vector<Material>& builtin_materials();
const Material* find_material(std::string_view key);

// Материал из базы по типу пластика из G-code ("PETG", "PLA+", "PA6-CF"…).
std::optional<std::string> guess_material(std::string_view filament_type);

// Жёсткость ортотропного материала; исключение, если она не положительно определена.
Mat6 orthotropic_stiffness(double E1, double E2, double E3, double G12, double G13, double G23, double nu12,
                           double nu13, double nu23);
Mat6 stiffness(const Material& m);

// Коэффициент вертикальной жёсткости разреженного заполнения по названию рисунка.
double infill_kz(std::string_view pattern);

// Поворот вокруг Z: жёсткость материала с осью 1 под углом theta к X — в глобальных осях.
Mat6 rotate_z(const Mat6& c_local, double theta);

// Инженерная деформация в глобальных осях → в осях валика, повёрнутого на theta.
Mat6 strain_to_local(double theta);

struct TemperatureFactor {
  double factor = 1.0;
  std::optional<std::string> warning;
};
// Множитель модуля и прочности при температуре T, °C.
TemperatureFactor temperature_factor(const Material& m, std::optional<double> T);

// Множитель прочности при циклической нагрузке.
double fatigue_factor(const Material& m, double cycles);

// Коэффициенты критерия Хоффмана при прочностях, умноженных на f.
struct HoffmanCoeffs {
  double C1, C2, C3, C4, C5, C6, C7, C8, C9;
};
HoffmanCoeffs hoffman_coeffs(const Material& m, double f = 1.0);

// Запас прочности λ: критерий Хоффмана F(λ·σ) = 1; σ — в осях валика.
double hoffman_sf(const Vec6& sigma, const HoffmanCoeffs& hc);

// Виды разрушения
enum class FailureMode : int {
  FiberTension = 0,       // разрыв вдоль нити
  FiberCompression,       // смятие вдоль нити
  TransverseTension,      // отрыв соседних нитей в слое
  TransverseCompression,  // смятие поперёк нити
  InterlayerTension,      // расслоение между слоями (отрыв по Z)
  InterlayerCompression,  // смятие по Z
  InterlayerShear,        // межслойный сдвиг
  InPlaneShear,           // сдвиг в плоскости слоя
};
inline constexpr int kFailureModeCount = 8;
std::string_view failure_mode_name(FailureMode mode);

// Доминирующий вид разрушения: наибольшее отношение напряжения к прочности.
FailureMode failure_mode(const Vec6& sigma, const Material& m);

// Расчётная прочность и модуль образца с укладкой angles (градусы) при растяжении вдоль X
// (load_dir = 0) или по Z (load_dir = 2). Для сверки базы с паспортами производителей.
struct LaminateCheck {
  double strength;
  double modulus;
};
LaminateCheck laminate_check(const Material& m, const std::vector<double>& angles_deg, int load_dir = 0);

}  // namespace kika::material
