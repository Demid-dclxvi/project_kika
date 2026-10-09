"""Создаёт примеры: настенный кронштейн для лампы, напечатанный двумя способами.

Кронштейн (в рабочем положении): пластина к стене 5×30×50 мм с двумя отверстиями под
саморезы Ø5, консоль 70×30×8 мм со скруглением R4 у пластины, на конце отверстие Ø8 для подвеса лампы.

  * bracket_side.gcode    — печать «на боку»: профиль L лежит в плоскости слоя (правильно)
  * bracket_upright.gcode — печать «стоя на пластине»: консоль растёт вверх по Z (слабо)
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import numpy as np
import trimesh
from mini_slicer import slice_mesh

OUT = os.path.join(os.path.dirname(HERE), "examples")


def bracket():
    plate = trimesh.creation.box((5, 30, 50))
    plate.apply_translation([2.5, 0, 25])
    arm = trimesh.creation.box((70, 30, 8))
    arm.apply_translation([35, 0, 46])
    # скругление R4 во внутреннем углу между пластиной и консолью
    blk = trimesh.creation.box((4, 30, 4))
    blk.apply_translation([7, 0, 40])
    cyl = trimesh.creation.cylinder(radius=4.0, height=40, sections=64)
    cyl.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [1, 0, 0]))
    cyl.apply_translation([9, 0, 38])
    fillet = blk.difference(cyl)
    body = trimesh.boolean.union([plate, arm, fillet])
    holes = []
    for z in (12.0, 28.0):
        c = trimesh.creation.cylinder(radius=2.5, height=20, sections=48)
        c.apply_transform(trimesh.transformations.rotation_matrix(np.pi / 2, [0, 1, 0]))
        c.apply_translation([2.5, 0, z])
        holes.append(c)
    hang = trimesh.creation.cylinder(radius=4.0, height=20, sections=48)
    hang.apply_translation([62, 0, 46])
    holes.append(hang)
    body = body.difference(trimesh.boolean.union(holes))
    return body


def main():
    os.makedirs(OUT, exist_ok=True)
    body = bracket()
    print("объём кронштейна, мм³:", round(body.volume, 1))
    # 1) на боку: рабочая ось Y (ширина) -> ось печати Z. Профиль XZ ложится в плоскость XY.
    side = body.copy()
    side.apply_transform(trimesh.transformations.rotation_matrix(-np.pi / 2, [1, 0, 0]))
    # после поворота: рабочая Z -> печать +Y, рабочая Y -> печать -Z
    txt, placed = slice_mesh(side, layer_h=0.2, first_h=0.2, w=0.45, walls=3, top=4, bottom=4,
                             infill=0.2, pattern="grid", flavor="orca", filament_type="PLA")
    open(os.path.join(OUT, "bracket_side.gcode"), "w").write(txt)
    # 2) стоя: задняя сторона пластины (рабочая X=0) на столе -> рабочая X = печать Z
    up = body.copy()
    up.apply_transform(trimesh.transformations.rotation_matrix(-np.pi / 2, [0, 1, 0]))
    txt2, placed2 = slice_mesh(up, layer_h=0.2, first_h=0.2, w=0.45, walls=3, top=4, bottom=4,
                               infill=0.2, pattern="grid", flavor="orca", filament_type="PLA")
    open(os.path.join(OUT, "bracket_upright.gcode"), "w").write(txt2)
    print("bounds side:", placed.bounds.round(2).tolist())
    print("bounds upright:", placed2.bounds.round(2).tolist())

    # Задания. Координаты — система детали (от минимального угла габарита).
    # На боку: печать X = рабочая X (0..70), печать Y = рабочая Z (0..50), печать Z = ширина (0..30)
    side_job = {
        "gcode": "bracket_side.gcode",
        "title": "Кронштейн для лампы",
        "subtitle": "Печать на боку: профиль кронштейна лежит в плоскости слоя, ширина 30 мм растёт по Z. "
                    "Крепится двумя саморезами к стене, лампа висит на отверстии на конце консоли.",
        "material": "PLA",
        "target_sf": 2.0,
        "fixtures": [
            {"name": "Саморезы", "where": [{"hole": {"axis": "x", "center": [12.0, 15.0], "radius": 2.5}},
                                          {"hole": {"axis": "x", "center": [28.0, 15.0], "radius": 2.5}}]},
            {"name": "Стена (упор)", "where": {"side": "xmin"}, "components": "x"},
        ],
        "cases": [
            {"name": "Лампа 2 кг", "duration": "long", "temperature": 30,
             "loads": [{"type": "bearing", "where": {"hole": {"axis": "y", "center": [62.0, 15.0], "radius": 4.0}},
                        "vector": [0, -19.62, 0]}]},
            {"name": "Рывок рукой 50 Н вниз", "duration": "short",
             "loads": [{"type": "force", "where": {"box": {"min": [60, 40, 0], "max": [70, 50, 30]}, "normal": "+y"},
                        "vector": [0, -50, 0]}]},
            {"name": "Боковой толчок 15 Н", "duration": "short",
             "loads": [{"type": "force", "where": {"side": "xmax"}, "vector": [0, 0, 15]}]},
        ],
    }
    # Стоя: печать Z = рабочая X (0..70), печать X = рабочая Z (0..50) (с поворотом), печать Y = ширина.
    lo2 = placed2.bounds[0]
    up_job = json.loads(json.dumps(side_job))
    up_job["gcode"] = "bracket_upright.gcode"
    up_job["title"] = "Кронштейн для лампы"
    up_job["subtitle"] = ("Печать стоя на пластине: консоль растёт вверх по Z, при изгибе слои работают на отрыв. "
                          "Тот же кронштейн и те же нагрузки, что при печати на боку.")
    # пересчёт: поворот -90° вокруг Y: (x,y,z) -> (-z, y, x); рабочая Z(0..50) -> печать X = 50 - z
    up_job["fixtures"] = [
        {"name": "Саморезы", "where": [{"hole": {"axis": "z", "center": [50 - 12.0, 15.0], "radius": 2.5}},
                                      {"hole": {"axis": "z", "center": [50 - 28.0, 15.0], "radius": 2.5}}]},
        {"name": "Стена (упор)", "where": {"side": "zmin"}, "components": "z"},
    ]
    up_job["cases"][0]["loads"][0]["where"] = {"hole": {"axis": "x", "center": [15.0, 62.0], "radius": 4.0}}
    up_job["cases"][0]["loads"][0]["vector"] = [19.62, 0, 0]
    up_job["cases"][1]["loads"][0]["where"] = {"box": {"min": [0, 0, 60], "max": [10, 30, 70]}, "normal": "-x"}
    up_job["cases"][1]["loads"][0]["vector"] = [50, 0, 0]
    up_job["cases"][2]["loads"][0]["where"] = {"side": "zmax"}
    up_job["cases"][2]["loads"][0]["vector"] = [0, 15, 0]
    json.dump(side_job, open(os.path.join(OUT, "bracket_side.job.json"), "w"), ensure_ascii=False, indent=2)
    json.dump(up_job, open(os.path.join(OUT, "bracket_upright.job.json"), "w"), ensure_ascii=False, indent=2)
    print("готово")


if __name__ == "__main__":
    main()
