"""Сборка расчёта: материал по элементам, нагрузки, решение, запас прочности."""
from __future__ import annotations

import math
import time
from dataclasses import dataclass, field

import numpy as np

from . import materials as mat
from .fem import Mesh, Solver, assemble, C_to_vec, PAIRS
from .gcode import Toolpaths, parse_gcode, load_gcode
from .loads import (G, SelectionError, select_faces, direction, distribute_force, distribute_moment,
                    distribute_pressure, distribute_bearing, face_node_weights)
from .voxelize import voxelize, NBINS

DURATION_NAMES = {"short": "кратковременная", "long": "длительная (ползучесть)",
                  "cyclic": "циклическая (усталость)"}


class Model:
    """G-code → воксели → сетка. Создаётся один раз, расчётов можно сделать сколько угодно."""

    def __init__(self, gcode=None, text=None, voxel=None, max_elems=120_000, progress=None, toolpaths=None):
        p = progress or (lambda *a: None)
        t0 = time.time()
        p("parse", 0.0, "Читаю G-code")
        if toolpaths is not None:
            self.tp = toolpaths
        elif text is not None:
            self.tp = parse_gcode(text)
        else:
            self.tp = load_gcode(gcode)
        p("voxel", 0.3, "Строю воксельную модель")
        self.vm = voxelize(self.tp, voxel=voxel, max_elems=max_elems)
        p("mesh", 0.7, "Строю сетку")
        self.mesh = Mesh(self.vm)
        # система координат детали: начало в минимальном углу габарита по наружной кромке валиков
        vm = self.vm
        lo = np.array([vm.x0 + vm.sx, vm.y0 + vm.sy, vm.z_edges[0]])
        hi = np.array([vm.x0 + (vm.nx - 1) * vm.sx, vm.y0 + (vm.ny - 1) * vm.sy, vm.z_edges[-1]])
        self.origin = lo
        self.size = hi - lo
        self.material_guess = mat.guess_material(self.tp.info.filament_type)
        self.build_time = time.time() - t0
        p("ready", 1.0, "Модель готова")

    def thin_fraction(self):
        """Доля материала в элементах толщиной в один воксель (тонкие стенки)."""
        nb = self.mesh.nbr
        thin = ((nb[:, 0] < 0) & (nb[:, 1] < 0)) | ((nb[:, 2] < 0) & (nb[:, 3] < 0)) | ((nb[:, 4] < 0) & (nb[:, 5] < 0))
        w = self.vm.rho() * self.mesh.elem_vol
        return float((w * thin).sum() / max(w.sum(), 1e-12))

    def summary(self):
        info = self.tp.info
        st = self.vm.stats
        warns = list(info.warnings)
        tf = self.thin_fraction()
        if tf > 0.12:
            warns.append(f"{round(tf * 100)}% материала — в стенках толщиной в один воксель. Жёсткость таких стенок "
                         f"на изгиб завышается: увеличьте детальность сетки.")
        return dict(
            slicer=info.slicer, filament_type=info.filament_type, material_guess=self.material_guess,
            layer_height=round(self.vm.layer_height, 3), infill_density=round(self.vm.infill_density, 3),
            infill_pattern=self.vm.infill_pattern, wall_loops=info.wall_loops,
            size=[round(float(v), 2) for v in self.size], voxel=round(self.vm.s, 3),
            elements=int(self.vm.n), nodes=int(self.mesh.n_nodes), layers=int(info.n_layers),
            volume_mm3=round(st["deposited_volume"], 1), model_volume_mm3=round(st["model_volume"], 1),
            removed_fraction=round(st["removed_fraction"], 4), warnings=warns,
            build_time=round(self.build_time, 2),
        )


@dataclass
class CaseResult:
    name: str
    u: np.ndarray
    sf: np.ndarray
    mode: np.ndarray
    vm: np.ndarray
    sigma: np.ndarray
    summary: dict
    bc_elem: np.ndarray
    sel_faces: dict = field(default_factory=dict)


class MaterialField:
    """Жёсткость каждого элемента и матрицы для пересчёта напряжений в оси валиков."""

    def __init__(self, model: Model, m: dict):
        self.m = m
        vm = model.vm
        Cd = mat.C_dense(m)
        kz = mat.infill_kz(vm.infill_pattern)
        self.kz = kz
        D0 = np.diag([m["E1"], 0.0, m["E3"] * kz, 0.0, m["G13"] * kz, 0.0])
        D1 = np.diag([0.0, m["E2"], 0.0, m["G23"], 0.0, m["G12"]])
        th = np.arange(NBINS) * math.pi / NBINS
        self.T = np.array([mat.strain_to_local(t) for t in th])           # (NB,6,6)
        Cr_d = np.array([C_to_vec(mat.rotate_C(Cd, t)) for t in th])       # (NB,21)
        Cr_0 = np.array([C_to_vec(mat.rotate_C(D0, t)) for t in th])
        Cr_1 = np.array([C_to_vec(mat.rotate_C(D1, t)) for t in th])
        hs = vm.hist_shell.copy()
        bad = (hs.sum(1) <= 0) & (vm.rho_shell > 0)
        hs[bad] = 1.0 / NBINS
        self.hs = hs
        self.hp = vm.hist_sparse
        rs, rp = vm.rho_shell, vm.rho_sparse
        self.rs, self.rp = rs, rp
        self.Cvec = rs[:, None] * (hs @ Cr_d) + rp[:, None] * (self.hp @ Cr_0) + (rp ** 2)[:, None] * (self.hp @ Cr_1)
        # минимальная жёсткость, чтобы не было вырожденных элементов
        iso = C_to_vec(Cd) * 1e-4
        self.Cvec = self.Cvec + iso[None, :]
        self.Ms = np.einsum("ij,bjk->bik", Cd, self.T)     # (NB,6,6) напряжение в осях валика
        self.M0 = np.einsum("ij,bjk->bik", D0, self.T)
        self.M1 = np.einsum("ij,bjk->bik", D1, self.T)
        self.hc = mat.hoffman_coeffs(m)

    def C_full(self, ii, Cvec=None):
        C = np.zeros((len(ii), 6, 6))
        cv = (self.Cvec if Cvec is None else Cvec)[ii]
        for k, (i, j) in enumerate(PAIRS):
            C[:, i, j] = cv[:, k]
            C[:, j, i] = cv[:, k]
        return C


def _vm(s):
    return np.sqrt(0.5 * ((s[..., 0] - s[..., 1]) ** 2 + (s[..., 1] - s[..., 2]) ** 2 + (s[..., 2] - s[..., 0]) ** 2)
                   + 3 * (s[..., 3] ** 2 + s[..., 4] ** 2 + s[..., 5] ** 2))


class Analysis:
    def __init__(self, model: Model, job: dict, progress=None, log=None):
        self.model = model
        self.job = job
        self.p = progress or (lambda *a: None)
        self.log = log or (lambda *a: None)
        mspec = job.get("material") or model.material_guess or "PLA"
        self.m = mat.material_from(mspec)
        self.target_sf = float(job.get("target_sf", 2.0))
        coords = job.get("coords", "part")
        self.origin = model.origin if coords == "part" else np.zeros(3)
        self.warnings = []

    # ------------------------------------------------------------------
    def run(self):
        t0 = time.time()
        model, mesh = self.model, self.model.mesh
        cases = self.job.get("cases") or []
        if not cases:
            raise ValueError("В задании нет ни одного расчётного случая (cases).")
        # сначала разбираем все нагрузки — ошибки в задании видны сразу
        self.p("material", 0.01, "Проверяю нагрузки и закрепления")
        bcs = []
        for ci, case in enumerate(cases):
            self.warnings = []
            bc = self._fixtures_and_loads(case)
            bc["pre_warnings"] = list(self.warnings)
            bcs.append(bc)
        self.p("material", 0.03, "Задаю анизотропию материала")
        self.mf = MaterialField(model, self.m)
        # Частично заполненные воксели на опорных и нагруженных гранях считаем сплошными:
        # реальная поверхность лежит внутри вокселя, «размазанный» мягкий слой дал бы
        # ложную податливость в месте закрепления.
        faces = [f for bc in bcs for f in bc["sel_faces"].values()]
        dens_e = np.unique(mesh.face_elem[np.concatenate(faces)]) if faces else np.array([], int)
        rho_e = self.mf.rs[dens_e] + self.mf.rp[dens_e]
        dens_e = dens_e[rho_e < 0.98]
        scale = 1.0 / np.clip(self.mf.rs[dens_e] + self.mf.rp[dens_e], 0.1, 1.0)
        self.Cvec = self.mf.Cvec.copy()
        self.Cvec[dens_e] *= scale[:, None]
        self.p("assemble", 0.05, "Собираю матрицу жёсткости")
        self.K = assemble(mesh, self.Cvec,
                          progress=lambda f: self.p("assemble", 0.05 + 0.2 * f, "Собираю матрицу жёсткости"))
        results = []
        solvers = {}
        for ci, case in enumerate(cases):
            base = 0.25 + 0.75 * ci / len(cases)
            span = 0.75 / len(cases)
            name = case.get("name") or f"Случай {ci + 1}"
            self.p("case", base, f"{name}: нагрузки")
            res = self._run_case(case, name, solvers, bcs[ci],
                                 lambda f, t, b=base, sp_=span, nm=name: self.p("case", b + sp_ * f, f"{nm}: {t}"))
            results.append(res)
        self.results = results
        self.total_time = time.time() - t0
        self.p("done", 1.0, "Готово")
        return results

    # ------------------------------------------------------------------
    def _fixtures_and_loads(self, case):
        mesh = self.model.mesh
        nn = mesh.n_nodes
        fixed = np.zeros(3 * nn, bool)
        uval = np.zeros(3 * nn)
        f_force = np.zeros(3 * nn)
        f_body_dirs = []
        bc_nodes = []
        sel_faces = {}
        impact = None
        thermal = bool(case.get("thermal_expansion", False))
        load_desc = []
        fix_nodes, disp_nodes = [], []
        fixtures = case.get("fixtures", self.job.get("fixtures")) or []
        if not fixtures:
            raise ValueError(f"«{case.get('name', '')}»: не задано ни одного закрепления.")
        for k, fx in enumerate(fixtures):
            faces = select_faces(mesh, fx.get("where"), self.origin, f"Закрепление {k + 1}")
            sel_faces[f"fix{k}"] = faces
            nodes, _ = face_node_weights(mesh, faces)
            comps = str(fx.get("components", "xyz")).lower()
            for c, ch in enumerate("xyz"):
                if ch in comps:
                    fixed[3 * nodes + c] = True
            bc_nodes.append(nodes)
            fix_nodes.append(nodes)
        loads = case.get("loads") or []
        if not loads:
            raise ValueError(f"«{case.get('name', '')}»: не задано ни одной нагрузки.")
        for k, ld in enumerate(loads):
            t = str(ld.get("type", "force")).lower()
            lname = f"Нагрузка {k + 1} ({t})"
            if t in ("gravity", "acceleration", "weight", "self_weight"):
                gv = ld.get("g", ld.get("vector", [0, 0, -1]))
                if isinstance(gv, (int, float)):
                    gv = [0, 0, -float(gv)]
                if isinstance(gv, str):
                    gv = direction(gv)
                f_body_dirs.append(np.asarray(gv, float))
                load_desc.append(dict(type=t, g=list(map(float, gv)), magnitude=float(np.linalg.norm(gv)), unit="g"))
                continue
            faces = select_faces(mesh, ld.get("where"), self.origin, lname)
            sel_faces[f"load{k}"] = faces
            nodes, _ = face_node_weights(mesh, faces)
            bc_nodes.append(nodes)
            if t == "force":
                F = self._vector(ld, faces, mesh)
                distribute_force(mesh, faces, F, f_force)
                if ld.get("point") is not None:
                    P = np.asarray(ld["point"], float) + self.origin
                    nodes_, w = face_node_weights(mesh, faces)
                    c = (mesh.xyz[nodes_] * w[:, None]).sum(0) / w.sum()
                    distribute_moment(mesh, faces, np.cross(P - c, F), f_force)
                load_desc.append(dict(type=t, magnitude=float(np.linalg.norm(F)), unit="Н", vector=F.tolist()))
            elif t in ("mass", "weight_kg", "hanging"):
                kg = float(ld.get("kg", ld.get("mass", 0)))
                d = direction(ld.get("direction", "-z"), mesh, faces)
                F = kg * 9.81 * d
                distribute_force(mesh, faces, F, f_force)
                if ld.get("point") is not None:
                    P = np.asarray(ld["point"], float) + self.origin
                    nodes_, w = face_node_weights(mesh, faces)
                    c = (mesh.xyz[nodes_] * w[:, None]).sum(0) / w.sum()
                    distribute_moment(mesh, faces, np.cross(P - c, F), f_force)
                load_desc.append(dict(type="mass", magnitude=kg, unit="кг", vector=F.tolist()))
            elif t == "pressure":
                pval = float(ld.get("value", ld.get("mpa", 0.0)))
                distribute_pressure(mesh, faces, pval, f_force)
                area = float(mesh.face_area[faces].sum())
                load_desc.append(dict(type=t, magnitude=pval, unit="МПа", area=area))
            elif t in ("moment", "torque"):
                if ld.get("value") is not None and ld.get("axis"):
                    M = direction(ld["axis"]) * float(ld["value"])
                else:
                    M = np.asarray(ld.get("vector") or [0, 0, 0], float)
                ctr = ld.get("center")
                ctr = None if ctr is None else np.asarray(ctr, float) + self.origin
                distribute_moment(mesh, faces, M, f_force, ctr)
                load_desc.append(dict(type="moment", magnitude=float(np.linalg.norm(M)), unit="Н·мм",
                                      vector=M.tolist()))
            elif t in ("bearing", "bolt", "pin"):
                F = self._vector(ld, faces, mesh)
                distribute_bearing(mesh, faces, F, f_force)
                load_desc.append(dict(type="bearing", magnitude=float(np.linalg.norm(F)), unit="Н", vector=F.tolist()))
            elif t == "impact":
                kg = float(ld.get("kg", ld.get("mass", 0)))
                hgt = float(ld.get("height", 0.0))
                d = direction(ld.get("direction", "-z"), mesh, faces)
                F = kg * 9.81 * d
                distribute_force(mesh, faces, F, f_force)
                impact = dict(kg=kg, height=hgt, dir=d, faces=faces)
                load_desc.append(dict(type="impact", magnitude=kg, unit="кг", height=hgt, vector=F.tolist()))
            elif t in ("displacement", "enforced", "displace"):
                vec = ld.get("vector", [None, None, None])
                if len(vec) != 3:
                    raise ValueError(f"{lname}: перемещение задаётся тремя числами [x, y, z] (null — свободно)")
                for c in range(3):
                    if vec[c] is not None:
                        fixed[3 * nodes + c] = True
                        uval[3 * nodes + c] = float(vec[c])
                disp_nodes.append(nodes)
                load_desc.append(dict(type="displacement", magnitude=float(np.linalg.norm([v or 0 for v in vec])),
                                      unit="мм", vector=[v for v in vec]))
            else:
                raise ValueError(f"{lname}: неизвестный тип нагрузки '{t}'")
        if impact is not None and len(loads) > 1:
            raise ValueError("Удар (impact) считается отдельным случаем: уберите из него другие нагрузки.")
        # проверка закреплений
        for c, ch in enumerate("XYZ"):
            if not fixed[c::3].any():
                self.warnings.append(f"Нет закрепления по оси {ch}: деталь может «уплыть» — добавлены "
                                     f"слабые пружины, проверьте результат.")
        return dict(fixed=fixed, uval=uval, f_force=f_force, body=f_body_dirs, bc_nodes=bc_nodes,
                    fix_nodes=np.unique(np.concatenate(fix_nodes)) if fix_nodes else np.array([], int),
                    disp_nodes=np.unique(np.concatenate(disp_nodes)) if disp_nodes else np.array([], int),
                    impact=impact, thermal=thermal, desc=load_desc, sel_faces=sel_faces)

    def _vector(self, ld, faces, mesh):
        if "vector" in ld and ld["vector"] is not None:
            return np.asarray(ld["vector"], float)
        mag = float(ld.get("value", ld.get("magnitude", 0.0)))
        return mag * direction(ld.get("direction", "-z"), mesh, faces)

    # ------------------------------------------------------------------
    def _run_case(self, case, name, solvers, bc, prog):
        model, mesh, mf, m = self.model, self.model.mesh, self.mf, self.m
        self.warnings = list(bc.get("pre_warnings", []))
        nn = mesh.n_nodes
        duration = str(case.get("duration", "short")).lower()
        T = case.get("temperature")
        f_T, wT = mat.temperature_factor(m, T)
        if wT:
            self.warnings.append(wT)
        f_str = f_T
        m_case = f_T
        cycles = None
        if duration == "long":
            f_str *= m["creep_strength"]
            m_case *= m["creep_modulus"]
        elif duration == "cyclic":
            cycles = float(case.get("cycles", 1e5))
            f_str *= mat.fatigue_factor(m, cycles)
        # массовые силы (вес / перегрузка)
        f_body = np.zeros(3 * nn)
        if bc["body"]:
            rho_t = m["density"] * 1e-9  # т/мм³
            emass = (mf.rs + mf.rp) * mesh.elem_vol * rho_t
            for gv in bc["body"]:
                acc = gv * G
                fe = emass[:, None] * acc[None, :] / 8.0
                for a in range(8):
                    np.add.at(f_body, (3 * mesh.elem_nodes[:, a, None] + np.arange(3)[None, :]).ravel(), fe.ravel())
        # температурное расширение
        f_th = np.zeros(3 * nn)
        eps_th = None
        if bc["thermal"] and T is not None:
            dT = float(T) - mat.T_REF
            eps_th = m["cte"] * dT * np.array([1, 1, 1, 0, 0, 0.0])
            dofs = mesh.dofs()
            for si, basis in enumerate(mesh.bases):
                ii = np.nonzero(mesh.elem_size_idx == si)[0]
                sig = np.einsum("nij,j->ni", mf.C_full(ii), eps_th)
                fe = sig @ basis.Bint      # (n,24)
                np.add.at(f_th, dofs[ii].ravel(), fe.ravel())
        f_ext = bc["f_force"] + f_body
        Cvec_case = self.Cvec
        key = bc["fixed"].tobytes()
        if key not in solvers:
            prog(0.05, "подготовка решателя")
            under = any(not bc["fixed"][c::3].any() for c in range(3))
            solvers[key] = (Solver(self.K, bc["fixed"], mesh.xyz, cells=mesh.node_ijk,
                                   springs=1e-7 if under else 1e-12), self.K)
        solver, Kc = solvers[key]
        prog(0.35, "решение системы")
        rhs = f_ext / m_case + f_th
        up = bc["uval"]
        rhs_eff = rhs - Kc @ up
        uf, sinfo = solver.solve(rhs_eff[solver.free])
        u = up.copy()
        u[solver.free] = uf
        k_imp = 1.0
        if bc["impact"] is not None:
            imp = bc["impact"]
            nodes, w = face_node_weights(mesh, imp["faces"])
            dst = float(((u.reshape(-1, 3)[nodes] @ imp["dir"]) * w).sum() / w.sum())
            if dst <= 1e-9:
                self.warnings.append("Удар: не удалось определить прогиб в точке удара.")
                dst = 1e-9
            k_imp = 1.0 + math.sqrt(1.0 + 2.0 * imp["height"] / dst)
            u = u * k_imp
        prog(0.7, "напряжения и запас прочности")
        sf, mode, vmis, sig_h, eps_max = self._postprocess(u / k_imp, eps_th, Cvec_case)
        sf = sf * f_str / (m_case * k_imp)
        vmis = vmis * m_case * k_imp
        sig_h = sig_h * m_case * k_imp
        U = u.reshape(-1, 3)
        # реакции
        r = m_case * (Kc @ (u / k_imp) - f_th) - f_ext
        R = r.reshape(-1, 3)
        dn = bc["disp_nodes"]
        fn = np.setdiff1d(bc["fix_nodes"], dn)
        reaction = (R[fn].sum(0) * k_imp).tolist()
        disp_force = R[dn].sum(0) if len(dn) else None      # усилие, которое нужно для заданного перемещения
        applied = (f_ext.reshape(-1, 3).sum(0) * k_imp + (disp_force if disp_force is not None else 0)).tolist()
        # элементы у мест приложения нагрузок и закреплений
        bcn = np.zeros(nn, bool)
        for nds in bc["bc_nodes"]:
            bcn[nds] = True
        bc_elem = bcn[mesh.elem_nodes].any(1)
        core = ~bc_elem if (~bc_elem).any() else np.ones_like(bc_elem)
        c = model.vm.centers() - self.origin
        i_min = int(np.argmin(sf))
        i_core = int(np.nonzero(core)[0][np.argmin(sf[core])])
        dmag = np.linalg.norm(U, axis=1)
        i_d = int(np.argmax(dmag))
        sf_core = float(sf[i_core])
        verdict = "ok" if sf_core >= self.target_sf else ("risk" if sf_core >= 1.0 else "fail")
        size = float(np.linalg.norm(model.size))
        if dmag[i_d] > 0.05 * size:
            self.warnings.append("Перемещения больше 5% размера детали — линейный расчёт неточен "
                                 "(реальная деталь будет вести себя нелинейно).")
        if eps_max * m_case * k_imp > 0.5 * m.get("elongation", 0.05) and m["E1"] > 100:
            self.warnings.append("Деформации близки к удлинению при разрыве материала — возможно хрупкое разрушение.")
        if np.linalg.norm(np.asarray(reaction) + np.asarray(applied)) > max(0.02 * np.linalg.norm(applied), 1e-3) \
                and not bc["thermal"]:
            self.warnings.append("Реакции опор не уравновешивают нагрузку — модель закреплена недостаточно.")
        limit = None if bc["thermal"] else self._limit_text(bc["desc"], sf_core)
        if bc["impact"] is not None:
            # допустимая высота падения: k_доп = запас при статической нагрузке весом груза
            k_allow = sf_core * k_imp
            imp = bc["impact"]
            dst = float(((U[face_node_weights(mesh, imp["faces"])[0]] @ imp["dir"])).mean()) / k_imp
            h_allow = max(0.0, ((k_allow - 1.0) ** 2 - 1.0) * dst / 2.0) if k_allow > 2.0 else 0.0
            v = f"{h_allow:.3g}" if h_allow < 100 else f"{h_allow:,.0f}".replace(",", " ")
            limit = dict(text=f"Допустимая высота падения груза ≈ {v} мм".replace(".", ","),
                         short=f"падение с ≈ {v} мм".replace(".", ","), label="Допустимая высота", value=h_allow, unit="мм")
        if disp_force is not None and limit is not None:
            limit["force"] = [round(float(x), 3) for x in disp_force]
        summary = dict(
            name=name, duration=duration, duration_name=DURATION_NAMES.get(duration, duration),
            cycles=cycles, temperature=T, strength_factor=round(f_str, 3), modulus_factor=round(m_case, 3),
            impact_factor=round(k_imp, 3) if bc["impact"] is not None else None,
            sf_min=float(sf[i_min]), sf_min_xyz=[round(float(v), 2) for v in c[i_min]],
            sf=sf_core, sf_xyz=[round(float(v), 2) for v in c[i_core]],
            mode=mat.MODES[int(mode[i_core])], mode_id=int(mode[i_core]),
            sf_min_at_bc=bool(bc_elem[i_min] and sf[i_min] < sf_core * 0.95),
            max_disp=float(dmag[i_d]), max_disp_xyz=[round(float(v), 2) for v in (mesh.xyz[i_d] - self.origin)],
            max_disp_vec=[float(v) for v in U[i_d]],
            max_stress=float(vmis.max()), verdict=verdict, target_sf=self.target_sf,
            reaction=[round(v, 3) for v in reaction], applied=[round(v, 3) for v in applied],
            loads=bc["desc"], limit=limit, solver=sinfo, warnings=list(self.warnings),
            disp_force=[round(float(x), 3) for x in disp_force] if disp_force is not None else None,
            fail_volume_frac=float(((sf < 1.0) * mesh.elem_vol).sum() / mesh.elem_vol.sum()),
        )
        prog(1.0, "готово")
        return CaseResult(name=name, u=U.astype(np.float32), sf=sf.astype(np.float32), mode=mode.astype(np.int8),
                          vm=vmis.astype(np.float32), sigma=sig_h.astype(np.float32), summary=summary,
                          bc_elem=bc_elem, sel_faces=bc["sel_faces"])

    def _limit_text(self, desc, sf):
        """Предельная нагрузка (для случая с одной нагрузкой): линейный расчёт -> нагрузка × запас."""
        if len(desc) != 1:
            return None
        d = desc[0]
        if d["type"] == "impact":
            return None
        val = d["magnitude"] * sf
        names = {"force": "Предельная сила", "bearing": "Предельная сила на отверстие",
                 "mass": "Предельный груз", "pressure": "Предельное давление",
                 "moment": "Предельный момент", "displacement": "Предельное перемещение"}
        nm = names.get(d["type"], "Предельная перегрузка")
        v = f"{val:.3g}" if val < 100 else f"{val:,.0f}".replace(",", " ")
        short = f"≈ {v} {d['unit']}".replace(".", ",")
        return dict(text=f"{nm} {short}", short=short, label=nm, value=val, unit=d["unit"])

    # ------------------------------------------------------------------
    def _postprocess(self, u, eps_th, Cvec):
        mesh, mf = self.model.mesh, self.mf
        ne = mesh.n_elems
        sf = np.full(ne, 1e6)
        mode = np.zeros(ne, np.int8)
        vmis = np.zeros(ne)
        sig_h = np.zeros((ne, 6))
        dofs = mesh.dofs()
        eps_max = 0.0
        hc = mf.hc
        thr = 0.02
        for si, basis in enumerate(mesh.bases):
            idx = np.nonzero(mesh.elem_size_idx == si)[0]
            for a in range(0, len(idx), 4000):
                ii = idx[a:a + 4000]
                ue = u[dofs[ii]]
                ec, em = basis.strains(Cvec[ii], ue)
                if eps_th is not None:
                    ec = ec - eps_th
                    em = em - eps_th
                eps_max = max(eps_max, float(np.abs(ec).max()))
                sig_h[ii] = np.einsum("nij,nj->ni", mf.C_full(ii, Cvec), em)
                n = len(ii)
                best = np.full(n, 1e6)
                best_sig = np.zeros((n, 6))
                # фаза «стенки/сплошное»
                s_sh = np.einsum("bij,ncj->ncbi", mf.Ms, ec)              # (n,8,NB,6)
                lam = mat.hoffman_sf(s_sh, hc)
                # доли стенки < 15% в вокселе — это «перелив» валика из соседнего вокселя, не оцениваем
                mask = (mf.hs[ii] < thr) | (mf.rs[ii] < 0.15)[:, None]
                lam = np.where(mask[:, None, :], 1e6, lam)
                flat = lam.reshape(n, -1)
                j = np.argmin(flat, axis=1)
                v = flat[np.arange(n), j]
                upd = v < best
                best = np.where(upd, v, best)
                best_sig[upd] = s_sh.reshape(n, -1, 6)[np.arange(n), j][upd]
                # фаза «разреженное заполнение»
                if (mf.rp[ii] > 0.02).any():
                    rp = mf.rp[ii]
                    s_sp = np.einsum("bij,ncj->ncbi", mf.M0, ec) + rp[:, None, None, None] * \
                        np.einsum("bij,ncj->ncbi", mf.M1, ec)
                    lam2 = mat.hoffman_sf(s_sp, hc)
                    mask2 = (mf.hp[ii] < thr) | (rp < 0.02)[:, None]
                    lam2 = np.where(mask2[:, None, :], 1e6, lam2)
                    flat2 = lam2.reshape(n, -1)
                    j2 = np.argmin(flat2, axis=1)
                    v2 = flat2[np.arange(n), j2]
                    upd2 = v2 < best
                    best = np.where(upd2, v2, best)
                    best_sig[upd2] = s_sp.reshape(n, -1, 6)[np.arange(n), j2][upd2]
                sf[ii] = best
                mode[ii] = mat.failure_mode(best_sig, self.m)
                vmis[ii] = _vm(best_sig)
        return sf, mode, vmis, sig_h, eps_max


def run(model: Model, job: dict, progress=None):
    a = Analysis(model, job, progress=progress)
    res = a.run()
    return a, res
