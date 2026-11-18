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
BETA = [False]
RULE = {"i": 0, "used": True, "link": 1, "room": "Living room", "timeout": 5, "night": True, "home": True, "bri": 80, "held": False,
        "ids": ["AA:BB:CC:DD:EE:01"]}

def link_rows(m, motion):
    tv = m if motion else 6
    return [
        {"i": 0, "mac": "A4:2B:B0:11:22:33", "name": "eero", "room": "", "m": 8 if motion else m, "sc": 1.15, "pps": 64, "rssi": -47, "s": "still" if motion else ("motion" if m >= 25 else "still"), "src": "csi", "on": True},
        {"i": 1, "mac": "44:65:0D:AB:CD:EF", "name": "Living room TV", "room": "Living room", "m": tv, "sc": 1 + tv / 40, "pps": 22, "rssi": -52, "s": "motion" if motion else "still", "src": "csi", "on": True},
        {"i": 2, "mac": "D8:EB:46:10:20:30", "name": "Kitchen Echo", "room": "Kitchen", "m": 5, "sc": 1.05, "pps": 14, "rssi": -60, "s": "still", "src": "csi", "on": True},
        {"i": 3, "mac": "7C:2E:BD:11:00:22", "name": "Roku", "room": "Den", "m": 9, "sc": 1.4, "pps": 3, "rssi": -66, "s": "still", "src": "rssi", "on": True},
        {"i": 4, "mac": "", "name": "", "room": "", "m": 0, "sc": 0, "pps": 0, "rssi": -100, "s": "off", "src": "none", "on": False},
        {"i": 5, "mac": "", "name": "", "room": "", "m": 0, "sc": 0, "pps": 0, "rssi": -100, "s": "off", "src": "none", "on": False},
    ]
ACT = [[int(max(0, 30 * math.sin(i / 6) + random.uniform(-6, 10))), 0, 0] for i in range(59)]
for a in ACT: a[1] = min(100, a[0] + random.randint(5, 30)); a[2] = 60 if a[0] > 25 else (10 if a[0] > 15 else 0)

def bms(): return int((time.time() - T0) * 1000) + UP0

def level(t):
    c = (t + 18) % 45   # open the preview mid-motion so a screenshot shows a disturbed path
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
            "ap": "ESP-Radar-3F2A", "apip": "192.168.71.1", "ch": 6, "up": bms() // 1000, "setup": False, "sta": True, "now": bms(), "v": "4.1.3-preview","rst":"POWERON","why":"","heap":140000,"minHeap":110000,"ps":2097152,"psFree":1900000,"scanMin":0,"pageMin":120000,"safe":False,
            "home": "home", "who": "Pixel", "deb": 12, "time": True, "tz": -240, "night0": 19, "night1": 7,
            "sniff": True, "sniffWant": True, "beta": BETA[0],
            "loud": "Living room TV" if HYST["motion"] else "eero",
            "loudm": m if HYST["motion"] else 8, "louds": "motion" if HYST["motion"] else "still",
            "links": link_rows(m, HYST["motion"])}

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


AUDIT = {"running": False, "cur": 0, "done": 0, "total": 29, "qlen": 0, "scanned": 3, "def": True, "phase": "", "open_ip": None}
REPORTS = {
  "192.168.4.1": {"ip":"192.168.4.1","mac":"A4:2B:B0:11:22:33","vendor":"TP-Link","sev":0,"sevName":"OK","when":1000,"role":"router","open":2,"def":True,"defTried":0,"defResult":"skipped",
    "ports":[{"port":80,"svc":"HTTP","state":"open"},{"port":443,"svc":"HTTPS","state":"open"},{"port":22,"svc":"SSH","state":"closed"},{"port":23,"svc":"Telnet","state":"closed"}],
    "findings":[{"sev":1,"title":"Web service","detail":"HTTPS on 443: status 200, Server: nginx (certificate not verified)","tip":"Certificate isn't validated by this light audit; a browser warning usually means a self-signed cert."},
                {"sev":0,"title":"Web service","detail":"HTTP on 80: status 301, title: Router","tip":"Informational."}]},
  "192.168.4.60": {"ip":"192.168.4.60","mac":"7C:2E:BD:11:00:22","vendor":"Roku","sev":2,"sevName":"Medium","when":2000,"role":"","open":3,"def":True,"defTried":0,"defResult":"skipped",
    "ports":[{"port":80,"svc":"HTTP","state":"open"},{"port":8080,"svc":"HTTP-alt","state":"open"},{"port":8060,"svc":"HTTP-alt","state":"closed"}],
    "findings":[{"sev":1,"title":"Web service","detail":"HTTP on 80: status 200, Server: Roku UPnP, title: Roku","tip":"Plain HTTP admin interface - prefer HTTPS and a strong password."},
                {"sev":2,"title":"Web login over plain HTTP","detail":"HTTP on 8080: status 401, WWW-Authenticate: Basic","tip":"Use HTTPS for the admin page if the device supports it, and set a strong password."}]},
  "192.168.4.112": {"ip":"192.168.4.112","mac":"3C:84:6A:90:12:34","vendor":"TP-Link","sev":4,"sevName":"Critical","when":3000,"role":"","open":4,"def":True,"defTried":3,"defResult":"found",
    "ports":[{"port":23,"svc":"Telnet","state":"open"},{"port":80,"svc":"HTTP","state":"open"},{"port":554,"svc":"RTSP","state":"open"},{"port":8080,"svc":"HTTP-alt","state":"open"}],
    "findings":[{"sev":3,"title":"Telnet is open","detail":"Port 23 (Telnet) accepts connections. Telnet sends everything, including passwords, in clear text and is a classic IoT attack vector. Banner: BusyBox v1.20.2","tip":"Disable Telnet in the device settings; use SSH or the web UI instead."},
                {"sev":2,"title":"RTSP camera stream port open","detail":"Port 554 (RTSP) is open - this is often an IP camera video stream that may be viewable without a password.","tip":"Set a strong camera password and disable RTSP if you don't use it."},
                {"sev":2,"title":"Web login over plain HTTP","detail":"HTTP on 80: status 401, Server: GoAhead, title: IP Camera","tip":"Use HTTPS for the admin page if the device supports it, and set a strong password."},
                {"sev":2,"title":"Dated software","detail":"Server header: GoAhead - GoAhead embedded webserver - common on cheap IP cameras with known issues","tip":"Check the vendor for a firmware update; replace the device if it is end-of-life."},
                {"sev":4,"title":"Default login accepted","detail":"A factory-default username/password worked on the web login (port 80). User \"admin\" with a well-known default password (pair 1).","tip":"Change this device's admin password immediately to a unique strong one."}]},
}

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
            aud_map = {"192.168.4.1":1,"192.168.4.60":3,"192.168.4.112":5,"192.168.4.91":1}
            d = [{"ip": ip, "mac": m, "vendor": v, "name": n, "first": now - (300000 if i < 9 else 40000), "last": now - random.randint(0, 20000),
                  "new": i >= 9 and role == "", "role": role, "audit": aud_map.get(ip, 0),
                  "vendor": "" if i == 10 else v,
                  "alias": "Pixel" if i == 2 else ("Living room TV" if i == 5 else ""),
                  "person": i == 2, "online": i != 10, "up": 96 if i != 4 else 71, "flap": 6 if i == 4 else 0,
                  "night": 3 if i in (3, 6) else 0, "drop": int(time.time()) - 5400 if i == 4 else 0,
                  "hours": 0x0fffff, "radio": i in (3, 5, 6, 7)} for i, (ip, m, v, n, role) in enumerate(shown)]
            return self.j({"now": now, "scanning": scanning, "progress": prog, "done": now - 30000, "subnet": "192.168.4.0/24", "known": 9, "d": d})
        
        if p == "/api/audit/status":
            ip = 0
            if "ip=" in self.path:
                try: ip = int(self.path.split("ip=")[1].split("&")[0])
                except: pass
            tip = f"{DEVS[0][0].rsplit('.',1)[0]}.{ip}" if ip else None
            rep = REPORTS.get(tip) if tip else None
            if AUDIT["open_ip"]:
                tip2 = AUDIT["open_ip"]; rep = REPORTS.get(tip2, rep)
            return self.j({"running": AUDIT["running"], "cur": AUDIT["cur"], "done": AUDIT["done"], "total": AUDIT["total"],
                           "qlen": AUDIT["qlen"], "scanned": AUDIT["scanned"], "def": AUDIT["def"], "phase": AUDIT["phase"], "report": rep})
        if p == "/api/audit/summary":
            return self.j({"total": 3, "ok": 1, "low": 0, "medium": 1, "high": 0, "critical": 1, "running": AUDIT["running"], "qlen": AUDIT["qlen"]})
        if p == "/api/links":
            rows = link_rows(40, True)
            return self.j({"sniff": True, "links": rows, "talkers": [
                {"mac": "44:65:0D:AB:CD:EF", "hits": 240, "rssi": -52, "vendor": "Amazon"},
                {"mac": "D8:EB:46:10:20:30", "hits": 180, "rssi": -60, "vendor": "Amazon"},
                {"mac": "CC:6E:A4:55:66:77", "hits": 90, "rssi": -58, "vendor": "Samsung"},
                {"mac": "7C:2E:BD:11:00:22", "hits": 40, "rssi": -66, "vendor": "Roku"}]})
        if p == "/api/presence":
            return self.j({"home": "home", "who": "Pixel", "debounceMin": 12, "tz": -240, "night0": 19, "night1": 7, "time": True, "log": [
                {"t": now - 3600000, "epoch": int(time.time()) - 3600, "arrive": True, "mac": "DA:4F:1C:77:10:02", "name": "Pixel"},
                {"t": now - 8 * 3600000, "epoch": int(time.time()) - 8 * 3600, "arrive": False, "mac": "DA:4F:1C:77:10:02", "name": "Pixel"}]})
        if p == "/api/lights":
            return self.j({"beta": BETA[0], "night": False, "home": "home", "lights": [
                {"id": "AA:BB:CC:DD:EE:01", "brand": "govee", "ip": "192.168.4.50", "name": "Govee H6008", "model": "H6008", "on": 0, "bri": 80},
                {"id": "a8bb50112233", "brand": "wiz", "ip": "192.168.4.51", "name": "WiZ lamp", "model": "WiZ", "on": 1, "bri": 60},
                {"id": "lifx-192.168.4.52", "brand": "lifx", "ip": "192.168.4.52", "name": "LIFX", "model": "LAN", "on": 0, "bri": 0},
            ] if BETA[0] else [], "rules": [RULE] if BETA[0] else [{"i": 0, "used": False, "link": -1, "room": "", "timeout": 5, "night": False, "home": False, "bri": 80, "held": False, "ids": []}]})

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
        if self.path == "/api/lights/beta":
            BETA[0] = "v=1" in body
        if self.path == "/api/lights/rule" and "used=0" in body:
            RULE["used"] = False
        if self.path == "/api/sample": SAMPLE["id"] += 1; SAMPLE["start"] = time.time(); return self.j({"id": SAMPLE["id"], "busy": True, "n": 0})
        
        if self.path == "/api/audit":
            AUDIT["def"] = "def=1" in body; ip = body.split("ip=")[1].split("&")[0] if "ip=" in body else ""
            AUDIT["running"]=True; AUDIT["cur"]=int(ip.split(".")[-1]) if ip else 0; AUDIT["open_ip"]=ip; AUDIT["phase"]="scanning :80 (HTTP)"; AUDIT["done"]=8; AUDIT["qlen"]=0
            return self.j({"queued":1,"ip":AUDIT["cur"]})
        if self.path == "/api/audit/all":
            AUDIT["def"]="def=1" in body; AUDIT["running"]=True; AUDIT["qlen"]=5; AUDIT["phase"]="queued"; return self.j({"queued":5})
        if self.path == "/api/audit/cancel":
            AUDIT["running"]=False; AUDIT["qlen"]=0; AUDIT["phase"]=""; return self.j({"ok":True})

        if self.path == "/api/devices/scan": DEVSCAN["start"] = time.time()
        self.j({"ok": True})

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8080
    print(f"preview (SIMULATED data) on http://127.0.0.1:{port}/")
    ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
