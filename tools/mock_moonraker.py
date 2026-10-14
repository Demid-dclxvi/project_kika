#!/usr/bin/env python3
"""Имитация принтера с Moonraker (Klipper) для проверки отправки на печать без настоящего принтера.

    python tools/mock_moonraker.py --port 7125 --dir /tmp/gcodes

Отвечает как Moonraker: /server/info, /printer/info, /printer/objects/query, /server/files/upload
(multipart, print=true запускает «печать»), /printer/print/start|pause|resume|cancel. «Печать» идёт
по времени: за --speed секунд — весь файл. Присланные файлы сохраняются в --dir.
"""

import argparse
import json
import os
import re
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

STATE = {
    "state": "standby",
    "filename": "",
    "started": 0.0,
    "paused_at": None,
    "paused_total": 0.0,
    "layers": 0,
    "nozzle_target": 0.0,
    "bed_target": 0.0,
}
LOCK = threading.Lock()
ARGS = None


def progress():
    if STATE["state"] not in ("printing", "paused"):
        return 1.0 if STATE["state"] == "complete" else 0.0
    now = STATE["paused_at"] or time.time()
    p = (now - STATE["started"] - STATE["paused_total"]) / ARGS.speed
    if p >= 1.0 and STATE["state"] == "printing":
        STATE["state"] = "complete"
        STATE["nozzle_target"] = STATE["bed_target"] = 0.0
        return 1.0
    return max(0.0, min(p, 1.0))


def start_print(name):
    path = os.path.join(ARGS.dir, name)
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"SET_PRINT_STATS_INFO TOTAL_LAYER=(\d+)", text)
    STATE.update(state="printing", filename=name, started=time.time(), paused_at=None, paused_total=0.0,
                 layers=int(m.group(1)) if m else 0)
    m = re.search(r"HOTEND=(\d+)", text)
    STATE["nozzle_target"] = float(m.group(1)) if m else 0.0
    m = re.search(r"BED=(\d+)", text)
    STATE["bed_target"] = float(m.group(1)) if m else 0.0


def multipart(body, ctype):
    boundary = re.search(r"boundary=(.+)", ctype).group(1).strip('"').encode()
    fields, files = {}, {}
    for part in body.split(b"--" + boundary):
        part = part.strip(b"\r\n")
        if not part or part == b"--":
            continue
        head, _, data = part.partition(b"\r\n\r\n")
        head = head.decode("utf-8", "replace")
        name = re.search(r'name="([^"]*)"', head).group(1)
        fn = re.search(r'filename="([^"]*)"', head)
        if fn:
            files[name] = (fn.group(1), data)
        else:
            fields[name] = data.decode("utf-8")
    return fields, files


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):
        print("mock-moonraker:", fmt % args, flush=True)

    def reply(self, obj, status=200, wrap=True):
        body = json.dumps({"result": obj} if wrap else obj).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def error(self, status, msg):
        self.reply({"error": {"code": status, "message": msg}}, status, wrap=False)

    def page(self):
        # простая страница вместо Fluidd/Mainsail — для вкладки «Принтер» без настоящего принтера
        p = progress()
        state = STATE["state"]
        name = STATE["filename"] or "—"
        html = f"""<!doctype html><html lang="ru"><head><meta charset="utf-8">
<title>mock-q2 — имитация Moonraker</title><meta http-equiv="refresh" content="3">
<style>body{{font-family:sans-serif;background:#1e2227;color:#e8e8e8;margin:0}}
header{{background:#2b3038;padding:14px 20px;font-weight:600}} main{{padding:20px;display:grid;
grid-template-columns:1fr 1fr;gap:16px}} .card{{background:#2b3038;border-radius:8px;padding:16px}}
.bar{{height:10px;background:#3a404a;border-radius:5px}} .fill{{height:10px;background:#2ea8df;border-radius:5px;
width:{p * 100:.0f}%}} h3{{margin:0 0 10px 0;font-size:14px;color:#9aa3ad;text-transform:uppercase}}</style></head>
<body><header>mock-q2 · имитация веб-интерфейса принтера (Fluidd, Mainsail)</header><main>
<div class="card"><h3>Печать</h3><p>Состояние: <b>{state}</b></p><p>Файл: {name}</p>
<div class="bar"><div class="fill"></div></div><p>{p * 100:.0f} %</p></div>
<div class="card"><h3>Температуры</h3><p>Сопло: {STATE["nozzle_target"] or 25:.0f} °C</p>
<p>Стол: {STATE["bed_target"] or 24:.0f} °C</p><p>Камера: 31 °C</p></div>
<div class="card"><h3>Камера</h3><p style="color:#9aa3ad">нет изображения в имитации</p></div>
<div class="card"><h3>Консоль</h3><p style="font-family:monospace;color:#9aa3ad">// Klipper: ready</p></div>
</main></body></html>"""
        body = html.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        u = urlparse(self.path)
        with LOCK:
            if u.path in ("/", "/index.html"):
                return self.page()
            if u.path == "/server/info":
                return self.reply({"klippy_connected": True, "klippy_state": "ready", "moonraker_version": "mock"})
            if u.path == "/printer/info":
                return self.reply({"state": "ready", "state_message": "Printer is ready", "hostname": "mock-q2",
                                   "software_version": "mock-klipper"})
            if u.path == "/printer/objects/query":
                p = progress()
                t = time.time()
                return self.reply({"eventtime": t, "status": {
                    "print_stats": {"state": STATE["state"], "filename": STATE["filename"],
                                    "print_duration": max(0.0, (STATE["paused_at"] or t) - STATE["started"]) if STATE["filename"] else 0.0,
                                    "total_duration": 0.0, "message": "",
                                    "info": {"total_layer": STATE["layers"] or None,
                                             "current_layer": int(p * STATE["layers"]) if STATE["layers"] else None}},
                    "virtual_sdcard": {"progress": p},
                    "extruder": {"temperature": STATE["nozzle_target"] or 25.0, "target": STATE["nozzle_target"]},
                    "heater_bed": {"temperature": STATE["bed_target"] or 24.0, "target": STATE["bed_target"]},
                    "heater_generic chamber": {"temperature": 31.0, "target": 0.0}}})
        return self.error(404, "Not Found")

    def do_POST(self):
        u = urlparse(self.path)
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length)
        with LOCK:
            if u.path == "/server/files/upload":
                fields, files = multipart(body, self.headers.get("Content-Type", ""))
                if "file" not in files:
                    return self.error(400, "No file")
                name, data = files["file"]
                os.makedirs(ARGS.dir, exist_ok=True)
                with open(os.path.join(ARGS.dir, name), "wb") as f:
                    f.write(data)
                started = False
                if fields.get("print") == "true" and STATE["state"] not in ("printing", "paused"):
                    start_print(name)
                    started = True
                print(f"mock-moonraker: принят {name} ({len(data)} байт), печать: {started}", flush=True)
                return self.reply({"item": {"path": name, "root": fields.get("root", "gcodes"), "size": len(data)},
                                   "print_started": started, "print_queued": False, "action": "create_file"},
                                  201, wrap=False)
            if u.path == "/printer/print/start":
                name = unquote(parse_qs(u.query).get("filename", [""])[0])
                if not os.path.exists(os.path.join(ARGS.dir, name)):
                    return self.error(400, f"File {name} does not exist")
                start_print(name)
                return self.reply("ok")
            if u.path == "/printer/print/pause" and STATE["state"] == "printing":
                STATE["state"], STATE["paused_at"] = "paused", time.time()
                return self.reply("ok")
            if u.path == "/printer/print/resume" and STATE["state"] == "paused":
                STATE["paused_total"] += time.time() - STATE["paused_at"]
                STATE["state"], STATE["paused_at"] = "printing", None
                return self.reply("ok")
            if u.path == "/printer/print/cancel" and STATE["state"] in ("printing", "paused"):
                STATE["state"] = "cancelled"
                STATE["nozzle_target"] = STATE["bed_target"] = 0.0
                return self.reply("ok")
        return self.error(400, "Printer not in the right state")


def main():
    global ARGS
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=7125)
    ap.add_argument("--dir", default="mock_gcodes")
    ap.add_argument("--speed", type=float, default=600.0, help="за сколько секунд «печатается» файл")
    ARGS = ap.parse_args()
    srv = ThreadingHTTPServer((ARGS.host, ARGS.port), Handler)
    print(f"mock-moonraker: http://{ARGS.host}:{ARGS.port}", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
