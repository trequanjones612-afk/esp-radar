#!/usr/bin/env python3
"""Local preview of the phone web UI with a SIMULATED data feed (no board needed).
   python3 tools/preview_server.py [port]  ->  open http://127.0.0.1:8080/
Every number served here is fake - it only exercises the UI."""
import json, math, random, time, sys
from http.server import ThreadingHTTPServer, BaseHTTPRequestHandler
from pathlib import Path

WEB = Path(__file__).resolve().parent.parent / "web"
T0 = time.time(); UP0 = 3 * 3600 * 1000      # pretend the board has been up for 3 h
SENS = [5]; HYST = {"motion": False, "last": 0.0}
SAMPLE = {"id": 0, "start": 0.0}
DEVSCAN = {"start": 0.0}
random.seed(7)
ACT = [[int(max(0, 30 * math.sin(i / 6) + random.uniform(-6, 10))), 0, 0] for i in range(59)]
for a in ACT: a[1] = min(100, a[0] + random.randint(5, 30)); a[2] = 60 if a[0] > 25 else (10 if a[0] > 15 else 0)

def bms(): return int((time.time() - T0) * 1000) + UP0

def level(t):
    c = t % 45
    if c < 12: return 2 + random.random() * 4
    if c < 30: return max(0, 55 + 30 * math.sin((c - 12) / 2.2) + random.uniform(-8, 8))
    if c < 36: return max(0, 30 - (c - 30) * 5 + random.uniform(-4, 4))
    return 1 + random.random() * 4

def state():
    t = time.time() - T0
    m = min(100, level(t))
    if m >= 25: HYST["motion"], HYST["last"] = True, t
    elif m >= 12: HYST["last"] = t
    elif HYST["motion"] and t - HYST["last"] > 2: HYST["motion"] = False
    return {"m": m, "p": min(100, m * 0.8), "s": "motion" if HYST["motion"] else "still", "cal": False, "cp": 1,
            "rssi": -47 + random.uniform(-1.5, 1.5), "pps": 64 + random.randint(-4, 4), "ppsAll": 81 + random.randint(-5, 5),
            "ping": 58 + random.randint(-3, 3), "score": 1 + m / 100 * 5.8, "sens": SENS[0], "ip": "192.168.4.91", "ssid": "HomeWiFi",
            "ap": "ESP-Radar-3F2A", "apip": "192.168.71.1", "ch": 6, "up": bms() // 1000, "setup": False, "sta": True, "now": bms(), "v": "2.0.0-preview"}

NETS = [("HomeWiFi", "A4:2B:B0:11:22:33", 6, -44, "WPA2/WPA3"), ("HomeWiFi-IoT", "A6:2B:B0:11:22:34", 6, -46, "WPA2"),
        ("NETGEAR47", "9C:3D:CF:AA:01:02", 1, -71, "WPA2"), ("xfinitywifi", "3A:1C:22:9B:00:10", 1, -78, "OPEN"),
        ("Linksys-Guest", "C0:56:27:10:20:30", 11, -83, "WPA2"), ("DIRECT-HP-Printer", "FA:B4:6A:01:02:03", 6, -69, "WPA2"),
        ("ATT-5G-Home", "10:93:97:44:55:66", 1, -80, "WPA2"), ("TP-Link_8C21", "50:C7:BF:8C:21:00", 3, -75, "WPA2"),
        ("", "02:1A:11:F0:00:01", 11, -86, "WPA2")]
DEVS = [("192.168.4.1", "A4:2B:B0:11:22:33", "TP-Link", "", "router"), ("192.168.4.20", "F0:18:98:12:34:56", "Apple", "STEVENS-MBP", ""),
        ("192.168.4.23", "DA:4F:1C:77:10:02", "Private (randomized) MAC", "", ""), ("192.168.4.31", "44:65:0D:AB:CD:EF", "Amazon", "", ""),
        ("192.168.4.37", "B8:27:EB:01:02:03", "Raspberry Pi", "PIHOLE", ""), ("192.168.4.44", "CC:6E:A4:55:66:77", "Samsung", "", ""),
        ("192.168.4.52", "D8:EB:46:10:20:30", "Google", "", ""), ("192.168.4.60", "7C:2E:BD:11:00:22", "Roku", "", ""),
        ("192.168.4.91", "48:CA:43:3F:2A:10", "Espressif", "esp-radar", "this radar"), ("192.168.4.112", "3C:84:6A:90:12:34", "TP-Link", "", ""),
        ("192.168.4.140", "6E:21:9A:00:42:7F", "Private (randomized) MAC", "", "")]

class H(BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def _send(self, code, ctype, body):
        b = body.encode() if isinstance(body, str) else body
        self.send_response(code); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)
    def j(self, o, code=200): self._send(code, "application/json", json.dumps(o))
    def do_GET(self):
        p = self.path.split("?")[0]; now = bms()
        if p in ("/", "/app"): return self._send(200, "text/html", (WEB / "index.html").read_text())
        if p == "/setup": return self._send(200, "text/html", (WEB / "setup.html").read_text())
        if p == "/api/state": return self.j(state())
        if p == "/api/events":
            ev = [{"t": now - 2400000, "start": True, "peak": 0, "dur": 0}, {"t": now - 2310000, "start": False, "peak": 81, "dur": 90000},
                  {"t": now - 600000, "start": True, "peak": 0, "dur": 0}, {"t": now - 552000, "start": False, "peak": 64, "dur": 48000}]
            return self.j({"now": now, "ev": ev})
        if p == "/api/activity":
            cur = state()
            return self.j({"now": now, "minuteStart": (now // 60000) * 60000, "cur": {"avg": int(cur["m"] / 2), "peak": int(cur["m"]), "mot": 30}, "b": ACT})
        if p == "/api/scan":
            nets = [{"ssid": s, "bssid": b, "ch": c, "rssi": r + random.randint(-2, 2), "auth": a, "open": a == "OPEN"} for s, b, c, r, a in NETS]
            return self.j({"scanning": False, "seq": 1, "age": 1, "bssid": "A4:2B:B0:11:22:33", "nets": nets})
        if p == "/api/sample":
            busy = time.time() - SAMPLE["start"] < 1.5
            o = {"id": SAMPLE["id"], "busy": busy, "n": 10}
            if not busy: o.update({"avg": round(-50 - random.random() * 25, 1), "min": -78, "max": -48, "sd": 1.8, "bssid": "A4:2B:B0:11:22:33", "ch": 6, "ssid": "HomeWiFi"})
            return self.j(o)
        if p == "/api/devices":
            el = time.time() - DEVSCAN["start"]; scanning = el < 6; prog = min(100, int(el / 6 * 100))
            shown = DEVS if not scanning else DEVS[:max(1, int(len(DEVS) * prog / 100))]
            d = [{"ip": ip, "mac": m, "vendor": v, "name": n, "first": now - (300000 if i < 9 else 40000), "last": now - random.randint(0, 20000),
                  "new": i >= 9 and role == "", "role": role} for i, (ip, m, v, n, role) in enumerate(shown)]
            return self.j({"now": now, "scanning": scanning, "progress": prog, "done": now - 30000, "subnet": "192.168.4.0/24", "known": 9, "d": d})
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
        if self.path == "/api/sample": SAMPLE["id"] += 1; SAMPLE["start"] = time.time(); return self.j({"id": SAMPLE["id"], "busy": True, "n": 0})
        if self.path == "/api/devices/scan": DEVSCAN["start"] = time.time()
        self.j({"ok": True})

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"preview (SIMULATED data) on http://127.0.0.1:{port}/")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
