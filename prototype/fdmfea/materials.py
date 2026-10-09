"""Материалы: ортотропные свойства напечатанного пластика и поправочные коэффициенты.

Система координат валика: 1 — вдоль нити, 2 — поперёк нити в плоскости слоя, 3 — Z.
Нотация Фойгта: [11, 22, 33, 23, 13, 12], сдвиги инженерные (γ = 2ε).
"""
from __future__ import annotations

import copy
import json
import math
import os

import numpy as np

_DB_PATH = os.path.join(os.path.dirname(__file__), "materials.json")
T_REF = 23.0

REQUIRED = ("E1", "E2", "E3", "G12", "G13", "G23", "nu12", "nu13", "nu23",
            "Xt", "Xc", "Yt", "Yc", "Zt", "Zc", "S12", "S13", "S23", "density")

DEFAULTS = {"cte": 7e-5, "hdt": 60.0, "tg": 70.0, "creep_strength": 0.4, "creep_modulus": 0.55,
            "fatigue_k": 10.0, "elongation": 0.05, "note": ""}


def load_db(extra_path: str | None = None) -> dict:
    with open(_DB_PATH, "r", encoding="utf-8") as f:
        db = json.load(f)
    db.pop("_readme", None)
    if extra_path and os.path.exists(extra_path):
        with open(extra_path, "r", encoding="utf-8") as f:
            extra = json.load(f)
        extra.pop("_readme", None)
        db.update(extra)
    for k, m in db.items():
        for dk, dv in DEFAULTS.items():
            m.setdefault(dk, dv)
        m.setdefault("name", k)
        m["key"] = k
    return db


_ALIASES = [
    (("PAHT", "PA6-CF", "PA12-CF", "PA-CF", "PACF", "NYLON-CF", "PA-GF", "PA6-GF", "PAHT-CF"), "PA-CF"),
    (("PETG-CF", "PET-CF", "PETG CF"), "PETG-CF"),
    (("PLA-CF", "PLA CF"), "PLA-CF"),
    (("PETG", "PET", "PCTG"), "PETG"),
    (("ASA",), "ASA"),
    (("ABS",), "ABS"),
    (("PC",), "PC"),
    (("PA", "NYLON", "PA6", "PA12"), "PA"),
    (("TPU", "TPE", "FLEX"), "TPU"),
    (("PLA", "PLA+", "PLA PRO", "PLA-S"), "PLA"),
]


def guess_material(filament_type: str) -> str | None:
    if not filament_type:
        return None
    t = filament_type.upper().strip()
    for keys, mat in _ALIASES:
        for k in keys:
            if t == k or t.startswith(k):
                return mat
    for keys, mat in _ALIASES:
        for k in keys:
            if k in t:
                return mat
    return None


def material_from(spec, db=None) -> dict:
    """spec: имя из базы или dict (может содержать 'base' и переопределения)."""
    db = db or load_db()
    if isinstance(spec, str):
        if spec not in db:
            raise KeyError(f"Материал '{spec}' не найден. Есть: {', '.join(db)}")
        m = copy.deepcopy(db[spec])
    else:
        base = spec.get("base") or spec.get("key")
        m = copy.deepcopy(db[base]) if base in db else {}
        for dk, dv in DEFAULTS.items():
            m.setdefault(dk, dv)
        m.update({k: v for k, v in spec.items() if k != "base"})
    missing = [k for k in REQUIRED if k not in m]
    if missing:
        raise ValueError("В описании материала не хватает: " + ", ".join(missing))
    for k in REQUIRED + ("cte", "hdt", "tg", "creep_strength", "creep_modulus", "fatigue_k"):
        m[k] = float(m[k])
    return m


def orthotropic_C(E1, E2, E3, G12, G13, G23, nu12, nu13, nu23) -> np.ndarray:
    S = np.zeros((6, 6))
    S[0, 0], S[1, 1], S[2, 2] = 1 / E1, 1 / E2, 1 / E3
    S[0, 1] = S[1, 0] = -nu12 / E1
    S[0, 2] = S[2, 0] = -nu13 / E1
    S[1, 2] = S[2, 1] = -nu23 / E2
    S[3, 3], S[4, 4], S[5, 5] = 1 / G23, 1 / G13, 1 / G12
    C = np.linalg.inv(S)
    w = np.linalg.eigvalsh(C)
    if w.min() <= 0:
        raise ValueError("Свойства материала дают неположительно определённую матрицу жёсткости "
                         "(проверьте коэффициенты Пуассона).")
    return C


def C_dense(m) -> np.ndarray:
    return orthotropic_C(m["E1"], m["E2"], m["E3"], m["G12"], m["G13"], m["G23"],
                         m["nu12"], m["nu13"], m["nu23"])


# коэффициент вертикальной жёсткости разреженного заполнения (доля от ρ·E3)
INFILL_KZ = {
    "grid": 1.0, "triangles": 1.0, "tri-hexagon": 1.0, "trihexagon": 1.0, "honeycomb": 1.0,
    "3dhoneycomb": 0.5, "cubic": 0.75, "adaptivecubic": 0.6, "supportcubic": 0.4,
    "gyroid": 0.5, "rectilinear": 0.35, "alignedrectilinear": 0.35, "line": 0.35, "lines": 0.35,
    "zig-zag": 0.35, "zigzag": 0.35, "crosshatch": 0.45, "cross": 0.3, "concentric": 1.0,
    "lightning": 0.1, "octagram": 0.4, "archimedeanchords": 0.4, "hilbertcurve": 0.4,
    "quartercubic": 0.7, "tpmsd": 0.5, "tpmsfk": 0.5,
}


def infill_kz(pattern: str) -> float:
    p = (pattern or "").lower().replace("_", "").replace(" ", "")
    return INFILL_KZ.get(p, 0.6)


def C_lattice_member(m, rho: float, kz: float) -> np.ndarray:
    """Жёсткость «материала заполнения» на уровне нити (без умножения на долю объёма).

    Вдоль нити — полная, поперёк — почти нет связи между отдельными линиями,
    по Z — вертикальные стенки рисунка (kz)."""
    rho = float(np.clip(rho, 0.02, 1.0))
    nu = 0.05
    return orthotropic_C(m["E1"], m["E2"] * rho, m["E3"] * kz,
                         m["G12"] * rho, m["G13"] * kz, m["G23"] * rho, nu, nu, nu)


# ------------------------- повороты вокруг Z ------------------------------
_VOIGT = [(0, 0), (1, 1), (2, 2), (1, 2), (0, 2), (0, 1)]


def _rotz(theta):
    c, s = math.cos(theta), math.sin(theta)
    return np.array([[c, -s, 0.0], [s, c, 0.0], [0.0, 0.0, 1.0]])


def voigt_to_tensor4(C):
    T = np.zeros((3, 3, 3, 3))
    for I, (i, j) in enumerate(_VOIGT):
        for J, (k, l) in enumerate(_VOIGT):
            v = C[I, J]
            for a, b in ((i, j), (j, i)):
                for c, d in ((k, l), (l, k)):
                    T[a, b, c, d] = v
    return T


def tensor4_to_voigt(T):
    C = np.zeros((6, 6))
    for I, (i, j) in enumerate(_VOIGT):
        for J, (k, l) in enumerate(_VOIGT):
            C[I, J] = T[i, j, k, l]
    return C


def rotate_C(C_local, theta):
    """Жёсткость локального материала (ось 1 под углом theta к X) в глобальных осях."""
    R = _rotz(theta)
    T = voigt_to_tensor4(C_local)
    Tg = np.einsum("ia,jb,kc,ld,abcd->ijkl", R, R, R, R, T)
    return tensor4_to_voigt(Tg)


def strain_to_local(theta):
    """Матрица 6x6: инженерная деформация в глобальных осях -> в осях валика."""
    R = _rotz(theta)
    M = np.zeros((6, 6))
    for J in range(6):
        e = np.zeros(6)
        e[J] = 1.0
        t = np.zeros((3, 3))
        for I, (i, j) in enumerate(_VOIGT):
            v = e[I] if I < 3 else e[I] / 2
            t[i, j] = v
            t[j, i] = v
        tl = R.T @ t @ R
        out = np.array([tl[i, j] if I < 3 else 2 * tl[i, j] for I, (i, j) in enumerate(_VOIGT)])
        M[:, J] = out
    return M


# ------------------------- поправочные коэффициенты ----------------------

def temperature_factor(m, T):
    """Множитель модуля и прочности при температуре T (°C). Возвращает (f, предупреждение)."""
    if T is None:
        return 1.0, None
    T = float(T)
    hdt = m["hdt"]
    if T <= T_REF:
        return 1.0, None
    if T <= hdt:
        return 1.0 - 0.4 * (T - T_REF) / max(hdt - T_REF, 1.0), None
    top = max(m["tg"], hdt) + 15.0
    f = 0.6 * max(0.05, 1.0 - (T - hdt) / max(top - hdt, 5.0))
    return f, (f"Температура {T:.0f} °C выше теплостойкости {m['name']} (HDT ≈ {hdt:.0f} °C): "
               f"деталь будет размягчаться и «плыть» под нагрузкой.")


def fatigue_factor(m, cycles):
    if not cycles or cycles <= 1:
        return 1.0
    return float(min(1.0, cycles ** (-1.0 / m["fatigue_k"])))


def hoffman_coeffs(m, f=1.0):
    """Коэффициенты критерия Хоффмана при прочностях, умноженных на f."""
    Xt, Xc, Yt, Yc, Zt, Zc = (m[k] * f for k in ("Xt", "Xc", "Yt", "Yc", "Zt", "Zc"))
    S12, S13, S23 = (m[k] * f for k in ("S12", "S13", "S23"))
    C1 = 0.5 * (1 / (Yt * Yc) + 1 / (Zt * Zc) - 1 / (Xt * Xc))
    C2 = 0.5 * (1 / (Zt * Zc) + 1 / (Xt * Xc) - 1 / (Yt * Yc))
    C3 = 0.5 * (1 / (Xt * Xc) + 1 / (Yt * Yc) - 1 / (Zt * Zc))
    return dict(C1=C1, C2=C2, C3=C3, C4=1 / Xt - 1 / Xc, C5=1 / Yt - 1 / Yc, C6=1 / Zt - 1 / Zc,
                C7=1 / S23 ** 2, C8=1 / S13 ** 2, C9=1 / S12 ** 2)


def hoffman_sf(sig, hc):
    """Коэффициент запаса λ: F(λσ)=1. sig: (..., 6) в осях валика."""
    s1, s2, s3, t23, t13, t12 = (sig[..., i] for i in range(6))
    a = (hc["C1"] * (s2 - s3) ** 2 + hc["C2"] * (s3 - s1) ** 2 + hc["C3"] * (s1 - s2) ** 2
         + hc["C7"] * t23 ** 2 + hc["C8"] * t13 ** 2 + hc["C9"] * t12 ** 2)
    b = hc["C4"] * s1 + hc["C5"] * s2 + hc["C6"] * s3
    a = np.maximum(a, 0.0)
    disc = np.sqrt(b * b + 4 * a)
    with np.errstate(divide="ignore", invalid="ignore"):
        lam = np.where(a > 1e-30, (-b + disc) / (2 * a), np.where(b > 1e-30, 1.0 / b, np.inf))
    return np.where(np.isfinite(lam), lam, 1e6)


# виды разрушения
MODES = [
    "Разрыв вдоль нити",
    "Смятие вдоль нити",
    "Отрыв соседних нитей (в слое)",
    "Смятие поперёк нити",
    "Расслоение между слоями (отрыв по Z)",
    "Смятие по Z",
    "Межслойный сдвиг",
    "Сдвиг в плоскости слоя",
]


def failure_mode(sig, m):
    """Индекс доминирующего вида разрушения (по максимальному отношению напряжение/прочность)."""
    s1, s2, s3, t23, t13, t12 = (sig[..., i] for i in range(6))
    r = np.stack([
        np.where(s1 > 0, s1 / m["Xt"], 0.0),
        np.where(s1 < 0, -s1 / m["Xc"], 0.0),
        np.where(s2 > 0, s2 / m["Yt"], 0.0),
        np.where(s2 < 0, -s2 / m["Yc"], 0.0),
        np.where(s3 > 0, s3 / m["Zt"], 0.0),
        np.where(s3 < 0, -s3 / m["Zc"], 0.0),
        np.maximum(np.abs(t13) / m["S13"], np.abs(t23) / m["S23"]),
        np.abs(t12) / m["S12"],
    ], axis=-1)
    return np.argmax(r, axis=-1)


def laminate_check(m, angles=(45, -45), load_dir=0):
    """Расчётная прочность образца из слоёв с заданными углами при растяжении вдоль X (или Z).
    Используется для сверки базы с паспортами."""
    C = C_dense(m)
    Cs = [rotate_C(C, math.radians(a)) for a in angles]
    Cavg = sum(Cs) / len(Cs)
    if load_dir == 2:
        return min(m["Zt"], m["Zt"]), 1.0 / np.linalg.inv(Cavg)[2, 2]
    # одноосное растяжение вдоль X: σ = [1,0,0,0,0,0]
    Sav = np.linalg.inv(Cavg)
    eps = Sav @ np.array([1.0, 0, 0, 0, 0, 0])
    hc = hoffman_coeffs(m)
    sf = []
    for a in angles:
        T = strain_to_local(math.radians(a))
        sig = C @ (T @ eps)
        sf.append(hoffman_sf(sig, hc))
    return float(min(sf)), 1.0 / Sav[0, 0]
