"""Прогон всех типов нагрузок на примере кронштейна (печать на боку)."""
import json, os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
import numpy as np
from fdmfea.analysis import Model, run

ROOT = os.path.dirname(HERE)
model = Model(gcode=os.path.join(ROOT, "examples", "bracket_side.gcode"), max_elems=25000)
print("элементов", model.vm.n, "воксель", round(model.vm.s, 3))
holes = [{"hole": {"axis": "x", "center": [12.0, 15.0], "radius": 2.5}},
         {"hole": {"axis": "x", "center": [28.0, 15.0], "radius": 2.5}}]
arm_top = {"box": {"min": [55, 40, 0], "max": [70, 50, 30]}, "normal": "+y"}
hang = {"hole": {"axis": "y", "center": [62.0, 15.0], "radius": 4.0}}
job = {
    "material": "PETG", "target_sf": 2.0,
    "fixtures": [{"where": holes}, {"where": {"side": "xmin"}, "components": "x"}],
    "cases": [
        {"name": "Сила 30 Н", "loads": [{"type": "force", "where": arm_top, "vector": [0, -30, 0]}]},
        {"name": "Сила по величине и направлению", "loads": [{"type": "force", "where": arm_top, "value": 30, "direction": "-y"}]},
        {"name": "Груз 1 кг на вынесенной точке", "loads": [{"type": "mass", "where": hang, "kg": 1, "direction": "-y", "point": [62, 20, 15]}]},
        {"name": "Давление 0,05 МПа", "loads": [{"type": "pressure", "where": {"side": "ymax"}, "value": 0.05}]},
        {"name": "Кручение 2 Н·м", "loads": [{"type": "moment", "where": {"side": "xmax"}, "axis": "+x", "value": 2000}]},
        {"name": "Болт 40 Н", "loads": [{"type": "bearing", "where": hang, "vector": [0, -40, 0]}]},
        {"name": "Удар 0,3 кг с 100 мм", "loads": [{"type": "impact", "where": arm_top, "kg": 0.3, "height": 100, "direction": "-y"}]},
        {"name": "Прогиб конца 1 мм", "loads": [{"type": "displacement", "where": {"side": "xmax"}, "vector": [None, -1.0, None]}]},
        {"name": "Перегрузка 20 g", "loads": [{"type": "gravity", "g": [0, -20, 0]}]},
        {"name": "Вибрация 10 Н, 10^6 циклов", "duration": "cyclic", "cycles": 1e6, "loads": [{"type": "force", "where": arm_top, "vector": [0, -10, 0]}]},
        {"name": "Нагрев 60 °C, длительно, зажат", "duration": "long", "temperature": 60, "thermal_expansion": True,
         "fixtures": [{"where": holes}, {"where": {"side": "xmin"}}, {"where": {"side": "xmax"}}],
         "loads": [{"type": "gravity", "g": [0, -1, 0]}]},
    ],
}
t = time.time()
a, res = run(model, job)
for r in res:
    s = r.summary
    print(f"{s['name']:34s} запас {s['sf']:8.2f} ({s['mode'][:24]:24s}) прогиб {s['max_disp']:8.4f} мм  "
          f"удар {s['impact_factor']}  предел {s['limit']['short'] if s['limit'] else '-':>12s}  реакция {np.round(s['reaction'], 2).tolist()}  приложено {np.round(s['applied'], 2).tolist()}")
    for w in s["warnings"]:
        print("      ⚠", w)
print("время", round(time.time() - t, 1), "с")
# проверки согласованности
S = {r.summary["name"]: r.summary for r in res}
assert abs(S["Сила 30 Н"]["sf"] - S["Сила по величине и направлению"]["sf"]) < 1e-6
assert abs(S["Болт 40 Н"]["reaction"][1] - 40) < 0.5
assert abs(S["Перегрузка 20 g"]["applied"][1] + 20 * 9.81e-3 * a.m["density"] * model.vm.stats["model_volume"] / 1000) < 0.05
print("проверки пройдены")
