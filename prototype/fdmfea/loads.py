"""Выбор граней модели и приложение нагрузок / закреплений.

Координаты в задании по умолчанию — в системе детали: начало в минимальном углу
габарита детали (мм), оси как у принтера (Z — вверх при печати).
"""
from __future__ import annotations

import numpy as np

G = 9810.0  # мм/с²

AXES = {"x": 0, "y": 1, "z": 2}
SIDES = {"xmin": (0, -1), "xmax": (0, 1), "ymin": (1, -1), "ymax": (1, 1), "zmin": (2, -1), "zmax": (2, 1)}


class SelectionError(ValueError):
    pass


def _as3(v, name="вектор"):
    a = np.asarray(v, dtype=float).reshape(-1)
    if a.size != 3:
        raise SelectionError(f"{name} должен содержать 3 числа [x, y, z]")
    return a


def select_faces(mesh, sel, origin, name="выбор"):
    """Возвращает индексы граничных граней по описанию sel (dict или список dict)."""
    if sel is None:
        raise SelectionError(f"{name}: не задана область")
    if isinstance(sel, list):
        idx = [select_faces(mesh, s, origin, name) for s in sel]
        return np.unique(np.concatenate(idx)) if idx else np.array([], int)
    if isinstance(sel, str):
        sel = {"side": sel}
    fc = mesh.face_center - origin
    fn = mesh.face_normal
    s = mesh.vm.s
    tol = 0.51 * s
    m = np.zeros(len(fc), bool)
    if "faces" in sel:
        keys = np.asarray(sel["faces"], dtype=np.int64)
        m = np.isin(mesh.face_key, keys)
    elif "side" in sel:
        side = str(sel["side"]).lower()
        if side not in SIDES:
            raise SelectionError(f"{name}: неизвестная сторона '{side}', допустимо: {', '.join(SIDES)}")
        ax, sg = SIDES[side]
        depth = float(sel.get("depth", 0.0)) + tol
        facing = fn[:, ax] * sg > 0.5
        if facing.any():
            ext = (fc[facing, ax] * sg).max()
            m = facing & (fc[:, ax] * sg >= ext - depth)
        rng = sel.get("range")
        if rng:
            m &= _in_box(fc, rng)
    elif "box" in sel:
        m = _in_box(fc, sel["box"])
        if "normal" in sel:
            m &= _normal_match(fn, sel["normal"])
    elif "sphere" in sel or "brush" in sel:
        d = sel.get("sphere") or sel.get("brush")
        c = _as3(d["center"], "центр")
        r = float(d["radius"])
        m = np.linalg.norm(fc - c, axis=1) <= r
    elif "cylinder" in sel or "hole" in sel:
        d = sel.get("cylinder") or sel.get("hole")
        ax = AXES[str(d.get("axis", "z")).lower()]
        others = [a for a in range(3) if a != ax]
        c = np.asarray(d["center"], dtype=float).reshape(-1)
        c2 = c[:2] if c.size == 2 else c[others]
        r = float(d["radius"])
        dist = np.hypot(fc[:, others[0]] - c2[0], fc[:, others[1]] - c2[1])
        side_face = np.abs(fn[:, ax]) < 0.5
        if d.get("outer"):
            m = side_face & (dist >= r - 1.2 * s) & (dist <= r + 1.2 * s)
        else:
            m = side_face & (dist <= r + 1.0 * s)
        if "range" in d:
            lo, hi = d["range"]
            m &= (fc[:, ax] >= lo) & (fc[:, ax] <= hi)
    else:
        raise SelectionError(f"{name}: не понимаю описание области {sel}")
    idx = np.nonzero(m)[0]
    if len(idx) == 0:
        raise SelectionError(f"{name}: область не содержит ни одной поверхности детали. "
                             f"Проверьте координаты (система детали, мм).")
    return idx


def _in_box(fc, box):
    if isinstance(box, dict):
        lo, hi = _as3(box["min"], "min"), _as3(box["max"], "max")
    else:
        b = np.asarray(box, dtype=float).reshape(-1)
        lo, hi = b[:3], b[3:]
    lo2, hi2 = np.minimum(lo, hi), np.maximum(lo, hi)
    return np.all((fc >= lo2 - 1e-9) & (fc <= hi2 + 1e-9), axis=1)


def _normal_match(fn, nrm):
    if isinstance(nrm, str):
        ax, sg = SIDES[{"-x": "xmin", "+x": "xmax", "-y": "ymin", "+y": "ymax", "-z": "zmin",
                        "+z": "zmax"}.get(nrm, nrm)]
        v = np.zeros(3)
        v[ax] = sg
    else:
        v = _as3(nrm)
    return fn @ v > 0.5


def face_node_weights(mesh, faces):
    """Узлы и их «площади» (A/4 от каждой грани)."""
    nodes = mesh.face_nodes[faces].ravel()
    w = np.repeat(mesh.face_area[faces] / 4.0, 4)
    un, inv = np.unique(nodes, return_inverse=True)
    wn = np.bincount(inv, weights=w)
    return un, wn


def direction(d, mesh=None, faces=None):
    """Направление: вектор, '+x'/'-z'..., 'normal_in' / 'normal_out' (по нормали грани)."""
    if isinstance(d, str):
        t = d.strip().lower()
        if t in ("normal_in", "inward", "внутрь"):
            n = (mesh.face_normal[faces] * mesh.face_area[faces, None]).sum(0)
            return -n / max(np.linalg.norm(n), 1e-12)
        if t in ("normal_out", "outward", "наружу"):
            n = (mesh.face_normal[faces] * mesh.face_area[faces, None]).sum(0)
            return n / max(np.linalg.norm(n), 1e-12)
        sg = -1.0 if t.startswith("-") else 1.0
        ax = AXES[t.lstrip("+-")]
        v = np.zeros(3)
        v[ax] = sg
        return v
    v = _as3(d, "направление")
    n = np.linalg.norm(v)
    if n < 1e-12:
        raise SelectionError("нулевое направление")
    return v / n


def distribute_force(mesh, faces, F, f_out):
    nodes, w = face_node_weights(mesh, faces)
    W = w.sum()
    np.add.at(f_out, (3 * nodes[:, None] + np.arange(3)[None, :]).ravel(),
              (np.outer(w / W, F)).ravel())
    return nodes, w


def distribute_moment(mesh, faces, M, f_out, center=None):
    nodes, w = face_node_weights(mesh, faces)
    x = mesh.xyz[nodes]
    c = (x * w[:, None]).sum(0) / w.sum() if center is None else center
    d = x - c
    J = (w[:, None, None] * ((d * d).sum(1)[:, None, None] * np.eye(3)[None] - d[:, :, None] * d[:, None, :])).sum(0)
    th = np.linalg.pinv(J) @ M
    f = w[:, None] * np.cross(th[None, :], d)
    np.add.at(f_out, (3 * nodes[:, None] + np.arange(3)[None, :]).ravel(), f.ravel())
    return nodes


def distribute_pressure(mesh, faces, p, f_out):
    fvec = -p * mesh.face_area[faces, None] * mesh.face_normal[faces]
    nodes = mesh.face_nodes[faces]
    for k in range(4):
        np.add.at(f_out, (3 * nodes[:, k, None] + np.arange(3)[None, :]).ravel(), (fvec / 4).ravel())
    return np.unique(nodes)


def distribute_bearing(mesh, faces, F, f_out):
    Fn = np.linalg.norm(F)
    if Fn < 1e-12:
        return np.array([], int)
    u = F / Fn
    wf = np.maximum(0.0, -(mesh.face_normal[faces] @ u)) * mesh.face_area[faces]
    if wf.sum() <= 1e-12:
        wf = mesh.face_area[faces].astype(float)
    nodes = mesh.face_nodes[faces].ravel()
    w = np.repeat(wf / 4.0, 4)
    un, inv = np.unique(nodes, return_inverse=True)
    wn = np.bincount(inv, weights=w)
    np.add.at(f_out, (3 * un[:, None] + np.arange(3)[None, :]).ravel(), np.outer(wn / wn.sum(), F).ravel())
    return un
