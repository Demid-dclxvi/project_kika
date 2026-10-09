"""Разбор G-code: из команд принтера получаем список отрезков экструзии.

Поддерживаются OrcaSlicer, Bambu Studio, PrusaSlicer, SuperSlicer, Cura,
Simplify3D (по комментариям ;TYPE: / ; FEATURE: / ;LAYER_CHANGE и т.п.).
Учитываются абсолютная/относительная экструзия (M82/M83, G90/G91, G92),
дуги G2/G3, дюймы (G20), отсечение стартового и финального G-code,
поддержек, юбки, каймы, черновой башни.
"""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field

import numpy as np

# Роли отрезков
ROLE_OUTER_WALL = 1
ROLE_INNER_WALL = 2
ROLE_SOLID = 3        # сплошное заполнение, верх/низ, мосты, заполнение щелей
ROLE_SPARSE = 4       # разреженное заполнение
ROLE_UNKNOWN = 5      # тип не указан, считаем сплошным материалом
ROLE_EXCLUDED = 0     # поддержки, юбка, кайма, башня, стартовый код

ROLE_NAMES = {
    ROLE_OUTER_WALL: "Внешняя стенка",
    ROLE_INNER_WALL: "Внутренняя стенка",
    ROLE_SOLID: "Сплошное заполнение / верх / низ",
    ROLE_SPARSE: "Разреженное заполнение",
    ROLE_UNKNOWN: "Без типа",
}

_EXCLUDE_KEYS = ("support", "skirt", "brim", "tower", "custom", "ironing",
                 "purge", "flush", "prime", "wipe", "undefined")


def classify_role(type_str: str) -> int:
    s = type_str.strip().lower()
    if not s:
        return ROLE_UNKNOWN
    if any(k in s for k in _EXCLUDE_KEYS):
        return ROLE_EXCLUDED
    if "outer" in s or "external" in s:
        return ROLE_OUTER_WALL
    if "wall" in s or "perimeter" in s or s.startswith("inner"):
        return ROLE_INNER_WALL
    if "sparse" in s or s in ("fill", "infill", "internal infill") or (
            "infill" in s and "solid" not in s and "bridge" not in s and "gap" not in s):
        return ROLE_SPARSE
    if any(k in s for k in ("top", "bottom", "skin", "solid", "bridge", "gap")):
        return ROLE_SOLID
    return ROLE_UNKNOWN


@dataclass
class GcodeInfo:
    slicer: str = "unknown"
    filament_diameter: float = 1.75
    filament_type: str = ""
    filament_density: float | None = None
    layer_height: float | None = None
    first_layer_height: float | None = None
    nozzle_diameter: float | None = None
    line_width: float | None = None
    infill_density: float | None = None     # доля 0..1
    infill_pattern: str = ""
    wall_loops: int | None = None
    top_layers: int | None = None
    bottom_layers: int | None = None
    settings: dict = field(default_factory=dict)
    warnings: list = field(default_factory=list)
    n_lines: int = 0
    n_layers: int = 0
    filament_used_mm: float = 0.0
    excluded_volume: float = 0.0

    def to_dict(self):
        d = {k: v for k, v in self.__dict__.items() if k != "settings"}
        return d


@dataclass
class Toolpaths:
    """Отрезки экструзии (только конструкционные, без поддержек и т.п.)."""
    x0: np.ndarray
    y0: np.ndarray
    x1: np.ndarray
    y1: np.ndarray
    z: np.ndarray       # Z верха валика (высота сопла при печати)
    h: np.ndarray       # толщина слоя для этого отрезка
    vol: np.ndarray     # объём выдавленного пластика, мм³
    role: np.ndarray    # код роли
    layer: np.ndarray   # номер слоя (порядковый)
    info: GcodeInfo

    @property
    def n(self):
        return len(self.x0)

    def total_volume(self):
        return float(self.vol.sum())

    def bbox(self):
        return (np.array([min(self.x0.min(), self.x1.min()), min(self.y0.min(), self.y1.min()),
                          float((self.z - self.h).min())]),
                np.array([max(self.x0.max(), self.x1.max()), max(self.y0.max(), self.y1.max()),
                          float(self.z.max())]))


_num_re = re.compile(r"([A-Za-z])\s*([-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?)")

# ключи настроек в комментариях разных слайсеров
_SETTING_KEYS = {
    "filament_diameter": "filament_diameter",
    "filament_type": "filament_type",
    "filament_density": "filament_density",
    "layer_height": "layer_height",
    "first_layer_height": "first_layer_height",
    "initial_layer_print_height": "first_layer_height",
    "nozzle_diameter": "nozzle_diameter",
    "line_width": "line_width",
    "extrusion_width": "line_width",
    "sparse_infill_density": "infill_density",
    "fill_density": "infill_density",
    "infill_sparse_density": "infill_density",
    "sparse_infill_pattern": "infill_pattern",
    "fill_pattern": "infill_pattern",
    "infill_pattern": "infill_pattern",
    "wall_loops": "wall_loops",
    "perimeters": "wall_loops",
    "wall_line_count": "wall_loops",
    "top_shell_layers": "top_layers",
    "top_solid_layers": "top_layers",
    "bottom_shell_layers": "bottom_layers",
    "bottom_solid_layers": "bottom_layers",
}


def _first_value(v: str):
    v = v.strip().strip('"')
    if "," in v:
        v = v.split(",")[0]
    if ";" in v:
        v = v.split(";")[0]
    return v.strip()


def _parse_setting_comment(c: str, info: GcodeInfo):
    # форматы: "key = value" (Orca/Prusa/Bambu), "Layer height: 0.2" (Cura)
    if "=" in c:
        k, v = c.split("=", 1)
    elif ":" in c:
        k, v = c.split(":", 1)
    else:
        return
    k = k.strip().lower().replace(" ", "_")
    if k not in _SETTING_KEYS:
        return
    info.settings[k] = v.strip()
    attr = _SETTING_KEYS[k]
    val = _first_value(v)
    try:
        if attr in ("filament_type", "infill_pattern"):
            if not getattr(info, attr):
                setattr(info, attr, val)
        elif attr == "infill_density":
            if val.endswith("%"):
                d = float(val[:-1]) / 100.0
            else:
                d = float(val)
                if d > 1.0:
                    d /= 100.0
            info.infill_density = d
        elif attr in ("wall_loops", "top_layers", "bottom_layers"):
            setattr(info, attr, int(float(val)))
        else:
            fv = float(val.rstrip("%"))
            if attr == "line_width" and val.endswith("%"):
                return
            if attr == "filament_diameter" and getattr(info, "_fd_set", False):
                return
            setattr(info, attr, fv)
            if attr == "filament_diameter":
                info._fd_set = True
    except ValueError:
        pass


def _detect_slicer(text_head: str) -> str:
    t = text_head.lower()
    for key, name in (("orcaslicer", "OrcaSlicer"), ("bambustudio", "Bambu Studio"),
                      ("bambu studio", "Bambu Studio"), ("superslicer", "SuperSlicer"),
                      ("prusaslicer", "PrusaSlicer"), ("cura", "Cura"),
                      ("simplify3d", "Simplify3D"), ("ideamaker", "ideaMaker"),
                      ("creality print", "Creality Print"), ("slic3r", "Slic3r")):
        if key in t:
            return name
    return "unknown"


def parse_gcode(text: str, arc_seg_len: float = 0.5) -> Toolpaths:
    info = GcodeInfo()
    info.slicer = _detect_slicer(text[:20000] + text[-20000:])

    lines = text.splitlines()
    info.n_lines = len(lines)

    # Сначала проход по комментариям-настройкам (обычно в начале/конце файла)
    for ln in lines:
        if ln.startswith(";"):
            c = ln[1:].strip()
            if c and ("=" in c or ":" in c) and len(c) < 400:
                _parse_setting_comment(c, info)
    fil_area = math.pi * (info.filament_diameter / 2.0) ** 2

    # Есть ли маркеры слоёв? Если да — всё до первого маркера = стартовый код.
    layer_markers = (";LAYER_CHANGE", "; CHANGE_LAYER", ";LAYER:", "; layer ")
    has_layer_markers = any(ln.startswith(layer_markers) for ln in lines)

    # состояние
    x = y = z = 0.0
    e = 0.0
    abs_xyz = True
    abs_e = True
    e_mode_explicit = False
    scale = 1.0
    started = not has_layer_markers
    finished = False
    cur_role = ROLE_UNKNOWN
    cur_height = None
    layer_idx = -1
    in_wipe = False

    X0, Y0, X1, Y1, Z, H, V, R, L = [], [], [], [], [], [], [], [], []
    excluded_vol = 0.0
    used = 0.0

    def emit(xa, ya, xb, yb, zz, dvol):
        nonlocal excluded_vol
        if not started or finished or cur_role == ROLE_EXCLUDED or in_wipe:
            excluded_vol += dvol
            return
        X0.append(xa); Y0.append(ya); X1.append(xb); Y1.append(yb)
        Z.append(zz); H.append(cur_height if cur_height else -1.0)
        V.append(dvol); R.append(cur_role); L.append(layer_idx)

    for raw in lines:
        if not raw:
            continue
        # комментарии
        sc = raw.find(";")
        if sc >= 0:
            comment = raw[sc + 1:]
            code = raw[:sc]
            cs = comment.strip()
            if cs:
                up = cs.upper()
                if up.startswith("TYPE:") or up.startswith("FEATURE:"):
                    cur_role = classify_role(cs.split(":", 1)[1])
                elif up.startswith("FEATURE "):          # Simplify3D
                    cur_role = classify_role(cs[8:])
                elif up == "LAYER_CHANGE" or up == "CHANGE_LAYER" or up.startswith("LAYER:") \
                        or (up.startswith("LAYER ") and "Z =" in up):
                    if up.startswith("LAYER:") and info.slicer == "Cura":
                        try:
                            if int(cs.split(":", 1)[1]) < 0:   # raft в Cura
                                cur_role = ROLE_EXCLUDED
                        except ValueError:
                            pass
                    started = True
                    layer_idx += 1
                elif up.startswith("HEIGHT:") or up.startswith("LAYER_HEIGHT:"):
                    try:
                        cur_height = float(cs.split(":", 1)[1])
                    except ValueError:
                        pass
                elif up.startswith("WIPE_START"):
                    in_wipe = True
                elif up.startswith("WIPE_END"):
                    in_wipe = False
                elif up.startswith("MACHINE_END_GCODE_START") or up.startswith("END OF GCODE") \
                        or up == "END GCODE" or up.startswith("FILAMENT_GCODE_END"):
                    if started:
                        finished = True
            if not code.strip():
                continue
        else:
            code = raw
        code = code.strip()
        if not code:
            continue
        c0 = code[0].upper()
        if c0 not in "GM":
            # Klipper-макросы и т.п.
            cu = code.upper()
            if cu.startswith("EXCLUDE_OBJECT") or cu.startswith("SET_"):
                continue
            continue
        # номер команды
        sp = 1
        while sp < len(code) and (code[sp].isdigit() or code[sp] == "."):
            sp += 1
        cmd = c0 + code[1:sp]
        if c0 == "M":
            if cmd == "M82":
                abs_e = True; e_mode_explicit = True
            elif cmd == "M83":
                abs_e = False; e_mode_explicit = True
            continue
        # G-команды
        if cmd in ("G1", "G0", "G01", "G00", "G2", "G3", "G02", "G03"):
            params = {}
            for m in _num_re.finditer(code, sp):
                params[m.group(1).upper()] = float(m.group(2))
            nx, ny, nz = x, y, z
            if "X" in params:
                nx = params["X"] * scale + (x if not abs_xyz else 0.0)
            if "Y" in params:
                ny = params["Y"] * scale + (y if not abs_xyz else 0.0)
            if "Z" in params:
                nz = params["Z"] * scale + (z if not abs_xyz else 0.0)
            de = 0.0
            if "E" in params:
                ev = params["E"] * scale
                if abs_e:
                    de = ev - e
                    e = ev
                else:
                    de = ev
            if de > 1e-7:
                used += de
            is_arc = cmd in ("G2", "G3", "G02", "G03")
            full_circle = is_arc and ("I" in params or "J" in params) and nx == x and ny == y
            if de > 1e-7 and (nx != x or ny != y or full_circle):
                dvol_total = de * fil_area
                if is_arc and ("I" in params or "J" in params or "R" in params):
                    pts = _arc_points(x, y, nx, ny, params, scale, cmd in ("G2", "G02"), arc_seg_len)
                    # распределяем объём по длинам
                    seg = np.diff(pts, axis=0)
                    ls = np.hypot(seg[:, 0], seg[:, 1])
                    tot = ls.sum()
                    if tot > 1e-9:
                        zs = np.linspace(z, nz, len(pts))
                        for k in range(len(pts) - 1):
                            emit(pts[k, 0], pts[k, 1], pts[k + 1, 0], pts[k + 1, 1],
                                 zs[k + 1], dvol_total * ls[k] / tot)
                else:
                    emit(x, y, nx, ny, nz, dvol_total)
            x, y, z = nx, ny, nz
        elif cmd == "G92":
            for m in _num_re.finditer(code, sp):
                k = m.group(1).upper()
                v = float(m.group(2)) * scale
                if k == "E":
                    e = v
                elif k == "X":
                    x = v
                elif k == "Y":
                    y = v
                elif k == "Z":
                    z = v
        elif cmd == "G90":
            abs_xyz = True
            if not e_mode_explicit:
                abs_e = True
        elif cmd == "G91":
            abs_xyz = False
            if not e_mode_explicit:
                abs_e = False
        elif cmd == "G20":
            scale = 25.4
        elif cmd == "G21":
            scale = 1.0

    info.filament_used_mm = used
    info.excluded_volume = excluded_vol
    if not X0:
        raise ValueError("В G-code не найдено ни одного отрезка экструзии детали. "
                         "Проверьте, что файл получен из слайсера и содержит печать модели.")

    tp = Toolpaths(np.array(X0), np.array(Y0), np.array(X1), np.array(Y1), np.array(Z),
                   np.array(H), np.array(V), np.array(R, dtype=np.int8), np.array(L, dtype=np.int32), info)
    _fix_layers(tp)
    if not info.filament_type:
        info.warnings.append("Тип пластика в G-code не указан — выберите материал вручную.")
    if not has_layer_markers:
        info.warnings.append("В G-code нет маркеров слоёв: стартовый код мог попасть в модель.")
    return tp


def _arc_points(x0, y0, x1, y1, params, scale, clockwise, seg_len):
    if "R" in params and not ("I" in params or "J" in params):
        r = params["R"] * scale
        dx, dy = x1 - x0, y1 - y0
        d = math.hypot(dx, dy)
        if d < 1e-9 or abs(r) < d / 2 - 1e-6:
            return np.array([[x0, y0], [x1, y1]])
        hgt = math.sqrt(max(r * r - d * d / 4, 0.0))
        mx, my = (x0 + x1) / 2, (y0 + y1) / 2
        lx, ly = -dy / d, dx / d          # «влево» от направления хорды
        sgn = -1.0 if clockwise else 1.0  # G2 — центр справа, G3 — слева (при R>0)
        if r < 0:
            sgn = -sgn
        cx, cy = mx + sgn * hgt * lx, my + sgn * hgt * ly
    else:
        cx = x0 + params.get("I", 0.0) * scale
        cy = y0 + params.get("J", 0.0) * scale
    r = math.hypot(x0 - cx, y0 - cy)
    a0 = math.atan2(y0 - cy, x0 - cx)
    a1 = math.atan2(y1 - cy, x1 - cx)
    if clockwise:
        sweep = a1 - a0
        if sweep >= -1e-9:
            sweep -= 2 * math.pi
    else:
        sweep = a1 - a0
        if sweep <= 1e-9:
            sweep += 2 * math.pi
    n = max(1, int(math.ceil(abs(sweep) * r / seg_len)))
    t = np.linspace(0, 1, n + 1)
    ang = a0 + sweep * t
    pts = np.stack([cx + r * np.cos(ang), cy + r * np.sin(ang)], axis=1)
    pts[-1] = (x1, y1)
    return pts


def _fix_layers(tp: Toolpaths):
    """Уточняет толщину слоя и номера слоёв по фактическим Z."""
    info = tp.info
    zr = np.round(tp.z, 3)
    uz = np.unique(zr)
    # толщина по разнице Z между соседними слоями
    prev = np.concatenate([[0.0], uz[:-1]])
    dz = uz - prev
    if info.layer_height:
        lh = float(info.layer_height)
    elif len(dz) > 1:
        lh = float(np.median(dz[1:]))
    else:
        lh = 0.2
    if lh <= 0:
        lh = 0.2
    # слишком мелкие ступени (z-hop, «вазы») — не слои
    if len(uz) > 4 * max(1, len(np.unique(tp.layer))) and len(np.unique(tp.layer)) > 1:
        info.warnings.append("Z меняется внутри слоёв (режим вазы или неплоская печать) — "
                             "толщина слоя взята из настроек.")
        dz_map = {}
    else:
        dz_map = dict(zip(uz.tolist(), dz.tolist()))
    h = tp.h.copy()
    bad = h <= 0
    if bad.any():
        if dz_map:
            h[bad] = np.array([dz_map.get(v, lh) for v in zr[bad]])
        else:
            h[bad] = lh
    # защита от выбросов
    h = np.clip(h, 0.02, 2.0 * max(lh, 0.05) + 0.3)
    tp.h = h
    # номера слоёв по уникальным Z (надёжнее, чем по маркерам)
    tp.layer = np.searchsorted(uz, zr).astype(np.int32)
    info.n_layers = int(len(uz))
    if info.layer_height is None:
        info.layer_height = float(lh)
    if info.first_layer_height is None:
        info.first_layer_height = float(uz[0])


def load_gcode(path: str) -> Toolpaths:
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    return parse_gcode(text)
