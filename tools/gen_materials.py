"""Генерирует src/core/src/material/builtin.cpp из prototype/fdmfea/materials.json.

Ядро не читает JSON само (у него нет внешних зависимостей), поэтому встроенная база
материалов — это таблица в коде. Запускать после изменения materials.json:

    python tools/gen_materials.py
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "prototype", "fdmfea", "materials.json")
DST = os.path.join(ROOT, "src", "core", "src", "material", "builtin.cpp")

NUMERIC = ["density", "E1", "E2", "E3", "G12", "G13", "G23", "nu12", "nu13", "nu23",
           "Xt", "Xc", "Yt", "Yc", "Zt", "Zc", "S12", "S13", "S23",
           "cte", "hdt", "tg", "creep_strength", "creep_modulus", "fatigue_k", "elongation"]
DEFAULTS = {"cte": 7e-5, "hdt": 60.0, "tg": 70.0, "creep_strength": 0.4, "creep_modulus": 0.55,
            "fatigue_k": 10.0, "elongation": 0.05, "note": ""}


def cstr(s: str) -> str:
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    with open(SRC, encoding="utf-8") as f:
        db = json.load(f)
    readme = db.pop("_readme", "")
    out = ["// Файл создан tools/gen_materials.py из prototype/fdmfea/materials.json — не редактировать.",
           "// " + readme[:200].replace("\n", " ") + "…",
           "",
           '#include "kika/material/material.hpp"',
           "",
           "namespace kika::material {",
           "",
           "const std::vector<Material>& builtin_materials() {",
           "  static const std::vector<Material> db = {"]
    for key, m in db.items():
        for k, v in DEFAULTS.items():
            m.setdefault(k, v)
        nums = ", ".join(repr(float(m[k])) for k in NUMERIC)
        out.append(f"      Material{{{cstr(key)}, {cstr(m.get('name', key))}, {cstr(m['note'])}, {nums}}},")
    out += ["  };", "  return db;", "}", "", "}  // namespace kika::material", ""]
    with open(DST, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out))
    print(f"{len(db)} материалов → {os.path.relpath(DST, ROOT)}")


if __name__ == "__main__":
    main()
