"""Метод конечных элементов на воксельной сетке.

Элемент: 8-узловой гексаэдр с несовместными модами (Wilson–Taylor), точно передаёт изгиб
даже при 2–3 элементах по толщине. Материал в каждом элементе — свой (анизотропный),
матрица жёсткости элемента линейна по компонентам C, поэтому базисные матрицы
считаются один раз для размера элемента.
"""
from __future__ import annotations

import math
import time

import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla

from .amg import SmoothedAggregation

PAIRS = [(i, j) for i in range(6) for j in range(i, 6)]          # 21 независимая компонента C
_PI = np.array([p[0] for p in PAIRS])
_PJ = np.array([p[1] for p in PAIRS])

LOCAL_NODES = np.array([[-1, -1, -1], [1, -1, -1], [1, 1, -1], [-1, 1, -1],
                        [-1, -1, 1], [1, -1, 1], [1, 1, 1], [-1, 1, 1]], dtype=float)
NODE_OFFS = ((LOCAL_NODES + 1) / 2).astype(int)   # смещения узлов в сетке (i, j, k)
GAUSS = LOCAL_NODES / math.sqrt(3.0)

# узлы граней: 0:-x 1:+x 2:-y 3:+y 4:-z 5:+z (обход — наружу по правилу правой руки)
FACE_NODES = np.array([[0, 4, 7, 3], [1, 2, 6, 5], [0, 1, 5, 4], [3, 7, 6, 2], [0, 3, 2, 1], [4, 5, 6, 7]])
FACE_NORMALS = np.array([[-1, 0, 0], [1, 0, 0], [0, -1, 0], [0, 1, 0], [0, 0, -1], [0, 0, 1]], dtype=float)


def C_to_vec(C):
    """(…,6,6) -> (…,21)"""
    return C[..., _PI, _PJ]


def _B_and_G(xi, eta, zeta, dx, dy, dz):
    inv = np.array([2 / dx, 2 / dy, 2 / dz])
    dN = np.zeros((8, 3))
    for a in range(8):
        xa, ya, za = LOCAL_NODES[a]
        dN[a, 0] = xa * (1 + eta * ya) * (1 + zeta * za) / 8
        dN[a, 1] = ya * (1 + xi * xa) * (1 + zeta * za) / 8
        dN[a, 2] = za * (1 + xi * xa) * (1 + eta * ya) / 8
    dN = dN * inv
    B = np.zeros((6, 24))
    for a in range(8):
        nx_, ny_, nz_ = dN[a]
        c = 3 * a
        B[0, c] = nx_
        B[1, c + 1] = ny_
        B[2, c + 2] = nz_
        B[3, c + 1], B[3, c + 2] = nz_, ny_
        B[4, c], B[4, c + 2] = nz_, nx_
        B[5, c], B[5, c + 1] = ny_, nx_
    # несовместные моды P1=1-ξ², P2=1-η², P3=1-ζ²
    dP = np.zeros((3, 3))
    dP[0, 0] = -2 * xi * inv[0]
    dP[1, 1] = -2 * eta * inv[1]
    dP[2, 2] = -2 * zeta * inv[2]
    G = np.zeros((6, 9))
    for k in range(3):
        nx_, ny_, nz_ = dP[k]
        c = 3 * k
        G[0, c] = nx_
        G[1, c + 1] = ny_
        G[2, c + 2] = nz_
        G[3, c + 1], G[3, c + 2] = nz_, ny_
        G[4, c], G[4, c + 2] = nz_, nx_
        G[5, c], G[5, c + 1] = ny_, nx_
    return B, G


class ElementBasis:
    """Базисные матрицы для элемента размера dx×dy×dz."""

    def __init__(self, dx, dy, dz):
        self.dims = (dx, dy, dz)
        detJ = dx * dy * dz / 8.0
        self.vol = dx * dy * dz
        self.Bg = np.zeros((8, 6, 24))
        self.Gg = np.zeros((8, 6, 9))
        for g in range(8):
            self.Bg[g], self.Gg[g] = _B_and_G(*GAUSS[g], dx, dy, dz)
        E = np.zeros((21, 6, 6))
        for k, (i, j) in enumerate(PAIRS):
            E[k, i, j] = 1.0
            E[k, j, i] = 1.0
        self.Kuu = np.einsum("gia,kij,gjb->kab", self.Bg, E, self.Bg) * detJ
        self.Kua = np.einsum("gia,kij,gjb->kab", self.Bg, E, self.Gg) * detJ
        self.Kaa = np.einsum("gia,kij,gjb->kab", self.Gg, E, self.Gg) * detJ
        self.Bint = self.Bg.sum(0) * detJ        # ∫B dV (6×24)
        # экстраполяция из точек Гаусса в углы: значения трилинейного поля
        Ex = np.zeros((8, 8))
        s3 = math.sqrt(3.0)
        for c in range(8):
            for g in range(8):
                Ex[c, g] = np.prod(1 + LOCAL_NODES[c] * s3 * LOCAL_NODES[g]) / 8.0
        self.Extrap = Ex
        self._Kuu2 = self.Kuu.reshape(21, -1)
        self._Kua2 = self.Kua.reshape(21, -1)
        self._Kaa2 = self.Kaa.reshape(21, -1)

    def stiffness(self, Cvec):
        n = Cvec.shape[0]
        Kuu = (Cvec @ self._Kuu2).reshape(n, 24, 24)
        Kua = (Cvec @ self._Kua2).reshape(n, 24, 9)
        Kaa = (Cvec @ self._Kaa2).reshape(n, 9, 9)
        X = np.linalg.solve(Kaa, np.transpose(Kua, (0, 2, 1)))     # Kaa^-1 Kau
        return Kuu - Kua @ X

    def strains(self, Cvec, ue, at="gauss"):
        """Деформации в 8 точках элемента (n,8,6) и средняя по элементу (n,6).

        at="gauss" — в точках Гаусса (устойчиво к ступенькам вокселей),
        at="corners" — экстраполяция в углы (точнее на гладком изгибе, но завышает пики)."""
        n = Cvec.shape[0]
        Kua = (Cvec @ self._Kua2).reshape(n, 24, 9)
        Kaa = (Cvec @ self._Kaa2).reshape(n, 9, 9)
        alpha = -np.linalg.solve(Kaa, np.einsum("nab,na->nb", Kua, ue)[..., None])[..., 0]
        eg = np.einsum("gij,nj->ngi", self.Bg, ue) + np.einsum("gij,nj->ngi", self.Gg, alpha)
        if at == "corners":
            return np.einsum("cg,ngi->nci", self.Extrap, eg), eg.mean(1)
        return eg, eg.mean(1)


class Mesh:
    """Узлы, элементы и граничные грани воксельной модели."""

    def __init__(self, vm):
        self.vm = vm
        nx1, ny1 = vm.nx + 1, vm.ny + 1
        corner = ((vm.iz[:, None].astype(np.int64) + NODE_OFFS[None, :, 2]) * ny1
                  + (vm.iy[:, None] + NODE_OFFS[None, :, 1])) * nx1 + (vm.ix[:, None] + NODE_OFFS[None, :, 0])
        self.node_flat, inv = np.unique(corner.ravel(), return_inverse=True)
        self.elem_nodes = inv.reshape(-1, 8).astype(np.int64)
        k = self.node_flat // (nx1 * ny1)
        rem = self.node_flat % (nx1 * ny1)
        j = rem // nx1
        i = rem % nx1
        self.node_ijk = np.stack([i, j, k], 1)
        self.xyz = np.stack([vm.x0 + i * vm.sx, vm.y0 + j * vm.sy, vm.z_edges[k]], 1)
        self.n_nodes = len(self.node_flat)
        self.n_elems = vm.n
        self.elem_flat = vm.flat()
        dz = vm.dz[vm.iz]
        key = np.round(dz, 5)
        self.size_keys, self.elem_size_idx = np.unique(key, return_inverse=True)
        self.bases = [ElementBasis(vm.sx, vm.sy, float(d)) for d in self.size_keys]
        self.elem_vol = vm.sx * vm.sy * dz
        self._neighbors()
        self._boundary_faces()

    def _neighbors(self):
        vm = self.vm
        nb = np.full((self.n_elems, 6), -1, dtype=np.int64)
        flat = self.elem_flat
        steps = [(-1, 0, 0), (1, 0, 0), (0, -1, 0), (0, 1, 0), (0, 0, -1), (0, 0, 1)]
        for d, (di, dj, dk) in enumerate(steps):
            ii, jj, kk = vm.ix + di, vm.iy + dj, vm.iz + dk
            ok = (ii >= 0) & (ii < vm.nx) & (jj >= 0) & (jj < vm.ny) & (kk >= 0) & (kk < vm.nz)
            f2 = (kk.astype(np.int64) * vm.ny + jj) * vm.nx + ii
            pos = np.clip(np.searchsorted(flat, f2), 0, len(flat) - 1)
            hit = ok & (flat[pos] == f2)
            nb[hit, d] = pos[hit]
        self.nbr = nb

    def _boundary_faces(self):
        vm = self.vm
        e, d = np.nonzero(self.nbr < 0)
        self.face_elem = e
        self.face_dir = d
        self.face_nodes = self.elem_nodes[e[:, None], FACE_NODES[d]]
        self.face_normal = FACE_NORMALS[d]
        dz = vm.dz[vm.iz[e]]
        self.face_area = np.where(d < 2, vm.sy * dz, np.where(d < 4, vm.sx * dz, vm.sx * vm.sy))
        self.face_center = self.xyz[self.face_nodes].mean(1)
        self.face_key = self.elem_flat[e] * 6 + d

    def dofs(self):
        return (3 * self.elem_nodes[:, :, None] + np.arange(3)[None, None, :]).reshape(-1, 24)


def assemble(mesh: Mesh, Cvec: np.ndarray, chunk=6000, progress=None, elems=None):
    """Глобальная матрица. Если задан elems — только эти элементы (Cvec той же длины)."""
    ndof = 3 * mesh.n_nodes
    dofs = mesh.dofs()
    K = sp.csr_matrix((ndof, ndof))
    all_e = np.arange(mesh.n_elems) if elems is None else np.asarray(elems)
    pos = np.arange(len(all_e))
    for si, basis in enumerate(mesh.bases):
        sel = mesh.elem_size_idx[all_e] == si
        idx = all_e[sel]
        pidx = pos[sel]
        for a in range(0, len(idx), chunk):
            ii = idx[a:a + chunk]
            Ke = basis.stiffness(Cvec[pidx[a:a + chunk]])
            d = dofs[ii]
            rows = np.repeat(d, 24, axis=1).ravel()
            cols = np.tile(d, (1, 24)).ravel()
            Kc = sp.csr_matrix((Ke.ravel(), (rows, cols)), shape=(ndof, ndof))
            K = K + Kc
            if progress:
                progress(min(1.0, (a + len(ii)) / max(len(idx), 1)))
    K = K.tocsr()
    K.sum_duplicates()
    return K


def rigid_modes(xyz):
    c = xyz - xyz.mean(0)
    n = len(xyz)
    B = np.zeros((3 * n, 6))
    B[0::3, 0] = 1
    B[1::3, 1] = 1
    B[2::3, 2] = 1
    # вращения
    B[0::3, 3], B[1::3, 3] = -c[:, 1], c[:, 0]          # вокруг Z
    B[1::3, 4], B[2::3, 4] = -c[:, 2], c[:, 1]          # вокруг X
    B[0::3, 5], B[2::3, 5] = c[:, 2], -c[:, 0]          # вокруг Y
    return B


class Solver:
    """Решатель K_ff u_f = f_f: прямой для маленьких задач, иначе многосеточный AMG + CG."""

    def __init__(self, K, fixed_mask, xyz, cells=None, log=None, tol=1e-7, springs=1e-12):
        self.K = K
        self.fixed = fixed_mask
        self.free = np.nonzero(~fixed_mask)[0]
        self.log = log or (lambda *a: None)
        self.tol = tol
        Kff = K[self.free][:, self.free].tocsr()
        diag = Kff.diagonal()
        self.eps_spring = springs * float(np.mean(diag))
        Kff = Kff + sp.diags(np.full(Kff.shape[0], self.eps_spring))
        self.Kff = Kff
        n = Kff.shape[0]
        t = time.time()
        if n <= 6_000 or cells is None:
            self.lu = spla.splu(Kff.tocsc(), permc_spec="MMD_AT_PLUS_A")
            self.method = "direct"
        else:
            B = rigid_modes(xyz)[self.free]
            self.ml = SmoothedAggregation(Kff, B, np.asarray(cells)[self.free // 3])
            self.method = "amg"
        self.setup_time = time.time() - t

    def solve(self, rhs_free):
        t = time.time()
        if self.method == "direct":
            x = self.lu.solve(rhs_free)
            info = dict(method="прямой", iters=1)
        else:
            x, it, rel = self.ml.solve(self.Kff, rhs_free, tol=self.tol, maxiter=800)
            info = dict(method="AMG+CG", iters=it, rel_res=rel, levels=self.ml.n_levels)
            if rel > 1e-4:
                raise RuntimeError("Решатель не сошёлся: проверьте закрепления (деталь может свободно "
                                   "двигаться или вращаться) и разумность нагрузок.")
        info["time"] = time.time() - t
        return x, info
