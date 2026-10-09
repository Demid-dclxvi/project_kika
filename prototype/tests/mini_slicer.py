"""Мини-слайсер ТОЛЬКО для тестов: делает G-code в стиле OrcaSlicer / Bambu / Prusa / Cura
для простых тел (trimesh). Нужен, чтобы проверять расчёт на известной геометрии.
Требует: trimesh, shapely (manifold3d для булевых операций при создании тел).
"""
from __future__ import annotations

import math

import numpy as np
import shapely
from shapely.geometry import LineString, MultiLineString, Polygon, MultiPolygon, box as sbox
from shapely.ops import unary_union


def section_region(mesh, z):
    s = mesh.section(plane_origin=[0, 0, z], plane_normal=[0, 0, 1])
    if s is None:
        return Polygon()
    region = Polygon()
    for loop in s.discrete:
        if len(loop) < 3:
            continue
        p = Polygon(loop[:, :2])
        if not p.is_valid:
            p = p.buffer(0)
        region = region.symmetric_difference(p)
    return region.buffer(0)


def rings(geom):
    out = []
    if geom.is_empty:
        return out
    polys = [geom] if isinstance(geom, Polygon) else list(getattr(geom, "geoms", []))
    for p in polys:
        if not isinstance(p, Polygon) or p.is_empty:
            continue
        out.append(np.array(p.exterior.coords))
        for r in p.interiors:
            out.append(np.array(r.coords))
    return out


def hatch(area, spacing, angle_deg, offset=0.0):
    if area.is_empty:
        return []
    minx, miny, maxx, maxy = area.bounds
    cx, cy = (minx + maxx) / 2, (miny + maxy) / 2
    R = math.hypot(maxx - minx, maxy - miny) / 2 + spacing
    a = math.radians(angle_deg)
    d = np.array([math.cos(a), math.sin(a)])
    nrm = np.array([-d[1], d[0]])
    segs = []
    k0 = int(math.floor((-R - offset) / spacing))
    k1 = int(math.ceil((R - offset) / spacing))
    flip = False
    for k in range(k0, k1 + 1):
        t = offset + k * spacing
        p0 = np.array([cx, cy]) + nrm * t - d * R
        p1 = np.array([cx, cy]) + nrm * t + d * R
        inter = area.intersection(LineString([p0, p1]))
        if inter.is_empty:
            continue
        parts = [inter] if isinstance(inter, LineString) else [g for g in getattr(inter, "geoms", []) if isinstance(g, LineString)]
        parts.sort(key=lambda g: np.dot(np.array(g.coords[0]), d))
        for g in parts:
            c = np.array(g.coords)
            if len(c) < 2 or g.length < 1e-3:
                continue
            if flip:
                c = c[::-1]
            segs.append(c)
        flip = not flip
    return segs


class GWriter:
    def __init__(self, flavor="orca", fil_d=1.75):
        self.flavor = flavor
        self.lines = []
        self.fil_area = math.pi * (fil_d / 2) ** 2
        self.e = 0.0
        self.abs_e = flavor in ("prusa", "cura")
        self.x = self.y = 0.0
        self.retracted = False

    def w(self, s):
        self.lines.append(s)

    def type(self, name):
        names = {
            "orca": {"outer": "Outer wall", "inner": "Inner wall", "solid": "Internal solid infill",
                     "top": "Top surface", "bottom": "Bottom surface", "sparse": "Sparse infill",
                     "skirt": "Skirt", "custom": "Custom", "support": "Support"},
            "prusa": {"outer": "External perimeter", "inner": "Perimeter", "solid": "Solid infill",
                      "top": "Top solid infill", "bottom": "Solid infill", "sparse": "Internal infill",
                      "skirt": "Skirt/Brim", "custom": "Custom", "support": "Support material"},
            "cura": {"outer": "WALL-OUTER", "inner": "WALL-INNER", "solid": "SKIN", "top": "SKIN",
                     "bottom": "SKIN", "sparse": "FILL", "skirt": "SKIRT", "custom": "CUSTOM", "support": "SUPPORT"},
        }
        tbl = names["orca" if self.flavor == "bambu" else self.flavor]
        if self.flavor == "bambu":
            self.w(f"; FEATURE: {tbl[name]}")
        else:
            self.w(f";TYPE:{tbl[name]}")

    def travel(self, x, y):
        dist = math.hypot(x - self.x, y - self.y)
        if dist > 2.0 and not self.retracted:
            self.extrude_e(-0.8)
            self.retracted = True
        self.w(f"G1 X{x:.3f} Y{y:.3f} F12000")
        self.x, self.y = x, y

    def extrude_e(self, de):
        if self.abs_e:
            self.e += de
            self.w(f"G1 E{self.e:.5f} F2400")
        else:
            self.w(f"G1 E{de:.5f} F2400")

    def path(self, pts, w, h, closed=False):
        pts = np.asarray(pts)
        if closed and np.linalg.norm(pts[0] - pts[-1]) > 1e-6:
            pts = np.vstack([pts, pts[:1]])
        self.travel(pts[0, 0], pts[0, 1])
        if self.retracted:
            self.extrude_e(0.8)
            self.retracted = False
        area = (w - h) * h + math.pi * (h / 2) ** 2
        for p in pts[1:]:
            L = math.hypot(p[0] - self.x, p[1] - self.y)
            if L < 1e-4:
                continue
            de = L * area / self.fil_area
            if self.abs_e:
                self.e += de
                self.w(f"G1 X{p[0]:.3f} Y{p[1]:.3f} E{self.e:.5f}")
            else:
                self.w(f"G1 X{p[0]:.3f} Y{p[1]:.3f} E{de:.5f}")
            self.x, self.y = p[0], p[1]

    def reset_e(self):
        if self.abs_e:
            self.w("G92 E0")
            self.e = 0.0


def slice_mesh(mesh, layer_h=0.2, first_h=0.2, w=0.45, walls=2, top=4, bottom=4, infill=0.15,
               pattern="grid", solid_angles=(45, -45), flavor="orca", center=(128, 128),
               filament_type="PLA", skirt=True, purge=True, sparse_angles=None):
    mesh = mesh.copy()
    mesh.apply_translation([center[0] - mesh.bounds[:, 0].mean(), center[1] - mesh.bounds[:, 1].mean(),
                            -mesh.bounds[0, 2]])
    zmax = mesh.bounds[1, 2]
    tops = [first_h]
    while tops[-1] + layer_h <= zmax + 1e-6:
        tops.append(tops[-1] + layer_h)
    hs = [first_h] + [layer_h] * (len(tops) - 1)
    regions = [section_region(mesh, t - hh / 2) for t, hh in zip(tops, hs)]
    n = len(tops)

    g = GWriter(flavor)
    # заголовок
    if flavor in ("orca", "bambu"):
        g.w("; generated by OrcaSlicer 2.3.1 (test mini-slicer)")
        g.w("; HEADER_BLOCK_START")
        g.w(f"; total layer number: {n}")
        g.w("; HEADER_BLOCK_END")
        g.w("M83" if not g.abs_e else "M82")
    elif flavor == "prusa":
        g.w("; generated by PrusaSlicer 2.8.1 (test mini-slicer)")
        g.w("M82")
    else:
        g.w(";FLAVOR:Marlin")
        g.w(";Generated with Cura_SteamEngine 5.7.0 (test mini-slicer)")
        g.w(f";Layer height: {layer_h}")
        g.w("M82")
    g.w("G90")
    g.w("G28")
    # стартовый код: линия прочистки ДО первого слоя
    if purge:
        if flavor == "cura":
            pass
        g.w("G1 Z0.3 F600")
        g.w("G1 X10 Y5 F6000")
        g.reset_e()
        for xx in (60, 110):
            if g.abs_e:
                g.e += 4.0
                g.w(f"G1 X{xx} Y5 E{g.e:.4f} F1500")
            else:
                g.w(f"G1 X{xx} Y5 E4.0 F1500")
        g.x, g.y = 110, 5
        g.reset_e()

    for i in range(n):
        z, h = tops[i], hs[i]
        reg = regions[i]
        if flavor == "bambu":
            g.w("; CHANGE_LAYER")
            g.w(f"; Z_HEIGHT: {z:.3f}")
            g.w(f"; LAYER_HEIGHT: {h:.3f}")
        elif flavor in ("orca", "prusa"):
            g.w(";LAYER_CHANGE")
            g.w(f";Z:{z:.3f}")
            g.w(f";HEIGHT:{h:.3f}")
        else:
            g.w(f";LAYER:{i}")
        g.reset_e()
        g.w(f"G1 Z{z:.3f} F600")
        if reg.is_empty:
            continue
        if i == 0 and skirt:
            g.type("skirt")
            for r in rings(reg.buffer(3.0)):
                g.path(r, w, h, closed=True)
        # стенки
        sw = w - h * (1 - math.pi / 4)   # шаг между соседними валиками как в Slic3r/Orca
        for k in reversed(range(walls)):
            ring_geom = reg.buffer(-(w / 2 + k * sw), join_style=2)
            g.type("outer" if k == 0 else "inner")
            for r in rings(ring_geom):
                g.path(r, w, h, closed=True)
        inner = reg.buffer(-(w / 2 + (walls - 1) * sw + sw / 2), join_style=2)
        if inner.is_empty:
            continue
        # сплошные/разреженные зоны
        cover = None
        for j in list(range(1, top + 1)) + [-j for j in range(1, bottom + 1)]:
            k = i + j
            rk = regions[k] if 0 <= k < n else Polygon()
            cover = rk if cover is None else cover.intersection(rk)
        cover = cover if cover is not None else Polygon()
        solid = inner.difference(cover).buffer(0)
        sparse = inner.intersection(cover).buffer(0)
        if not solid.is_empty:
            is_top = i >= n - top
            g.type("top" if is_top and i == n - 1 else ("bottom" if i < bottom else "solid"))
            ang = solid_angles[i % len(solid_angles)]
            for s in hatch(solid, w - h * (1 - math.pi / 4), ang, offset=w / 2):
                g.path(s, w, h)
        if not sparse.is_empty and infill > 0:
            if infill >= 0.999:
                g.type("solid")
                ang = solid_angles[i % len(solid_angles)]
                for s in hatch(sparse, w - h * (1 - math.pi / 4), ang, offset=w / 2):
                    g.path(s, w, h)
            else:
                g.type("sparse")
                if sparse_angles is not None:
                    angs = [sparse_angles[i % len(sparse_angles)]]
                    sp = w / infill
                elif pattern == "grid":
                    angs, sp = [45, -45], 2 * w / infill
                elif pattern == "rectilinear":
                    angs, sp = [45 if i % 2 == 0 else -45], w / infill
                else:
                    angs, sp = [0], w / infill
                for a in angs:
                    for s in hatch(sparse, sp, a):
                        g.path(s, w, h)
    # финальный код
    g.type("custom")
    g.extrude_e(-2.0)
    g.w("G1 Z{:.2f} F600".format(tops[-1] + 10))
    g.w("M104 S0")
    # настройки (в конце, как у Orca/Prusa)
    if flavor in ("orca", "bambu"):
        g.w("; CONFIG_BLOCK_START")
        g.w(f"; layer_height = {layer_h}")
        g.w(f"; initial_layer_print_height = {first_h}")
        g.w("; filament_diameter = 1.75")
        g.w(f"; filament_type = {filament_type}")
        g.w("; nozzle_diameter = 0.4")
        g.w(f"; line_width = {w}")
        g.w(f"; sparse_infill_density = {infill * 100:.0f}%")
        g.w(f"; sparse_infill_pattern = {pattern}")
        g.w(f"; wall_loops = {walls}")
        g.w(f"; top_shell_layers = {top}")
        g.w(f"; bottom_shell_layers = {bottom}")
        g.w("; CONFIG_BLOCK_END")
    elif flavor == "prusa":
        g.w(f"; layer_height = {layer_h}")
        g.w(f"; first_layer_height = {first_h}")
        g.w("; filament_diameter = 1.75")
        g.w(f"; filament_type = {filament_type}")
        g.w(f"; fill_density = {infill * 100:.0f}%")
        g.w(f"; fill_pattern = {pattern}")
        g.w(f"; perimeters = {walls}")
        g.w("; prusaslicer_config = end")
    else:
        g.w(";End of Gcode")
    return "\n".join(g.lines) + "\n", mesh
