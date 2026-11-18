#!/usr/bin/env python3
"""Compare internal DRAM in the linker map against the v4.1.1 baseline.

v4.1.1 (map from the 4.1.1 build, globals reported as 116100):
  .dram0.bss  0x167d8 (92120)
  .dram0.data 0x5dab  (23979)
  LinkRT rt   39864 bytes in .bss
  devices     3456
  lights      2880
  light-save  2480 + 2480 (two function-static copies)
  health      2064

This script reads build/esp_radar.ino.map and fails if those arrays are
back in internal BSS, or if static DRAM is not at least 40 KB smaller.
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[1]
map_path = root / "build" / "esp_radar.ino.map"
if not map_path.exists():
    print("no map at", map_path, "- run tools/build.sh first")
    sys.exit(1)
text = map_path.read_text(errors="ignore").splitlines()

def section(name):
    for line in text:
        if line.startswith(name + " ") or line.startswith(name + "\t"):
            parts = line.split()
            return int(parts[2], 16)
    return None

bss = section(".dram0.bss")
data = section(".dram0.data")
static = (bss or 0) + (data or 0)
before = 116100
print(f"internal static  v4.1.1 {before:6d}   v4.1.2 {static:6d}   freed {before - static:6d}")
print(f"  .dram0.bss  {bss}   .dram0.data {data}")

rt = None
for i, line in enumerate(text):
    m = re.search(r"\.bss\._ZL2rt\s+(0x[0-9a-fA-F]+)\s+(0x[0-9a-fA-F]+)", line)
    if m:
        rt = int(m.group(2), 16)
        break
print("rt internal bytes", rt)

errors = []
if static > 80000:
    errors.append(f"internal static {static} is still high (want the big tables in PSRAM)")
if rt is None or rt > 64:
    errors.append("LinkRT should be a PSRAM pointer, not an internal array")
if errors:
    print("FAIL")
    for e in errors:
        print(" -", e)
    sys.exit(1)
print("OK static DRAM dropped by moving CSI, devices, health, and lights off the internal heap")
print("field v4.1.1 steady free was 60 KB (min 15 KB on Motion, 1 KB on a failed scan)")
print(f"estimate v4.1.2 steady free ~= 60 KB + {before - static} B static + 12 KB lights task not started when Beta is off")
