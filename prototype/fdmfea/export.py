"""Подготовка данных для 3D-просмотра и HTML-отчёта."""
from __future__ import annotations

import base64
import datetime as _dt
import gzip
import json
import os

import numpy as np

from .gcode import ROLE_OUTER_WALL, ROLE_INNER_WALL, ROLE_SOLID, ROLE_SPARSE

WEB = os.path.join(os.path.dirname(__file__), "web")
VERDICT = {"ok": "Выдержит", "risk": "Мало запаса", "fail": "Разрушится"}


def b64(a: np.ndarray) -> str:
    return base64.b64encode(np.ascontiguousarray(a).tobytes()).decode("ascii")


def _round(v, n=4):
    if isinstance(v, (bool, np.bool_)):
        return bool(v)
    if isinstance(v, (float, np.floating)):
        return round(float(v), n)
    if isinstance(v, (np.integer,)):
        return int(v)
    if isinstance(v, dict):
        return {k: _round(x, n) for k, x in v.items()}
    if isinstance(v, (list, tuple)):
        return [_round(x, n) for x in v]
    return v


def model_payload(model) -> dict:
    vm, mesh = model.vm, model.mesh
    elems = np.stack([vm.ix, vm.iy, vm.iz], 1).astype(np.int16)
    role = np.full(vm.n, 2, np.uint8)
    role[(vm.role == ROLE_OUTER_WALL) | (vm.role == ROLE_INNER_WALL)] = 0
    role[vm.role == ROLE_SOLID] = 1
    role[vm.role == ROLE_SPARSE] = 2
    rho = np.clip(np.round((vm.rho_shell + vm.rho_sparse) * 255), 0, 255).astype(np.uint8)
    return dict(
        grid=dict(nx=int(vm.nx), ny=int(vm.ny), nz=int(vm.nz), sx=float(vm.sx), sy=float(vm.sy),
                  x0=float(vm.x0), y0=float(vm.y0), z_edges=[float(z) for z in vm.z_edges]),
        origin=[float(v) for v in model.origin],
        size=[float(v) for v in model.size],
        n_elems=int(vm.n), n_nodes=int(mesh.n_nodes),
        elems=b64(elems), role=b64(role), rho=b64(rho),
        node_flat=b64(mesh.node_flat.astype(np.int32)),
        summary=_round(model.summary()),
    )


def results_payload(analysis, results) -> dict:
    out = []
    for r in results:
        sel = {k: [int(x) for x in analysis.model.mesh.face_key[v]] for k, v in r.sel_faces.items()}
        out.append(dict(
            summary=_round(r.summary),
            sf=b64(np.minimum(r.sf, 1e4).astype(np.float32)),
            vm=b64(r.vm.astype(np.float32)),
            mode=b64(r.mode.astype(np.uint8)),
            u=b64(r.u.astype(np.float32)),
            sel=sel,
        ))
    m = analysis.m
    return dict(results=out, material=_round({k: m[k] for k in m if not isinstance(m[k], (list, dict))}),
                target_sf=analysis.target_sf, total_time=round(getattr(analysis, "total_time", 0.0), 2),
                kz=analysis.mf.kz)


def _read(name):
    with open(os.path.join(WEB, name), "r", encoding="utf-8") as f:
        return f.read()


def _esc(s):
    return (str(s).replace("&", "&amp;").replace("<", "&lt;").replace(">", "&gt;").replace('"', "&quot;"))


def _f(v, d=None):
    if v is None:
        return "—"
    v = float(v)
    if d is None:
        a = abs(v)
        d = 0 if a >= 100 else 1 if a >= 10 else 2
    return f"{v:.{d}f}".replace(".", ",")


def _int(v):
    return f"{int(v):,}".replace(",", " ")


def report_html(model, analysis, results, job: dict, title: str | None = None,
                gcode_name: str = "", full: bool = True) -> str:
    """Самодостаточный HTML-отчёт: сводка + интерактивный 3D-просмотр результатов.

    full=False — фрагмент без <!doctype>/<html> (для публикации как веб-страницы)."""
    title = title or job.get("title") or "Расчёт детали"
    date = _dt.datetime.now().strftime("%d.%m.%Y %H:%M")
    payload = dict(model=model_payload(model), results=results_payload(analysis, results), job=job,
                   meta=dict(title=title, gcode=gcode_name, date=date,
                             version=__import__("fdmfea").__version__))
    raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    packed = base64.b64encode(gzip.compress(raw, 6)).decode("ascii")
    html = (_read("report.html")
            .replace("__TITLE__", _esc(title))
            .replace("__HEADER__", _header_html(model, analysis, title, gcode_name, date, job.get("subtitle")))
            .replace("__CASES__", _cases_html(results))
            .replace("__DETAILS__", _details_html(model, analysis))
            .replace("__PAYLOAD__", packed))
    html = (html.replace("/*__VIEWER_CSS__*/", _read("viewer.css"))
                .replace("/*__VIEWER_JS__*/", _read("viewer.js"))
                .replace("/*__REPORT_JS__*/", _read("report.js"))
                .replace("/*__THREE__*/", _read("vendor/three.min.js")))
    if full:
        head, sep, rest = html.partition('<div class="page">')
        html = ('<!doctype html>\n<html lang="ru">\n<head>\n<meta charset="utf-8">\n'
                '<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">\n'
                + head + '</head>\n<body>\n' + sep + rest + '\n</body>\n</html>\n')
    return html


def _header_html(model, analysis, title, gcode_name, date, subtitle=None):
    sm = model.summary()
    m = analysis.m
    mass = sm["volume_mm3"] * m["density"] / 1000.0
    sz = sm["size"]
    pat = sm.get("infill_pattern") or ""
    chips = [
        ("Материал", m["name"]),
        ("Слой", f"{_f(sm['layer_height'], 2)} мм"),
        ("Заполнение", f"{round(sm['infill_density'] * 100):d}%" + (f" {pat}" if pat else "")),
    ]
    if sm.get("wall_loops"):
        chips.append(("Стенки", str(sm["wall_loops"])))
    chips += [
        ("Габарит", f"{_f(sz[0], 1)} × {_f(sz[1], 1)} × {_f(sz[2], 1)} мм"),
        ("Масса", f"{_f(mass, 1)} г"),
        ("Сетка", f"{_f(sm['voxel'], 2)} мм · {_int(sm['elements'])} эл."),
    ]
    if sm.get("slicer") and sm["slicer"] != "unknown":
        chips.append(("Слайсер", sm["slicer"]))
    spec = "".join(f'<span class="spec"><span>{_esc(k)}</span><b>{_esc(v)}</b></span>' for k, v in chips)
    src = f" · {_esc(gcode_name)}" if gcode_name else ""
    return (f'<header class="hdr"><div class="eyebrow">Расчёт прочности FDM-детали · {date}{src}</div>'
            f'<h1>{_esc(title)}</h1>' + (f'<p class="sub">{_esc(subtitle)}</p>' if subtitle else '') +
            f'<div class="specs">{spec}</div></header>')


def _cycles(n):
    if not n:
        return ""
    p = int(np.floor(np.log10(n)))
    mant = n / 10 ** p
    return (f"{_f(mant, 1)}·" if abs(mant - 1) > 1e-6 else "") + f"10<sup>{p}</sup> циклов"


def _cases_html(results):
    out = []
    for i, r in enumerate(results):
        s = r.summary
        v = s["verdict"]
        kind = [_esc(s["duration_name"])]
        if s.get("cycles"):
            kind.append(_cycles(s["cycles"]))
        if s.get("temperature") is not None:
            kind.append(f"{_f(s['temperature'], 0)} °C")
        rows = [("Разрушение", _esc(s["mode"])),
                ("Где", '<span class="num">' + " · ".join(_f(c, 1) for c in s["sf_xyz"]) + " мм</span>"),
                ("Прогиб", f'<span class="num">{_f(s["max_disp"], 2)} мм</span>')]
        if s.get("limit"):
            rows.append(("Выдержит до", _esc(s["limit"].get("short") or s["limit"]["text"])))
        if s.get("impact_factor"):
            rows.append(("Удар", f'коэффициент <span class="num">{_f(s["impact_factor"], 2)}</span>'))
        if s.get("disp_force"):
            rows.append(("Усилие", f'<span class="num">{_f(float(np.linalg.norm(s["disp_force"])), 1)} Н</span>'))
        kv = "".join(f"<dt>{k}</dt><dd>{v_}</dd>" for k, v_ in rows)
        warns = list(s.get("warnings") or [])
        if s.get("sf_min_at_bc"):
            warns.append(f"У места закрепления или приложения нагрузки есть локальный пик (запас {_f(s['sf_min'], 2)}). "
                         f"Обычно это особенность расчётной модели; проверьте конструкцию узла.")
        w = "".join(f'<div class="warn">{_esc(x)}</div>' for x in warns)
        on = i == 0
        out.append(
            f'<button type="button" class="case{" on" if on else ""}" aria-pressed="{"true" if on else "false"}">'
            f'<div class="case-top"><div><h3>{_esc(s["name"])}</h3><div class="kind">{" · ".join(kind)}</div></div>'
            f'<span class="verdict {v}">{VERDICT[v]}</span></div>'
            f'<div class="sfrow"><span class="sfbig {v}">{_f(min(s["sf"], 999), 2)}</span>'
            f'<span class="sflabel">запас прочности<br>нужно не меньше {_f(s["target_sf"], 1)}</span></div>'
            f'<dl class="kv">{kv}</dl>{w}</button>')
    return "\n".join(out)


def _details_html(model, analysis):
    m = analysis.m
    rows = [
        ("Модуль упругости, МПа", m["E1"], m["E2"], m["E3"]),
        ("Прочность на растяжение, МПа", m["Xt"], m["Yt"], m["Zt"]),
        ("Прочность на сжатие, МПа", m["Xc"], m["Yc"], m["Zc"]),
    ]
    tb = "".join(f"<tr><td>{a}</td><td>{_f(b, 0)}</td><td>{_f(c, 0)}</td><td>{_f(d, 0)}</td></tr>" for a, b, c, d in rows)
    tb += (f"<tr><td>Прочность на сдвиг, МПа</td><td>—</td><td>{_f(m['S12'], 0)}</td>"
           f"<td>{_f(m['S13'], 0)}</td></tr>")
    note = _esc(m.get("note") or "")
    return f'''<section class="details">
<div>
<h2>Как читать результат</h2>
<ul>
<li><b>Запас прочности</b> показывает, во сколько раз можно увеличить нагрузку до разрушения. Меньше 1 — деталь сломается. Для обычных деталей нужно 2 и больше, для ответственных — 3.</li>
<li><b>Что разрушится первым</b>: разрыв нити, отрыв соседних нитей в слое или расслоение между слоями. Расслоение — типичная слабость печати: по Z пластик обычно в 1,5–2 раза слабее.</li>
<li>Длительная нагрузка учитывает ползучесть пластика, циклическая — усталость, температура — размягчение.</li>
<li>Острые внутренние углы на воксельной сетке дают завышенные пики. Если минимум запаса в таком углу, добавьте скругление и пересчитайте.</li>
</ul>
<h2 style="margin-top:18px">Допущения</h2>
<ul>
<li>Линейная упругость и малые перемещения. Контакт, трение и потеря устойчивости не считаются.</li>
<li>Свойства материала типовые для хорошей печати сухим пластиком. Реальная прочность между слоями зависит от температуры, обдува и скорости — для ответственных деталей замените значения своими испытаниями.</li>
<li>Стенки, верх и низ учитываются по реальным траекториям из G-code; разреженное заполнение усредняется по ячейке рисунка.</li>
</ul>
</div>
<div>
<h2>Материал: {_esc(m["name"])}</h2>
<div class="tbl-wrap"><table class="props"><thead><tr><th></th><th>вдоль нити</th><th>поперёк нити</th><th>между слоями</th></tr></thead><tbody>{tb}</tbody></table></div>
<p class="foot">{note} Плотность {_f(m["density"], 2)} г/см³ · теплостойкость (HDT) {_f(m["hdt"], 0)} °C · длительная прочность {round(m["creep_strength"] * 100)}% от кратковременной.<br>
Расчётная сетка: {_int(model.vm.n)} элементов, {_int(model.mesh.n_nodes * 3)} неизвестных · время расчёта {_f(getattr(analysis, "total_time", 0), 1)} с · fdmfea {__import__("fdmfea").__version__}</p>
</div>
</section>'''


def text_summary(analysis, results) -> str:
    lines = []
    m = analysis.m
    lines.append(f"Материал: {m['name']}   требуемый запас: {analysis.target_sf:g}")
    for r in results:
        s = r.summary
        verdict = {"ok": "ВЫДЕРЖИТ", "risk": "МАЛО ЗАПАСА", "fail": "РАЗРУШИТСЯ"}[s["verdict"]]
        lines.append(f"\n▶ {s['name']}  [{s['duration_name']}]")
        lines.append(f"   Итог: {verdict}   запас прочности {s['sf']:.2f}  (в точке {s['sf_xyz']} мм)")
        lines.append(f"   Вид разрушения: {s['mode']}")
        lines.append(f"   Наибольшее перемещение: {s['max_disp']:.3f} мм")
        if s.get("limit"):
            lines.append(f"   {s['limit']['text']}")
        if s.get("impact_factor"):
            lines.append(f"   Коэффициент удара: {s['impact_factor']:.2f}")
        if s.get("disp_force"):
            lines.append(f"   Усилие для заданного перемещения: {float(np.linalg.norm(s['disp_force'])):.1f} Н")
        if s["sf_min_at_bc"]:
            lines.append(f"   Прим.: у места закрепления/нагрузки локальный пик {s['sf_min']:.2f} — "
                         f"обычно это особенность модели, проверьте конструкцию узла.")
        for w in s["warnings"]:
            lines.append(f"   ⚠ {w}")
    return "\n".join(lines)
