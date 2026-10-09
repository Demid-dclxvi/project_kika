"""Алгебраический многосеточный решатель (сглаженная агрегация) на чистых numpy/scipy.

Нужен, чтобы программа не зависела от компилируемых пакетов: агрегаты строятся
геометрически по воксельной сетке (блоки 3×3×3 узла), ближнее ядро — 6 жёстких
перемещений тела, сглаживатель — полином Чебышёва, внешний метод — сопряжённые градиенты.
"""
from __future__ import annotations

import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla


def _spectral_radius(A, Dinv, iters=20, seed=0):
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(A.shape[0])
    x /= np.linalg.norm(x)
    lam = 1.0
    for _ in range(iters):
        y = Dinv * (A @ x)
        lam = float(np.linalg.norm(y))
        if lam == 0:
            return 1.0
        x = y / lam
    return lam


def _tentative(agg, n_agg, B):
    """Tentative prolongator T (QR базиса B на каждом агрегате) и грубый базис Bc."""
    n, nb = B.shape
    order = np.argsort(agg, kind="stable")
    counts = np.bincount(agg, minlength=n_agg)
    starts = np.concatenate([[0], np.cumsum(counts)])
    k = np.minimum(counts, nb)                       # число грубых неизвестных в агрегате
    coff = np.concatenate([[0], np.cumsum(k)])
    rows, cols, vals = [], [], []
    Bc = np.zeros((int(coff[-1]), nb))
    for m in np.unique(counts):
        if m == 0:
            continue
        aggs = np.nonzero(counts == m)[0]
        idx = order[starts[aggs][:, None] + np.arange(m)[None, :]]     # (na, m)
        Ba = B[idx]                                                    # (na, m, nb)
        km = min(m, nb)
        Q, R = np.linalg.qr(Ba, mode="reduced")                        # (na,m,km), (na,km,nb)
        # знак диагонали R делаем положительным — для устойчивости
        s = np.sign(np.diagonal(R, axis1=1, axis2=2))
        s[s == 0] = 1.0
        Q = Q * s[:, None, :]
        R = R * s[:, :, None]
        c = coff[aggs][:, None] + np.arange(km)[None, :]                # (na, km)
        rows.append(np.repeat(idx, km, axis=1).ravel())
        cols.append(np.tile(c, (1, m)).ravel())
        vals.append(Q.reshape(len(aggs), -1).ravel())
        Bc[c.ravel()] = R.reshape(-1, nb)
    T = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                      shape=(n, int(coff[-1])))
    return T, Bc, coff


class SmoothedAggregation:
    def __init__(self, A, B, cells, max_coarse=3000, max_levels=12, degree=3, block=3):
        A = sp.csr_matrix(A)
        self.levels = []
        self.degree = degree
        cells = np.asarray(cells, dtype=np.int64)
        while A.shape[0] > max_coarse and len(self.levels) < max_levels:
            D = A.diagonal()
            Dinv = 1.0 / np.where(D > 0, D, 1.0)
            rho = _spectral_radius(A, Dinv)
            acell = cells // block
            mx = acell.max(0) + 1
            key = (acell[:, 2] * mx[1] + acell[:, 1]) * mx[0] + acell[:, 0]
            ukey, agg = np.unique(key, return_inverse=True)
            n_agg = len(ukey)
            T, Bc, coff = _tentative(agg, n_agg, B)
            if T.shape[1] >= 0.9 * A.shape[0]:      # огрубление застопорилось
                break
            omega = 4.0 / (3.0 * rho)
            P = (T - sp.diags(omega * Dinv) @ (A @ T)).tocsr()
            P.eliminate_zeros()
            R = P.T.tocsr()
            Ac = (R @ (A @ P)).tocsr()
            Ac = 0.5 * (Ac + Ac.T)
            self.levels.append(dict(A=A, P=P, R=R, Dinv=Dinv, rho=rho))
            # «ячейка» грубой неизвестной — ячейка её агрегата
            agg_cell = np.zeros((n_agg, 3), np.int64)
            agg_cell[agg] = acell
            kk = np.diff(coff)
            cells = np.repeat(agg_cell, kk, axis=0)
            A, B = Ac.tocsr(), Bc
        self.A_coarse = A
        n = A.shape[0]
        if n <= 4000:
            import scipy.linalg as sla
            Ad = A.toarray()
            Ad = 0.5 * (Ad + Ad.T)
            Ad[np.diag_indices(n)] += 1e-10 * max(float(np.abs(np.diag(Ad)).max()), 1e-30)
            try:
                self._coarse = ("chol", sla.cho_factor(Ad, lower=True))
            except Exception:  # почти вырожденная грубая матрица
                self._coarse = ("dense", np.linalg.pinv(Ad, hermitian=True))
        else:
            self._coarse = ("lu", spla.splu(A.tocsc()))

    @property
    def n_levels(self):
        return len(self.levels) + 1

    def _coarse_solve(self, b):
        kind, f = self._coarse
        if kind == "chol":
            import scipy.linalg as sla
            return sla.cho_solve(f, b)
        if kind == "lu":
            return f.solve(b)
        return f @ b

    def _cheb(self, lv, b, x):
        A, Dinv, rho = lv["A"], lv["Dinv"], lv["rho"]
        upper, lower = 1.1 * rho, rho / 30.0
        theta = 0.5 * (upper + lower)
        delta = 0.5 * (upper - lower)
        sigma = theta / delta
        r = b - A @ x if x is not None else b.copy()
        x = np.zeros_like(b) if x is None else x
        d = Dinv * r / theta
        x = x + d
        rho_old = 1.0 / sigma
        for _ in range(self.degree - 1):
            r = r - A @ d
            rho_new = 1.0 / (2.0 * sigma - rho_old)
            d = rho_new * rho_old * d + (2.0 * rho_new / delta) * (Dinv * r)
            x = x + d
            rho_old = rho_new
        return x

    def vcycle(self, b, level=0):
        if level == len(self.levels):
            return self._coarse_solve(b)
        lv = self.levels[level]
        x = self._cheb(lv, b, None)
        r = b - lv["A"] @ x
        x = x + lv["P"] @ self.vcycle(lv["R"] @ r, level + 1)
        return self._cheb(lv, b, x)

    def solve(self, A, b, tol=1e-7, maxiter=500):
        """PCG с V-циклом в качестве предобусловливателя. Возвращает (x, число итераций, отн. невязка)."""
        x = np.zeros_like(b)
        r = b.copy()
        nb = np.linalg.norm(b)
        if nb == 0:
            return x, 0, 0.0
        z = self.vcycle(r)
        p = z.copy()
        rz = r @ z
        it = 0
        rel = 1.0
        for it in range(1, maxiter + 1):
            Ap = A @ p
            alpha = rz / (p @ Ap)
            x += alpha * p
            r -= alpha * Ap
            rel = np.linalg.norm(r) / nb
            if rel < tol:
                break
            z = self.vcycle(r)
            rz_new = r @ z
            p = z + (rz_new / rz) * p
            rz = rz_new
        return x, it, rel
