#!/usr/bin/env python3
"""Host-side check that the Arduino loop is not the light/BLE worker.

v4.1.0 called scanPump, tuyaPortProbe, and goveeBleScan from loop().
WiFiClient::connect waits in select on the caller, and the CSI/SSE/OLED
work shares that task, so a sweep stalled the page and WiFi RX.

This does not flash a board. It fails the build if loop() grows a call
into that path again.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
ino = (root / "src/esp_radar/esp_radar.ino").read_text()
lights = (root / "src/esp_radar/v4_lights.h").read_text()
tuya = (root / "src/esp_radar/v4_tuya.h").read_text()
ble = (root / "src/esp_radar/v4_govee_ble.h").read_text()

def fn_body(src, name):
    m = re.search(r"(?:[\w:\*]+\s+)+%s\s*\([^)]*\)\s*\{" % name, src)
    if not m:
        return None
    i = m.end() - 1
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[m.start():j + 1]
    return None

errors = []
loop = fn_body(ino, "loop")
if not loop:
    errors.append("loop() not found")
else:
    banned = [
        "v4LightsTick", "v4LightsWorker", "scanPump", "tuyaPump", "goveeBlePump",
        "tuyaPortProbe", "tuyaControl", "goveeBleScan", "goveeBleInit", "goveeBleWrite",
        "WiFiClient", "BLEDevice", "ltEnsure", "probeHost",
    ]
    for name in banned:
        if re.search(r"\b%s\s*\(" % name, loop):
            errors.append("loop() calls %s" % name)

if "v4LightsStart()" not in ino:
    errors.append("setup does not start the lights task")
worker = fn_body(lights, "v4LightsWorker")
if not worker:
    errors.append("v4LightsWorker missing")
elif "scanPump" not in worker or "goveeBlePump" not in worker:
    errors.append("worker does not own the scan and BLE pumps")

if "BLEDevice::init" not in ble:
    errors.append("BLE init missing")
for fn in ("goveeBleScan", "goveeBleWrite"):
    body = fn_body(ble, fn)
    if not body or "goveeBleShutdown" not in body:
        errors.append("%s does not shut BLE down" % fn)
if "BLEDevice::deinit" not in ble:
    errors.append("BLEDevice::deinit missing")
# init must not be reachable from boot helpers
for fn in ("setup", "ltEnsure", "ltLoad"):
    body = fn_body(ino if fn == "setup" else lights, fn)
    if body and "goveeBleInit" in body:
        errors.append("%s initializes BLE" % fn)

if "safeMode" not in lights or "crashReset" not in lights:
    errors.append("safe mode not applied when Beta was on")
if "minHeap" not in ino or "rstName" not in ino:
    errors.append("state JSON missing reset reason or min heap")

# Old loop cost was one host per iteration: 160 ms connect + up to ~350 ms of delay(8).
# The new loop must not contain those waits. The worker uses short timeouts.
if re.search(r"connect\([^)]*160", tuya) or re.search(r"connect\([^)]*800", tuya):
    errors.append("Tuya still uses the v4.1.0 connect timeouts")
if "tcpReadSome(c, rx, rxCap, 350)" in tuya:
    errors.append("Tuya control read is still 350 ms")

if errors:
    print("FAIL")
    for e in errors:
        print(" -", e)
    sys.exit(1)
print("OK loop does not call the light/BLE/TCP path")
print("OK BLE init is paired with deinit and is not in setup or ltEnsure")
print("OK connect timeouts are the short ones; worker owns scanPump")
print("note: v4.1.0 loop could spend 160-510 ms inside lwIP per host; loop() no longer does")
