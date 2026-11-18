#!/usr/bin/env bash
# Rebuild firmware/esp-radar-heltec-v4.bin from source (arduino-cli + arduino-esp32 core 3.3.x).
# Needs: arduino-cli, `arduino-cli core install esp32:esp32@3.3.13`, and the libraries:
#   arduino-cli lib install "ESP Async WebServer@3.12.1" "Async TCP@3.6.1" "ESP8266 and ESP32 OLED driver for SSD1306 displays@4.6.2"
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$ROOT/build}"
FQBN="esp32:esp32:heltec_wifi_lora_32_V4:PartitionScheme=no_ota,CDCOnBoot=cdc,PSRAM=enabled"
python3 "$ROOT/tools/embed_web.py"
# Cap the async queues. v4.1.1 left SSE at 32 messages per client, and each
# state event is an internal allocation, which is what walked the heap down
# to 1 KB while the Motion page was open.
EXTRA="-DSSE_MAX_QUEUED_MESSAGES=2 -DWS_MAX_QUEUED_MESSAGES=4 -DDEFAULT_MAX_WS_CLIENTS=2 -DCONFIG_ASYNC_TCP_QUEUE_SIZE=16 -DCONFIG_ASYNC_TCP_STACK_SIZE=8192"
arduino-cli compile -b "$FQBN" --warnings default --build-property "compiler.cpp.extra_flags=${EXTRA}" --output-dir "$OUT" "$ROOT/src/esp_radar"
CORE="$(arduino-cli config get directories.data)/packages/esp32"
# shellcheck disable=SC2012
BOOTAPP0="$(ls "$CORE"/hardware/esp32/*/tools/partitions/boot_app0.bin | tail -1)"
# shellcheck disable=SC2012
ESPTOOL="$(ls "$CORE"/tools/esptool_py/*/esptool | tail -1)"
"$ESPTOOL" --chip esp32s3 merge-bin -o "$ROOT/firmware/esp-radar-heltec-v4.bin" \
  --flash-mode keep --flash-freq keep --flash-size 16MB \
  0x0 "$OUT/esp_radar.ino.bootloader.bin" 0x8000 "$OUT/esp_radar.ino.partitions.bin" \
  0xe000 "$BOOTAPP0" 0x10000 "$OUT/esp_radar.ino.bin"
cd "$ROOT/firmware"
base64 -w 76 esp-radar-heltec-v4.bin > esp-radar-heltec-v4.bin.b64
sha256sum esp-radar-heltec-v4.bin | awk '{print $1}' > esp-radar-heltec-v4.bin.sha256
ls -l esp-radar-heltec-v4.bin*; cat esp-radar-heltec-v4.bin.sha256
