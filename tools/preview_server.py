#!/usr/bin/env python3
"""Local preview of the phone web UI with a SIMULATED data feed (no board needed).
   python3 tools/preview_server.py  ->  open http://127.0.0.1:8080/
The motion numbers here are fake - they only exercise the UI."""
import json, math, random, time, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"
T0 = time.time()
SENS = [5]
HYST = {"motion": False, "last": 0.0}

def level(t):
    c = t % 45
    if c < 12: return 2 + random.random() * 3                      # still room
    if c < 30: return max(0, 55 + 30 * math.sin((c - 12) / 2.2) + random.uniform(-8, 8))  # someone walking
    if c < 36: return max(0, 30 - (c - 30) * 5 + random.uniform(-4, 4))
    return 1 + random.random() * 3

def state():
    t = time.time() - T0
    m = min(100, level(t))
    if m >= 25: HYST["motion"], HYST["last"] = True, t          # same hysteresis as the firmware
    elif m >= 12: HYST["last"] = t
    elif HYST["motion"] and t - HYST["last"] > 2: HYST["motion"] = False
    return {"m": m, "p": min(100, m * 0.8 + random.uniform(0, 8)), "s": "motion" if HYST["motion"] else "still",
            "cal": False, "cp": 1, "rssi": -52 + random.uniform(-2, 2), "pps": 49 + random.randint(-2, 2),
            "score": m / 10, "base": 0.4, "sig": 0.05, "sens": SENS[0], "ip": "192.168.1.73",
            "ssid": "HomeWiFi", "ap": "ESP-Radar-3F2A", "up": int(t) + 754, "setup": False, "sta": True, "v": "preview"}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, ctype, body):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def do_GET(self):
        p = self.path.split("?")[0]
        if p in ("/", "/radar"): return self._send(200, "text/html", (WEB / "radar.html").read_text())
        if p == "/setup": return self._send(200, "text/html", (WEB / "setup.html").read_text())
        if p == "/api/state": return self._send(200, "application/json", json.dumps(state()))
        if p == "/api/events":
            now = int((time.time() - T0) * 1000) + 754000
            ev = [{"t": now - 600000, "start": True, "peak": 0}, {"t": now - 540000, "start": False, "peak": 81},
                  {"t": now - 200000, "start": True, "peak": 0}, {"t": now - 170000, "start": False, "peak": 64}]
            return self._send(200, "application/json", json.dumps({"now": now, "ev": ev}))
        if p == "/api/scan":
            nets = [{"ssid": "HomeWiFi", "rssi": -48, "open": False}, {"ssid": "Neighbor-5G", "rssi": -71, "open": False}, {"ssid": "CoffeeShop", "rssi": -80, "open": True}]
            return self._send(200, "application/json", json.dumps({"scanning": False, "nets": nets}))
        if p == "/events":
            self.send_response(200); self.send_header("Content-Type", "text/event-stream"); self.send_header("Cache-Control", "no-cache"); self.end_headers()
            try:
                while True:
                    self.wfile.write(f"event: state\ndata: {json.dumps(state())}\n\n".encode()); self.wfile.flush(); time.sleep(0.125)
            except (BrokenPipeError, ConnectionResetError):
                return
        self._send(404, "text/plain", "not found")
    def do_POST(self):
        n = int(self.headers.get("Content-Length") or 0); body = self.rfile.read(n).decode()
        if self.path == "/api/sens" and body.startswith("v="): SENS[0] = int(body[2:])
        self._send(200, "application/json", '{"ok":true}')

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"preview (SIMULATED data) on http://127.0.0.1:{port}/")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
