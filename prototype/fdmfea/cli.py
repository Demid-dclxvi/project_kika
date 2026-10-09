"""Командная строка.

  python -m fdmfea gui                       — открыть приложение в браузере
  python -m fdmfea run деталь.gcode задание.json [-o отчёт.html]
  python -m fdmfea info деталь.gcode         — что извлечено из G-code
  python -m fdmfea materials                 — список материалов
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time


def _load_job(path):
    with open(path, "r", encoding="utf-8") as f:
        if path.lower().endswith((".yaml", ".yml")):
            try:
                import yaml  # type: ignore
            except ImportError:
                sys.exit("Для заданий в YAML установите пакет: python -m pip install pyyaml (или сохраните задание в JSON).")
            return yaml.safe_load(f)
        return json.load(f)


def _progress_printer():
    last = [0.0, ""]

    def p(stage, frac, text):
        now = time.time()
        if text != last[1] and (now - last[0] > 0.4 or frac >= 1.0):
            print(f"  [{frac * 100:5.1f}%] {text}", flush=True)
            last[0], last[1] = now, text
    return p


def cmd_run(a):
    from .analysis import Model, run
    from .export import report_html, text_summary
    job = _load_job(a.job)
    gpath = a.gcode or job.get("gcode")
    if not gpath:
        sys.exit("Укажите файл G-code.")
    if not os.path.isabs(gpath) and not os.path.exists(gpath) and a.job:
        cand = os.path.join(os.path.dirname(os.path.abspath(a.job)), gpath)
        if os.path.exists(cand):
            gpath = cand
    voxel = a.voxel if a.voxel else job.get("voxel")
    max_el = a.max_elems or job.get("max_elems") or 150_000
    print(f"G-code: {gpath}")
    t = time.time()
    model = Model(gcode=gpath, voxel=voxel, max_elems=max_el, progress=_progress_printer())
    sm = model.summary()
    print(f"  слайсер {sm['slicer']}, пластик {sm['filament_type'] or '?'}, слой {sm['layer_height']} мм, "
          f"заполнение {sm['infill_density'] * 100:.0f}% {sm['infill_pattern']}, габарит {sm['size']} мм")
    print(f"  сетка: воксель {sm['voxel']} мм, {sm['elements']} элементов, {sm['nodes']} узлов")
    for w in sm["warnings"]:
        print("  ⚠", w)
    analysis, results = run(model, job, progress=_progress_printer())
    print()
    print(text_summary(analysis, results))
    out = a.out or os.path.splitext(os.path.basename(gpath))[0] + "_отчёт.html"
    html = report_html(model, analysis, results, job, title=job.get("title"), gcode_name=os.path.basename(gpath))
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    if a.json:
        with open(a.json, "w", encoding="utf-8") as f:
            json.dump([r.summary for r in results], f, ensure_ascii=False, indent=2)
    print(f"\nОтчёт: {os.path.abspath(out)}   (всего {time.time() - t:.1f} с)")


def cmd_info(a):
    from .analysis import Model
    model = Model(gcode=a.gcode, voxel=a.voxel, max_elems=a.max_elems or 150_000)
    sm = model.summary()
    print(json.dumps(sm, ensure_ascii=False, indent=2))
    print("\nСистема координат задания: начало — минимальный угол габарита детали, оси как у принтера.")
    print(f"Деталь занимает X 0…{sm['size'][0]:.1f}, Y 0…{sm['size'][1]:.1f}, Z 0…{sm['size'][2]:.1f} мм.")


def cmd_materials(a):
    from .materials import load_db
    db = load_db()
    for k, m in db.items():
        print(f"{k:8s} {m['name']:40s} E={m['E1']:.0f}/{m['E2']:.0f}/{m['E3']:.0f} МПа  "
              f"прочность {m['Xt']:.0f}/{m['Yt']:.0f}/{m['Zt']:.0f} МПа  HDT {m['hdt']:.0f} °C")


def cmd_gui(a):
    from .server import serve
    serve(port=a.port, open_browser=not a.no_browser, host=a.host)


def main(argv=None):
    ap = argparse.ArgumentParser(prog="fdmfea", description="Расчёт прочности FDM-деталей по G-code")
    sub = ap.add_subparsers(dest="cmd")
    g = sub.add_parser("gui", help="открыть приложение в браузере")
    g.add_argument("--port", type=int, default=8765)
    g.add_argument("--host", default="127.0.0.1")
    g.add_argument("--no-browser", action="store_true")
    r = sub.add_parser("run", help="расчёт по файлу задания")
    r.add_argument("gcode", nargs="?")
    r.add_argument("job")
    r.add_argument("-o", "--out")
    r.add_argument("--json")
    r.add_argument("--voxel", type=float)
    r.add_argument("--max-elems", type=int)
    i = sub.add_parser("info", help="что извлечено из G-code")
    i.add_argument("gcode")
    i.add_argument("--voxel", type=float)
    i.add_argument("--max-elems", type=int)
    sub.add_parser("materials", help="список материалов")
    a = ap.parse_args(argv)
    if a.cmd is None:
        a = ap.parse_args(["gui"] + (argv or sys.argv[1:]))
    {"run": cmd_run, "info": cmd_info, "materials": cmd_materials, "gui": cmd_gui}[a.cmd](a)


if __name__ == "__main__":
    main()
