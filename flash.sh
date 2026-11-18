#!/usr/bin/env bash
# ESP Radar flasher for Linux / macOS - Heltec WiFi LoRa 32 V4 (ESP32-S3)
#   curl -fsSL https://raw.githubusercontent.com/trequanjones612-afk/esp-radar/main/flash.sh | bash
# Options (environment variables):
#   ESP_RADAR_PORT=/dev/ttyACM0   use this serial port instead of auto-detecting
#   ESP_RADAR_ERASE=1             erase the whole flash first (recommended when coming from Meshtastic)
#   ESP_RADAR_DRY_RUN=1           do everything except writing to the board
#   ESP_RADAR_REF=main            git branch/tag to download the firmware from
# No sudo is used. Everything lives in ~/.local/share/esp-radar.
set -euo pipefail

main() {
  local REPO="trequanjones612-afk/esp-radar"
  local REF="${ESP_RADAR_REF:-main}"
  local RAW="${ESP_RADAR_BASE_URL:-https://raw.githubusercontent.com/$REPO/$REF}"
  local HOME_DIR="${XDG_DATA_HOME:-$HOME/.local/share}/esp-radar"
  local VENV="$HOME_DIR/venv"
  local BIN="$HOME_DIR/esp-radar-heltec-v4.bin"
  local DRY="${ESP_RADAR_DRY_RUN:-0}"
  local G=$'\033[1;32m' Y=$'\033[1;33m' R=$'\033[1;31m' D=$'\033[2m' N=$'\033[0m'
  [ -t 1 ] || { G=""; Y=""; R=""; D=""; N=""; }

  say()  { printf '%s==>%s %s\n' "$G" "$N" "$*"; }
  warn() { printf '%s!!%s  %s\n' "$Y" "$N" "$*" >&2; }
  die()  { printf '%sxx%s  %s\n' "$R" "$N" "$*" >&2; exit 1; }

  printf '%s' "$G"
  cat <<'ART'
    ______ _____ ____     ____            __
   / ____// ___// __ \   / __ \____ _____/ /___ ______
  / __/   \__ \/ /_/ /  / /_/ / __ `/ __  / __ `/ ___/
 / /___  ___/ / ____/  / _, _/ /_/ / /_/ / /_/ / /
/_____/ /____/_/      /_/ |_|\__,_/\__,_/\__,_/_/
        WiFi CSI motion radar  ·  Heltec WiFi LoRa 32 V4
ART
  printf '%s\n' "$N"
  [ "$DRY" = "1" ] && warn "DRY RUN: nothing will be written to the board."

  command -v curl >/dev/null 2>&1 || die "curl is required."
  local PY=""
  for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1 && "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>/dev/null; then PY="$c"; break; fi
  done
  mkdir -p "$HOME_DIR"

  # ---- 1. esptool ----
  local ESPTOOL=""
  if [ -n "$PY" ]; then
    say "Python: $($PY --version 2>&1). Setting up esptool in $VENV"
    if [ -x "$VENV/bin/python" ] || "$PY" -m venv "$VENV" >/dev/null 2>&1; then
      if "$VENV/bin/python" -m pip install --quiet --disable-pip-version-check --upgrade "esptool>=4.7,<6" >/dev/null 2>&1; then
        ESPTOOL="$VENV/bin/python -m esptool"
      else
        warn "pip could not install esptool into the venv."
      fi
    else
      warn "python3 -m venv failed (on Debian/Ubuntu: python3-venv is missing)."
      rm -rf "$VENV"
    fi
  else
    warn "Python 3.8+ not found."
  fi
  if [ -z "$ESPTOOL" ]; then
    say "Falling back to the standalone esptool release from Espressif"
    local os arch asset ver="v5.6.0"
    os="$(uname -s)"; arch="$(uname -m)"
    case "$os/$arch" in
      Linux/x86_64) asset="linux-amd64" ;; Linux/aarch64|Linux/arm64) asset="linux-aarch64" ;; Linux/armv7l) asset="linux-armv7" ;;
      Darwin/arm64) asset="macos-arm64" ;; Darwin/x86_64) asset="macos-amd64" ;;
      *) die "No standalone esptool for $os/$arch. Install Python 3.8+ (with venv) and re-run." ;;
    esac
    mkdir -p "$HOME_DIR/esptool"
    curl -fsSL "https://github.com/espressif/esptool/releases/download/$ver/esptool-$ver-$asset.tar.gz" | tar xz -C "$HOME_DIR/esptool" --strip-components=1 \
      || die "Could not download esptool."
    ESPTOOL="$HOME_DIR/esptool/esptool"
  fi
  say "esptool ready: $($ESPTOOL version 2>/dev/null | tail -1)"

  # ---- 2. firmware ----
  say "Downloading firmware from $RAW/firmware/"
  ESP_RADAR_TMP="$(mktemp -d)"
  trap 'rm -rf "${ESP_RADAR_TMP:-}"' EXIT
  local tmp="$ESP_RADAR_TMP"
  curl -fsSL "$RAW/firmware/esp-radar-heltec-v4.bin.b64" -o "$tmp/fw.b64" || die "Firmware download failed."
  curl -fsSL "$RAW/firmware/esp-radar-heltec-v4.bin.sha256" -o "$tmp/fw.sha256" || die "Checksum download failed."
  if [ -n "$PY" ]; then
    "$PY" -c 'import base64,sys; open(sys.argv[2],"wb").write(base64.b64decode(open(sys.argv[1],"rb").read()))' "$tmp/fw.b64" "$tmp/fw.bin"
  else
    base64 -d < "$tmp/fw.b64" > "$tmp/fw.bin" 2>/dev/null || base64 -D < "$tmp/fw.b64" > "$tmp/fw.bin"
  fi
  local want got
  want="$(tr -d ' \r\n' < "$tmp/fw.sha256")"
  if command -v sha256sum >/dev/null 2>&1; then got="$(sha256sum "$tmp/fw.bin" | awk '{print $1}')"; else got="$(shasum -a 256 "$tmp/fw.bin" | awk '{print $1}')"; fi
  [ "$want" = "$got" ] || die "Checksum mismatch (expected $want, got $got)."
  mv "$tmp/fw.bin" "$BIN"
  say "Firmware OK: $(wc -c < "$BIN" | tr -d ' ') bytes, sha256 ${got:0:16}…"

  # ---- 3. serial port ----
  local PORT="${ESP_RADAR_PORT:-}"
  find_port() {
    local p
    for p in /dev/ttyACM* /dev/ttyUSB* /dev/cu.usbmodem* /dev/cu.usbserial* /dev/cu.wchusbserial* /dev/cu.SLAB_USBtoUART*; do
      [ -e "$p" ] && { echo "$p"; return 0; }
    done
    return 1
  }
  if [ -z "$PORT" ]; then
    say "Looking for the board (USB-C cable with data lines)…"
    local waited=0 limit=60
    [ "$DRY" = "1" ] && limit=0
    until PORT="$(find_port)"; do
      if [ "$waited" -ge "$limit" ]; then
        if [ "$DRY" = "1" ]; then
          warn "No serial port found (expected in a dry run without a board)."
          say "Dry run finished: esptool + firmware are ready in $HOME_DIR"
          return 0
        fi
        die "No board found. Plug it in, or hold PRG, tap RST, release PRG (download mode), then re-run. Or set ESP_RADAR_PORT=/dev/ttyXXX."
      fi
      [ "$waited" -eq 0 ] && warn "No serial port yet - plug in the Heltec V4 now (waiting up to ${limit}s)…"
      sleep 2; waited=$((waited + 2))
    done
  fi
  say "Using port $PORT"
  if [ -e "$PORT" ] && [ ! -w "$PORT" ]; then
    warn "No permission to open $PORT."
    if [ "$(uname -s)" = "Linux" ]; then
      warn "Add yourself to the serial group, then log out and back in:  sudo usermod -aG dialout \$USER"
      warn "(Arch/Fedora may use the 'uucp' group instead of 'dialout'.)"
    fi
    [ "$DRY" = "1" ] || exit 1
  fi

  # ---- 4. flash ----
  local cmd="$ESPTOOL --chip esp32s3 --port $PORT --baud 921600 write_flash 0x0 $BIN"
  if [ "$DRY" = "1" ]; then
    say "DRY RUN - would run:"
    [ "${ESP_RADAR_ERASE:-0}" = "1" ] && echo "   $ESPTOOL --chip esp32s3 --port $PORT erase_flash"
    echo "   $cmd"
    return 0
  fi
  if [ "${ESP_RADAR_ERASE:-0}" = "1" ]; then
    say "Erasing flash…"
    $ESPTOOL --chip esp32s3 --port "$PORT" erase_flash
  fi
  say "Flashing (about 20 s)…"
  if ! $ESPTOOL --chip esp32s3 --port "$PORT" --baud 921600 write_flash 0x0 "$BIN"; then
    warn "Flash failed. Retrying at 115200 baud…"
    $ESPTOOL --chip esp32s3 --port "$PORT" --baud 115200 write_flash 0x0 "$BIN" || die \
      "Still failing. Put the board in download mode: hold PRG, tap RST, release PRG - then re-run this command."
  fi

  printf '\n%s' "$G"
  cat <<'DONE'
  ✔ ESP Radar is flashed! Next steps:
    1. The OLED shows "SETUP MODE". On your phone, join WiFi  ESP-Radar-XXXX
       (password: espradar).
    2. Open  http://192.168.4.1  and pick your home 2.4 GHz WiFi + password.
    3. The board restarts, joins your WiFi and calibrates for ~10 s - keep the room still.
    4. Open the radar at  http://esp-radar.local  (or the IP on the OLED),
       or keep using http://192.168.4.1 on the ESP-Radar network.
    Short-press PRG = recalibrate · hold PRG 3 s = forget WiFi.
DONE
  printf '%s\n' "$N"
  printf '%sSerial CSV log: %s -m serial.tools.miniterm %s 115200%s\n' "$D" "$VENV/bin/python" "$PORT" "$N"
}

main "$@"
