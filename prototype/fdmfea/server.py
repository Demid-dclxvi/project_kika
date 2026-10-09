"""Локальное веб-приложение: python -m fdmfea gui

Работает только на этом компьютере (127.0.0.1), интернет не нужен.
"""
from __future__ import annotations

import gzip
import json
import mimetypes
import os
import threading
import time
import traceback
import uuid
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse, unquote

from . import __version__
from .analysis import Model, Analysis
from .export import model_payload, results_payload, report_html
from .gcode import parse_gcode
from .materials import load_db

WEB = os.path.join(os.path.dirname(__file__), "web")
EXAMPLES = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "examples"))


class Task:
    def __init__(self, kind):
        self.id = uuid.uuid4().hex[:12]
        self.kind = kind
        self.frac = 0.0
        self.text = "Запуск"
        self.done = False
        self.error = None
        self.result = None
        self.started = time.time()

    def progress(self, stage, frac, text):
        self.frac = float(frac)
        self.text = text

    def status(self):
        return dict(id=self.id, kind=self.kind, frac=round(self.frac, 3), text=self.text, done=self.done,
                    error=self.error, elapsed=round(time.time() - self.started, 1))


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.tasks: dict[str, Task] = {}
        self.toolpaths = None
        self.model = None
        self.gcode_name = ""
        self.analysis = None
        self.results = None
        self.job = None

    def run_task(self, kind, fn):
        t = Task(kind)
        self.tasks[t.id] = t

        def work():
            try:
                t.result = fn(t)
            except Exception as e:  # noqa: BLE001
                traceback.print_exc()
                t.error = str(e) or e.__class__.__name__
            finally:
                t.frac = 1.0
                t.done = True
        threading.Thread(target=work, daemon=True).start()
        # старые задачи чистим
        for k in [k for k, v in self.tasks.items() if v.done and time.time() - v.started > 3600]:
            self.tasks.pop(k, None)
        return t


STATE = State()


def _build_model(task, text=None, name="", voxel=None, max_elems=120_000):
    st = STATE
    if text is not None:
        task.progress("parse", 0.05, "Читаю G-code")
        tp = parse_gcode(text)
        st.toolpaths = tp
        st.gcode_name = name
    if st.toolpaths is None:
        raise ValueError("Сначала откройте файл G-code.")
    model = Model(toolpaths=st.toolpaths, voxel=voxel, max_elems=max_elems,
                  progress=lambda s, f, t: task.progress(s, 0.1 + 0.85 * f, t))
    st.model = model
    st.analysis = None
    st.results = None
    task.progress("send", 0.97, "Передаю модель")
    return dict(model=model_payload(model), gcode_name=st.gcode_name, material_guess=model.material_guess)


def _solve(task, job):
    st = STATE
    if st.model is None:
        raise ValueError("Модель не загружена.")
    an = Analysis(st.model, job, progress=lambda s, f, t: task.progress(s, f, t))
    res = an.run()
    st.analysis, st.results, st.job = an, res, job
    return results_payload(an, res)


def _resolve_job(job):
    """Геометрические описания областей (side/box/hole…) -> списки граней текущей модели."""
    from .loads import select_faces
    st = STATE
    if st.model is None:
        raise ValueError("Сначала откройте G-code детали.")
    mesh, origin = st.model.mesh, st.model.origin if job.get("coords", "part") == "part" else None
    if origin is None:
        import numpy as np
        origin = np.zeros(3)

    def res(where, name):
        if where is None or (isinstance(where, dict) and "faces" in where):
            return where
        idx = select_faces(mesh, where, origin, name)
        return {"faces": [int(k) for k in mesh.face_key[idx]]}
    out = json.loads(json.dumps(job))
    for i, fx in enumerate(out.get("fixtures") or []):
        fx["where"] = res(fx.get("where"), f"Закрепление {i + 1}")
    for c in out.get("cases") or []:
        for fx in c.get("fixtures") or []:
            fx["where"] = res(fx.get("where"), "Закрепление")
        for j, ld in enumerate(c.get("loads") or []):
            if "where" in ld:
                ld["where"] = res(ld.get("where"), f"{c.get('name', '')}: нагрузка {j + 1}")
    out["coords"] = "part"
    return out


class Handler(BaseHTTPRequestHandler):
    server_version = "fdmfea/" + __version__

    def log_message(self, fmt, *args):  # тише в консоли
        if os.environ.get("FDMFEA_DEBUG"):
            super().log_message(fmt, *args)

    # ---------------- ответы
    def _send(self, code, body: bytes, ctype="application/json; charset=utf-8", extra=None):
        gz = "gzip" in (self.headers.get("Accept-Encoding") or "") and len(body) > 1024 \
            and not ctype.startswith(("image/", "application/octet-stream"))
        if gz:
            body = gzip.compress(body, 5)
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        if gz:
            self.send_header("Content-Encoding", "gzip")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8"))

    def _err(self, msg, code=400):
        self._json({"error": msg}, code)

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    # ---------------- маршруты
    def do_GET(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        path = u.path
        if path in ("/", "/index.html"):
            return self._static("app.html")
        if path.startswith("/static/"):
            return self._static(unquote(path[len("/static/"):]))
        if path == "/api/materials":
            return self._json(load_db())
        if path == "/api/task":
            t = STATE.tasks.get((q.get("id") or [""])[0])
            if not t:
                return self._err("Задача не найдена", 404)
            return self._json(t.status())
        if path == "/api/task_result":
            t = STATE.tasks.get((q.get("id") or [""])[0])
            if not t or not t.done:
                return self._err("Результат ещё не готов", 404)
            if t.error:
                return self._err(t.error, 500)
            return self._json(t.result)
        if path == "/api/report":
            st = STATE
            if not st.results:
                return self._err("Сначала выполните расчёт.", 404)
            title = (st.job or {}).get("title") or (os.path.splitext(st.gcode_name)[0] or "Деталь")
            html = report_html(st.model, st.analysis, st.results, st.job or {}, title=title, gcode_name=st.gcode_name)
            fname = (os.path.splitext(st.gcode_name)[0] or "деталь") + "_отчёт.html"
            from urllib.parse import quote
            return self._send(200, html.encode("utf-8"), "text/html; charset=utf-8",
                              {"Content-Disposition": f"attachment; filename*=UTF-8''{quote(fname)}"})
        if path == "/api/examples":
            items = []
            if os.path.isdir(EXAMPLES):
                for f in sorted(os.listdir(EXAMPLES)):
                    if f.endswith(".gcode"):
                        base = f[:-6]
                        job = os.path.join(EXAMPLES, base + ".job.json")
                        items.append(dict(name=f, job=os.path.exists(job)))
            return self._json(items)
        if path == "/api/example_job":
            name = (q.get("name") or [""])[0]
            p = os.path.join(EXAMPLES, os.path.basename(name).replace(".gcode", "") + ".job.json")
            if not os.path.exists(p):
                return self._err("Нет задания для примера", 404)
            with open(p, "r", encoding="utf-8") as f:
                job = json.load(f)
            try:
                return self._json(_resolve_job(job))
            except Exception as e:  # noqa: BLE001
                return self._err(str(e), 400)
        if path == "/api/version":
            return self._json({"version": __version__})
        return self._err("Не найдено", 404)

    def do_POST(self):
        u = urlparse(self.path)
        q = parse_qs(u.query)
        path = u.path
        try:
            if path == "/api/model":
                raw = self._body()
                name = unquote(self.headers.get("X-Filename") or "деталь.gcode")
                text = raw.decode("utf-8", errors="replace")
                voxel = float(q["voxel"][0]) if q.get("voxel") and q["voxel"][0] else None
                max_el = int(q.get("max_elems", ["120000"])[0])
                t = STATE.run_task("model", lambda task: _build_model(task, text, name, voxel, max_el))
                return self._json(t.status())
            if path == "/api/example":
                name = os.path.basename((q.get("name") or ["bracket_side.gcode"])[0])
                p = os.path.join(EXAMPLES, name)
                if not os.path.exists(p):
                    return self._err("Пример не найден", 404)
                with open(p, "r", encoding="utf-8") as f:
                    text = f.read()
                max_el = int(q.get("max_elems", ["120000"])[0])
                t = STATE.run_task("model", lambda task: _build_model(task, text, name, None, max_el))
                return self._json(t.status())
            if path == "/api/rebuild":
                body = json.loads(self._body() or b"{}")
                voxel = body.get("voxel")
                max_el = int(body.get("max_elems") or 120_000)
                t = STATE.run_task("model", lambda task: _build_model(task, None, "", voxel, max_el))
                return self._json(t.status())
            if path == "/api/resolve_job":
                job = json.loads(self._body() or b"{}")
                return self._json(_resolve_job(job))
            if path == "/api/solve":
                job = json.loads(self._body() or b"{}")
                t = STATE.run_task("solve", lambda task: _solve(task, job))
                return self._json(t.status())
        except Exception as e:  # noqa: BLE001
            traceback.print_exc()
            return self._err(str(e), 400)
        return self._err("Не найдено", 404)

    def _static(self, rel):
        p = os.path.normpath(os.path.join(WEB, rel))
        if not p.startswith(os.path.normpath(WEB)) or not os.path.isfile(p):
            return self._err("Не найдено", 404)
        ctype = mimetypes.guess_type(p)[0] or "application/octet-stream"
        if ctype.startswith("text/") or ctype in ("application/javascript",):
            ctype += "; charset=utf-8"
        with open(p, "rb") as f:
            self._send(200, f.read(), ctype)


def serve(port=8765, open_browser=True, host="127.0.0.1"):
    mimetypes.add_type("application/javascript", ".js")
    mimetypes.add_type("text/css", ".css")
    httpd = None
    for p in range(port, port + 20):
        try:
            httpd = ThreadingHTTPServer((host, p), Handler)
            port = p
            break
        except OSError:
            continue
    if httpd is None:
        raise SystemExit("Не удалось занять порт для приложения.")
    url = f"http://{'127.0.0.1' if host in ('0.0.0.0', '') else host}:{port}/"
    print(f"Прочность печати {__version__}: откройте в браузере {url}")
    print("Остановить — Ctrl+C в этом окне.")
    if open_browser:
        threading.Timer(0.8, lambda: webbrowser.open(url)).start()
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\nОстановлено.")
