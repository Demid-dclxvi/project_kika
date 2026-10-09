"""Перевод траекторий печати в воксельную расчётную модель.

Для каждого вокселя считаются:
  * доля объёма, занятая стенками / сплошным заполнением (rho_shell),
  * доля разреженного заполнения (rho_sparse) — усредняется по ячейке рисунка,
  * распределение направлений нитей (гистограмма по 12 углам в плоскости слоя).
Слои печати группируются так, чтобы границы вокселей по Z совпадали с границами слоёв.
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field

import numpy as np
from scipy import ndimage

from .gcode import Toolpaths, ROLE_SPARSE, ROLE_OUTER_WALL, ROLE_INNER_WALL, ROLE_SOLID, ROLE_UNKNOWN

NBINS = 12


@dataclass
class VoxelModel:
    s: float                    # средний размер вокселя в XY, мм
    sx: float                   # шаг по X
    sy: float                   # шаг по Y
    x0: float
    y0: float
    nx: int
    ny: int
    nz: int
    z_edges: np.ndarray         # (nz+1,) границы по Z
    ix: np.ndarray
    iy: np.ndarray
    iz: np.ndarray
    rho_shell: np.ndarray
    rho_sparse: np.ndarray
    hist_shell: np.ndarray      # (n, NB), нормировано (или нули)
    hist_sparse: np.ndarray     # (n, NB)
    role: np.ndarray            # доминирующая роль (1..4)
    infill_density: float
    infill_pattern: str
    layer_height: float
    stats: dict = field(default_factory=dict)

    @property
    def n(self):
        return len(self.ix)

    @property
    def dz(self):
        return np.diff(self.z_edges)

    def flat(self):
        return (self.iz.astype(np.int64) * self.ny + self.iy) * self.nx + self.ix

    def centers(self):
        zc = 0.5 * (self.z_edges[:-1] + self.z_edges[1:])
        return np.stack([self.x0 + (self.ix + 0.5) * self.sx, self.y0 + (self.iy + 0.5) * self.sy,
                         zc[self.iz]], axis=1)

    def rho(self):
        return self.rho_shell + self.rho_sparse

    def bbox(self):
        c = self.centers()
        half = np.stack([np.full(self.n, self.sx / 2), np.full(self.n, self.sy / 2), self.dz[self.iz] / 2], 1)
        return (c - half).min(0), (c + half).max(0)


def _layer_groups(tops, hs, dz_target):
    """Группирует слои в воксельные слои ~dz_target. Возвращает границы и номер группы для слоя."""
    edges = [tops[0] - hs[0]]
    grp = np.zeros(len(tops), dtype=np.int32)
    acc = 0.0
    g = 0
    for k in range(len(tops)):
        acc += hs[k]
        grp[k] = g
        if acc >= dz_target - 0.5 * hs[k] + 1e-9:
            edges.append(tops[k])
            acc = 0.0
            g += 1
    if acc > 0:
        if acc < 0.5 * dz_target and len(edges) > 1:
            edges[-1] = tops[-1]
            grp[grp == g] = g - 1
        else:
            edges.append(tops[-1])
    return np.array(edges), grp


def _deposit(tp: Toolpaths, sx, sy, X0, Y0, nx, ny, layer_grp):
    s = min(sx, sy)
    L = np.hypot(tp.x1 - tp.x0, tp.y1 - tp.y0)
    ok = L > 1e-6
    x0, y0, x1, y1 = tp.x0[ok], tp.y0[ok], tp.x1[ok], tp.y1[ok]
    vol, h, role, lay, L = tp.vol[ok], tp.h[ok], tp.role[ok], tp.layer[ok], L[ok]
    w = np.clip(vol / (L * h), 0.05, None)
    ang = np.mod(np.arctan2(y1 - y0, x1 - x0), np.pi)
    b = np.mod(np.round(ang / np.pi * NBINS).astype(np.int64), NBINS)   # центры корзин: 0°, 15°, 30°…
    nsub = np.maximum(1, np.ceil(L / (s / 3.0)).astype(np.int64))
    seg = np.repeat(np.arange(len(L)), nsub)
    start = np.concatenate([[0], np.cumsum(nsub)[:-1]])
    t = (np.arange(len(seg)) - start[seg] + 0.5) / nsub[seg]
    px = x0[seg] + t * (x1 - x0)[seg]
    py = y0[seg] + t * (y1 - y0)[seg]
    dv = (vol / nsub)[seg]
    wsx = np.minimum(w[seg] / sx, 1.0)
    wsy = np.minimum(w[seg] / sy, 1.0)
    iz = layer_grp[lay[seg]].astype(np.int64)
    out_keys, out_w, out_seg = [], [], []
    lo_x = (px - X0) / sx - wsx / 2
    lo_y = (py - Y0) / sy - wsy / 2
    ix0 = np.floor(lo_x).astype(np.int64)
    iy0 = np.floor(lo_y).astype(np.int64)
    fx0 = np.clip((ix0 + 1 - lo_x) / wsx, 0.0, 1.0)
    fy0 = np.clip((iy0 + 1 - lo_y) / wsy, 0.0, 1.0)
    for dxi, fx in ((0, fx0), (1, 1.0 - fx0)):
        for dyi, fy in ((0, fy0), (1, 1.0 - fy0)):
            wt = fx * fy
            m = wt > 1e-6
            ixx = np.clip(ix0[m] + dxi, 0, nx - 1)
            iyy = np.clip(iy0[m] + dyi, 0, ny - 1)
            out_keys.append((iz[m] * ny + iyy) * nx + ixx)
            out_w.append(dv[m] * wt[m])
            out_seg.append(seg[m])
    keys = np.concatenate(out_keys)
    wts = np.concatenate(out_w)
    sg = np.concatenate(out_seg)
    return keys, wts, role[sg], b[sg], ang, w


def voxelize(tp: Toolpaths, voxel: float | None = None, max_elems: int = 120_000,
             rho_min_shell: float = 0.12, rho_min_sparse: float = 0.02,
             keep_largest: bool = True, verbose=None) -> VoxelModel:
    info = tp.info
    lo, hi = tp.bbox()
    size = hi - lo
    # толщины слоёв
    zr = np.round(tp.z, 3)
    tops = np.unique(zr)
    hs = np.zeros(len(tops))
    idx = np.searchsorted(tops, zr)
    hsum = np.bincount(idx, weights=tp.h, minlength=len(tops))
    hcnt = np.bincount(idx, minlength=len(tops))
    hs = hsum / np.maximum(hcnt, 1)
    layer_h = float(np.median(hs)) if len(hs) else 0.2
    w_typ = float(np.median(tp.vol / np.maximum(np.hypot(tp.x1 - tp.x0, tp.y1 - tp.y0) * tp.h, 1e-9)))
    w_typ = float(np.clip(w_typ, 0.2, 2.0))

    if voxel:
        s_list = [float(voxel)]
    else:
        V_bb = float(np.prod(np.maximum(size, 0.1)))
        s0 = max(0.6 * w_typ, (V_bb / max_elems) ** (1 / 3))
        s_list = [s0]
    attempt = 0
    while True:
        s = s_list[-1]
        vm = _build(tp, s, lo, hi, tops, hs, layer_h, w_typ, rho_min_shell, rho_min_sparse, keep_largest)
        attempt += 1
        if voxel or attempt >= 4:
            break
        n = vm.n
        if n > 1.1 * max_elems:
            s_list.append(s * (n / max_elems) ** (1 / 3) * 1.03)
        elif n < 0.6 * max_elems and s > 0.6 * w_typ * 1.05:
            s_new = max(0.6 * w_typ, s * (n / (0.9 * max_elems)) ** (1 / 3))
            if s_new >= s * 0.97:
                break
            s_list.append(s_new)
        else:
            break
    if verbose:
        verbose(f"Воксель {vm.s:.2f} мм, элементов {vm.n}")
    return vm


def _build(tp, s, lo, hi, tops, hs, layer_h, w_typ, rho_min_shell, rho_min_sparse, keep_largest):
    info = tp.info
    # сетка выравнивается по габариту детали (по наружной кромке валиков),
    # шаг по X и Y слегка подгоняется, чтобы края детали совпали с гранями вокселей
    xl, xh = lo[0] - w_typ / 2, hi[0] + w_typ / 2
    yl, yh = lo[1] - w_typ / 2, hi[1] + w_typ / 2
    npx = max(1, int(round((xh - xl) / s)))
    npy = max(1, int(round((yh - yl) / s)))
    sx = (xh - xl) / npx
    sy = (yh - yl) / npy
    X0, Y0 = xl - sx, yl - sy
    nx, ny = npx + 2, npy + 2
    dz_target = max(layer_h, s)
    z_edges, grp = _layer_groups(tops, hs, dz_target)
    nz = len(z_edges) - 1
    N = nx * ny * nz
    if N > 60_000_000:
        raise MemoryError("Слишком мелкий воксель для такой детали — увеличьте размер вокселя.")
    dzs = np.diff(z_edges)
    vvox = (sx * sy * dzs)  # объём вокселя по слою

    keys, wts, roles, bins, seg_ang, seg_w = _deposit(tp, sx, sy, X0, Y0, nx, ny, grp)
    sparse_m = roles == ROLE_SPARSE
    shell_m = ~sparse_m

    vol_shell = np.bincount(keys[shell_m], weights=wts[shell_m], minlength=N).reshape(nz, ny, nx)
    vol_sp = np.bincount(keys[sparse_m], weights=wts[sparse_m], minlength=N).reshape(nz, ny, nx)
    rho_sh = (vol_shell / vvox[:, None, None])
    rho_sp_raw = (vol_sp / vvox[:, None, None])
    total_dep = float(wts.sum())

    # ---- области разреженного заполнения в каждом слое ----
    has_sparse = sparse_m.any()
    rho_sp = np.zeros_like(rho_sp_raw)
    infill_density = info.infill_density
    pattern = info.infill_pattern or ""
    if has_sparse:
        wall = rho_sh > 0.15
        st4 = ndimage.generate_binary_structure(2, 1)
        region = np.zeros_like(wall)
        for k in range(nz):
            if not (rho_sp_raw[k] > 0).any():
                continue
            lab, nl = ndimage.label(~wall[k], structure=st4)
            if nl == 0:
                continue
            border = np.unique(np.concatenate([lab[0], lab[-1], lab[:, 0], lab[:, -1]]))
            has = np.bincount(lab[rho_sp_raw[k] > 0].ravel(), minlength=nl + 1) > 0
            has[border] = False
            has[0] = False
            region[k] = has[lab]
        # оценка доли заполнения
        if region.any():
            est = float(rho_sp_raw[region].sum() / region.sum())
        else:
            est = 0.0
        if not infill_density or infill_density <= 0:
            infill_density = est
        d = max(infill_density or est, 0.03)
        winx = int(np.clip(round(2.0 * w_typ / (d * sx)) + 1, 1, 15))
        winy = int(np.clip(round(2.0 * w_typ / (d * sy)) + 1, 1, 15))
        if max(winx, winy) > 1:
            m = region.astype(np.float64)
            num = ndimage.uniform_filter(rho_sp_raw * m, size=(1, winy, winx), mode="constant")
            den = ndimage.uniform_filter(m, size=(1, winy, winx), mode="constant")
            with np.errstate(invalid="ignore", divide="ignore"):
                sm = np.where(den > 1e-6, num / den, 0.0)
            rho_sp = np.where(region, sm, rho_sp_raw)
        else:
            rho_sp = rho_sp_raw.copy()
    rho_sh = np.clip(rho_sh, 0.0, 1.0)
    rho_sp = np.clip(rho_sp, 0.0, None)
    rho_sp = np.minimum(rho_sp, 1.0 - rho_sh)

    active = (rho_sh >= rho_min_shell) | ((rho_sp >= rho_min_sparse) & (rho_sh + rho_sp >= rho_min_sparse))
    removed_frac = 0.0
    if keep_largest and active.any():
        lab, nl = ndimage.label(active, structure=ndimage.generate_binary_structure(3, 1))
        if nl > 1:
            cnt = np.bincount(lab.ravel())
            cnt[0] = 0
            big = int(np.argmax(cnt))
            mass_all = ((rho_sh + rho_sp) * vvox[:, None, None])[active].sum()
            keep = lab == big
            mass_keep = ((rho_sh + rho_sp) * vvox[:, None, None])[keep].sum()
            removed_frac = float(1 - mass_keep / max(mass_all, 1e-12))
            active = keep
    iz, iy, ix = np.nonzero(active)
    flat = (iz.astype(np.int64) * ny + iy) * nx + ix

    # ---- гистограммы направлений стенок ----
    occ_keys, inv = np.unique(keys[shell_m], return_inverse=True)
    hist = np.bincount(inv * NBINS + bins[shell_m], weights=wts[shell_m],
                       minlength=len(occ_keys) * NBINS).reshape(-1, NBINS)
    hist_shell = np.zeros((len(flat), NBINS))
    if len(occ_keys):
        pos_c = np.clip(np.searchsorted(occ_keys, flat), 0, len(occ_keys) - 1)
        found = occ_keys[pos_c] == flat
        hist_shell[found] = hist[pos_c[found]]
    sm = hist_shell.sum(1, keepdims=True)
    hist_shell = np.where(sm > 0, hist_shell / np.maximum(sm, 1e-30), 0.0)

    # ---- направления разреженного заполнения: по слоям вокселей ----
    if has_sparse:
        kz_keys = keys[sparse_m] // (nx * ny)
        hz = np.bincount(kz_keys * NBINS + bins[sparse_m], weights=wts[sparse_m],
                         minlength=nz * NBINS).reshape(nz, NBINS)
        # пустые слои — берём соседние
        tot = hz.sum(1)
        if (tot > 0).any():
            good = np.nonzero(tot > 0)[0]
            nearest = good[np.clip(np.searchsorted(good, np.arange(nz)), 0, len(good) - 1)]
            hz = hz[nearest]
        hz = hz / np.maximum(hz.sum(1, keepdims=True), 1e-30)
        hist_sparse = hz[iz]
    else:
        hist_sparse = np.zeros((len(flat), NBINS))

    rs = rho_sh[iz, iy, ix].astype(np.float64)
    rp = rho_sp[iz, iy, ix].astype(np.float64)
    # элемент без гистограммы стенок (только заполнение) — переносим долю в заполнение
    no_h = hist_shell.sum(1) <= 0
    rp[no_h] = np.minimum(1.0, rp[no_h] + rs[no_h])
    rs[no_h] = 0.0
    if has_sparse:
        no_sp_h = hist_sparse.sum(1) <= 0
        rs[no_sp_h] = np.minimum(1.0, rs[no_sp_h] + rp[no_sp_h])
        rp[no_sp_h] = 0.0

    # доминирующая роль
    role_codes = np.array([ROLE_OUTER_WALL, ROLE_INNER_WALL, ROLE_SOLID, ROLE_SPARSE])
    rmap = np.full(8, 2)
    rmap[ROLE_OUTER_WALL], rmap[ROLE_INNER_WALL], rmap[ROLE_SOLID], rmap[ROLE_UNKNOWN] = 0, 1, 2, 2
    rmap[ROLE_SPARSE] = 3
    r_idx = rmap[roles]
    occ_all, inv_all = np.unique(keys, return_inverse=True)
    rh = np.bincount(inv_all * 4 + r_idx, weights=wts, minlength=len(occ_all) * 4).reshape(-1, 4)
    pa = np.clip(np.searchsorted(occ_all, flat), 0, len(occ_all) - 1)
    fa = occ_all[pa] == flat
    rh_e = np.zeros((len(flat), 4))
    rh_e[fa] = rh[pa[fa]]
    rh_e[:, 3] += rp * vvox[iz]    # сглаженное заполнение
    role = role_codes[np.argmax(rh_e, axis=1)]
    role[(rh_e.sum(1) <= 0)] = ROLE_SPARSE

    model_vol = float(((rs + rp) * vvox[iz]).sum())
    vm = VoxelModel(s=0.5 * (sx + sy), sx=sx, sy=sy, x0=X0, y0=Y0, nx=nx, ny=ny, nz=nz, z_edges=z_edges,
                    ix=ix.astype(np.int32), iy=iy.astype(np.int32), iz=iz.astype(np.int32),
                    rho_shell=rs, rho_sparse=rp, hist_shell=hist_shell, hist_sparse=hist_sparse,
                    role=role.astype(np.int8), infill_density=float(infill_density or 0.0),
                    infill_pattern=pattern, layer_height=layer_h)
    vm.stats = dict(deposited_volume=total_dep, model_volume=model_vol,
                    removed_fraction=removed_frac, grid=(nx, ny, nz), line_width=w_typ,
                    dz_mean=float(dzs.mean()))
    return vm
