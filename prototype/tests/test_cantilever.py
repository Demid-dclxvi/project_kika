"""Проверка на аналитике: консольная балка 100×10×10 мм (PLA), сила на конце.
Печать плашмя (все нити вдоль балки) и стоя (балка вдоль Z)."""
import os, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE)); sys.path.insert(0, HERE)
import numpy as np, trimesh
from mini_slicer import slice_mesh
from fdmfea.analysis import Model, run
from fdmfea import materials as mat

def make(upright, voxel, infill=1.0, pattern="grid"):
    ext = (10, 10, 100) if upright else (100, 10, 10)
    mesh = trimesh.creation.box(ext)
    txt, _ = slice_mesh(mesh, flavor="orca", infill=infill, solid_angles=(0,), walls=2, pattern=pattern)
    return Model(text=txt, voxel=voxel)

def analytic(E, G, F=50.0, L=100.0, b=10.0, h=10.0):
    I = b * h**3 / 12
    d = F * L**3 / (3 * E * I) + F * L / (5/6 * G * b * h)
    sig = F * L * (h / 2) / I
    return d, sig

if __name__ == "__main__":
    m = mat.material_from("PLA")
    for upright in (False, True):
        for vox in (1.0, 0.7):
            t = time.time()
            model = make(upright, vox)
            if upright:
                job = {"material": "PLA", "cases": [{"name": "изгиб", "fixtures": [{"where": "zmin"}],
                        "loads": [{"type": "force", "where": "zmax", "vector": [50, 0, 0]}]}]}
                d_an, s_an = analytic(m["E3"], m["G13"])
                sf_an = m["Zt"] / s_an
            else:
                job = {"material": "PLA", "cases": [{"name": "изгиб", "fixtures": [{"where": "xmin"}],
                        "loads": [{"type": "force", "where": "xmax", "vector": [0, 0, -50]}]}]}
                d_an, s_an = analytic(m["E1"], m["G13"])
                sf_an = m["Xt"] / s_an
            a, res = run(model, job)
            s = res[0].summary
            # запас посередине пролёта (там M = F·L/2, нет влияния заделки)
            c = model.vm.centers() - model.origin
            ax = 2 if upright else 0
            mid = np.abs(c[:, ax] - 50.0) < model.vm.s
            sf_mid = float(res[0].sf[mid].min())
            # прогиб конца: среднее по торцу
            print(f"{'стоя ' if upright else 'плашмя'} воксель {model.vm.s:.2f} эл {model.vm.n:6d}  "
                  f"прогиб {s['max_disp']:.3f} (аналит {d_an:.3f}, {100*(s['max_disp']/d_an-1):+.1f}%)  "
                  f"запас: середина {sf_mid:.3f} (аналит {2*sf_an:.3f}, {100*(sf_mid/(2*sf_an)-1):+.1f}%), у заделки {s['sf']:.3f} (аналит {sf_an:.3f})  "
                  f"вид: {s['mode']}  решатель {s['solver']['method']} {s['solver']['time']:.1f}с  всего {time.time()-t:.1f}с")
            print("     реакции", s['reaction'], 'приложено', s['applied'], s['warnings'])
