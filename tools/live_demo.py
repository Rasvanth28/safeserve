#!/usr/bin/env python3
"""Live demo controller: runs one SafeServe per strategy behind a single
front door, and lets the dashboard restart them with buttons.

    http://localhost:8080/naive/       -> SafeServe --mode naive     (127.0.0.1:8081)
    http://localhost:8080/lockorder/   -> SafeServe --mode lockorder (127.0.0.1:8082)
    http://localhost:8080/detect/      -> SafeServe --mode detect    (127.0.0.1:8083)
    http://localhost:8080/banker/      -> SafeServe --mode banker    (127.0.0.1:8084)
    http://localhost:8080/control/...  -> restart / setup / status (used by the dashboard)

One public port is all a host like Render gives a service, so everything goes
through it. Setups: "classroom" (1 DB, 1 CLOCK, each upload/refresh holds its
first resource 4 s, so a deadlock can be built by clicking) or "fast" (the
normal configs).

Locally: double-click "Start Live Demo.app" (or `make live`); "Stop demo" on the
dashboard stops it. Hosted (--hosted, used by the Dockerfile): listens on $PORT
and the stop button is disabled, since every visitor shares the servers.
"""
import argparse
import http.client
import json
import os
import signal
import subprocess
import sys
import threading
import time
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SAFESERVE = os.environ.get("SAFESERVE", os.path.join(ROOT, "bin", "safeserve"))
SERVERS = [("naive", 8081), ("lockorder", 8082), ("detect", 8083), ("banker", 8084)]
PORT_OF = dict(SERVERS)
SETUPS = {
    "classroom": ["--config", "config/classroom-server.conf", "--resources", "config/classroom-resources.conf"],
    "fast": ["--config", "config/server.conf", "--resources", "config/resources.conf"],
}
# A deadlocked naive request never answers; the dashboard gives up after 12 s
PROXY_TIMEOUT_S = 15

lock = threading.Lock()
procs = {}            # mode -> Popen
started = {}          # mode -> time.time() of the last (re)start
setup = "classroom"
hosted = False
stopping = threading.Event()


def responding(port, timeout=0.5):
    # /metrics, not /health: the server doesn't count /metrics, so these checks
    # don't show up in the dashboard's request counters
    try:
        with urllib.request.urlopen(f"http://127.0.0.1:{port}/metrics", timeout=timeout):
            return True
    except Exception:
        return False


def stop_one(mode):
    p = procs.pop(mode, None)
    if p is None or p.poll() is not None:
        return
    # A deadlocked naive server may never finish a graceful shutdown, and the
    # demo needs the port back quickly: ask nicely, then force it.
    p.send_signal(signal.SIGINT)
    try:
        p.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        p.kill()
        p.wait()


def start_one(mode):
    port = PORT_OF[mode]
    log = open(f"/tmp/safeserve_{mode}.log", "w")
    procs[mode] = subprocess.Popen(
        [SAFESERVE, *SETUPS[setup], "--mode", mode, "--port", str(port), "--quiet"],
        cwd=ROOT, stdout=log, stderr=subprocess.STDOUT)
    started[mode] = time.time()
    for _ in range(30):                     # wait until it answers (max 3 s)
        if responding(port, 0.2):
            break
        time.sleep(0.1)


def restart(modes):
    with lock:
        for m in modes:
            stop_one(m)
        for m in modes:
            start_one(m)


def status():
    with lock:
        servers = []
        for mode, port in SERVERS:
            p = procs.get(mode)
            alive = p is not None and p.poll() is None
            servers.append({"mode": mode, "port": port, "running": alive,
                            "responding": alive and responding(port, 0.4),
                            "uptime_s": round(time.time() - started.get(mode, time.time()), 1)})
        return {"setup": setup, "hosted": hosted, "servers": servers}


class FrontDoor(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *args):
        pass

    def send_body(self, code, body, ctype, extra=()):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != "HEAD":
            self.wfile.write(body)

    def reply(self, code, obj):
        # CORS: a dashboard opened straight from a server port (8081-8084) calls us cross-origin
        self.send_body(code, json.dumps(obj).encode(), "application/json",
                       [("Access-Control-Allow-Origin", "*")])

    def redirect(self, where):
        self.send_body(302, b"", "text/plain", [("Location", where)])

    def route(self):
        url = urlparse(self.path)
        parts = url.path.split("/", 2)            # "", first segment, rest
        first = parts[1] if len(parts) > 1 else ""
        if url.path in ("/", "/index.html"):
            return self.redirect("/naive/")
        if url.path == "/healthz":
            return self.send_body(200, b"ok\n", "text/plain")
        if first == "control":
            return self.control(url)
        if first in PORT_OF:
            if len(parts) == 2:                   # "/naive" -> "/naive/"
                return self.redirect(f"/{first}/" + (f"?{url.query}" if url.query else ""))
            return self.proxy(PORT_OF[first], "/" + parts[2] + (f"?{url.query}" if url.query else ""))
        self.reply(404, {"error": "not found"})

    do_GET = do_POST = do_HEAD = do_PUT = do_DELETE = route

    def proxy(self, port, path):
        length = int(self.headers.get("Content-Length") or 0)
        body = self.rfile.read(length) if length else None
        headers = {k: v for k, v in self.headers.items()
                   if k.lower() in ("content-type", "content-length", "accept")}
        try:
            conn = http.client.HTTPConnection("127.0.0.1", port, timeout=PROXY_TIMEOUT_S)
            conn.request(self.command, path, body=body, headers=headers)
            resp = conn.getresponse()
            data = resp.read()
            conn.close()
        except (TimeoutError, OSError):
            # Stuck in a deadlock, frozen, or restarting
            return self.send_body(504, b"504 Gateway Timeout: the server did not answer (deadlocked?)\n",
                                  "text/plain; charset=utf-8")
        extra = [(k, v) for k, v in resp.getheaders()
                 if k.lower() in ("retry-after", "allow")]
        self.send_body(resp.status, data, resp.getheader("Content-Type", "application/octet-stream"), extra)

    def control(self, url):
        global setup
        q = {k: v[0] for k, v in parse_qs(url.query).items()}
        modes = [m for m, _ in SERVERS]
        if url.path == "/control/status":
            return self.reply(200, status())
        if self.command != "POST":
            return self.reply(405, {"error": "use POST"})
        if url.path == "/control/restart":
            target = q.get("mode", "all")
            if target != "all" and target not in modes:
                return self.reply(400, {"error": "unknown mode"})
            restart(modes if target == "all" else [target])
            return self.reply(200, status())
        if url.path == "/control/setup":
            if q.get("value") not in SETUPS:
                return self.reply(400, {"error": "setup must be classroom or fast"})
            setup = q["value"]
            restart(modes)
            return self.reply(200, status())
        if url.path == "/control/shutdown":
            if hosted:
                return self.reply(403, {"error": "the hosted demo can't be stopped from the page"})
            self.reply(200, {"stopped": True})
            threading.Thread(target=shutdown, daemon=True).start()
            return
        self.reply(404, {"error": "not found"})


def shutdown(*_):
    if stopping.is_set():
        return
    stopping.set()
    print("\nStopping all four servers...")
    with lock:
        for m, _ in SERVERS:
            stop_one(m)
    os._exit(0)


def main():
    global setup, hosted
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--setup", choices=SETUPS, default="classroom")
    ap.add_argument("--port", type=int, default=int(os.environ.get("PORT", 8080)),
                    help="public port (default: $PORT, else 8080)")
    ap.add_argument("--host", default="127.0.0.1", help="address to listen on (0.0.0.0 in a container)")
    ap.add_argument("--hosted", action="store_true", help="shared public deployment: disable 'Stop demo'")
    ap.add_argument("--open", action="store_true", help="open the dashboard in the browser")
    args = ap.parse_args()
    setup, hosted = args.setup, args.hosted

    if not os.access(SAFESERVE, os.X_OK):
        sys.exit("Build first: make")
    busy = [p for _, p in SERVERS if responding(p, 0.3)]
    if busy:
        sys.exit(f"Ports already in use: {busy}. Is another demo still running?")
    try:
        server = ThreadingHTTPServer((args.host, args.port), FrontDoor)
    except OSError as e:
        sys.exit(f"Can't listen on port {args.port}: {e}. Is another demo still running?")
    server.daemon_threads = True
    signal.signal(signal.SIGINT, shutdown)
    signal.signal(signal.SIGTERM, shutdown)
    restart([m for m, _ in SERVERS])

    url = f"http://localhost:{args.port}/"
    print(f"SafeServe live demo is running ({setup} setup) at {url}")
    for m, _ in SERVERS:
        print(f"  {m:10s} {url}{m}/")
    if not hosted:
        print("Everything else is done from the dashboard; 'Stop demo' stops all servers.")
    if args.open:
        webbrowser.open(url)
    server.serve_forever()


if __name__ == "__main__":
    main()
