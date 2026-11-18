# ESP Radar flasher for Windows (PowerShell 5.1+ / 7+) - Heltec WiFi LoRa 32 V4 (ESP32-S3)
#   irm https://raw.githubusercontent.com/trequanjones612-afk/esp-radar/main/flash.ps1 | iex
# Options (set before running):
#   $env:ESP_RADAR_PORT = "COM5"   use this port instead of auto-detecting
#   $env:ESP_RADAR_ERASE = "1"     erase the whole flash first (wipes saved WiFi; recommended when coming from Meshtastic)
# By default the NVS partition (saved WiFi + settings) is NOT written, so updates keep your settings.
#   $env:ESP_RADAR_DRY_RUN = "1"   do everything except writing to the board
# No admin rights needed. Everything lives in %LOCALAPPDATA%\esp-radar.
# Written as one function so a failure never closes your PowerShell window.

function Install-EspRadar {
  # 'Continue' on purpose: on Windows PowerShell 5.1, 'Stop' turns any native-command stderr
  # (pip/esptool warnings) into a terminating error. Cmdlets that must succeed use -ErrorAction Stop.
  $ErrorActionPreference = 'Continue'
  $ProgressPreference = 'SilentlyContinue'   # makes Invoke-WebRequest much faster on PS 5.1
  try { [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12 } catch {}

  $Repo = 'trequanjones612-afk/esp-radar'
  $Ref = if ($env:ESP_RADAR_REF) { $env:ESP_RADAR_REF } else { 'main' }
  $Raw = if ($env:ESP_RADAR_BASE_URL) { $env:ESP_RADAR_BASE_URL } else { "https://raw.githubusercontent.com/$Repo/$Ref" }
  $Dry = ($env:ESP_RADAR_DRY_RUN -eq '1')
  $LocalBase = if ($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $HOME '.local/share' }
  $Dir = Join-Path $LocalBase 'esp-radar'
  $Venv = Join-Path $Dir 'venv'
  $Bin = Join-Path $Dir 'esp-radar-heltec-v4.bin'
  New-Item -ItemType Directory -Force -Path $Dir -ErrorAction Stop | Out-Null

  function Say($m)  { Write-Host "==> $m" -ForegroundColor Green }
  function Warn($m) { Write-Host "!!  $m" -ForegroundColor Yellow }

  Write-Host @'
    ______ _____ ____     ____            __
   / ____// ___// __ \   / __ \____ _____/ /___ ______
  / __/   \__ \/ /_/ /  / /_/ / __ `/ __  / __ `/ ___/
 / /___  ___/ / ____/  / _, _/ /_/ / /_/ / /_/ / /
/_____/ /____/_/      /_/ |_|\__,_/\__,_/\__,_/_/
        WiFi CSI motion radar  -  Heltec WiFi LoRa 32 V4
'@ -ForegroundColor Green
  if ($Dry) { Warn 'DRY RUN: nothing will be written to the board.' }

  # ---- 1. Python 3.8+ (py launcher first; skip the Microsoft Store alias stub) ----
  $Py = $null
  foreach ($cand in @(@('py', '-3'), @('python'), @('python3'))) {
    $exe = $cand[0]; $pre = @($cand | Select-Object -Skip 1)
    if (-not (Get-Command $exe -ErrorAction SilentlyContinue)) { continue }
    try {
      $null = & $exe @pre -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' 2>$null
      if ($LASTEXITCODE -eq 0) { $Py = $cand; break }
    } catch {}
  }

  # ---- 2. esptool (venv, or the standalone Espressif build as fallback) ----
  $Esptool = $null
  $VenvPy = if ($IsLinux -or $IsMacOS) { Join-Path $Venv 'bin/python' } else { Join-Path $Venv 'Scripts\python.exe' }
  if ($Py) {
    $pyExe = $Py[0]; $pyPre = @($Py | Select-Object -Skip 1)
    Say ("Python: " + (& $pyExe @pyPre --version 2>&1) + ". Setting up esptool in $Venv")
    if (-not (Test-Path $VenvPy)) { & $pyExe @pyPre -m venv $Venv 2>$null | Out-Null }
    if (Test-Path $VenvPy) {
      & $VenvPy -m pip install --quiet --disable-pip-version-check --upgrade 'esptool>=4.7,<6' 2>$null | Out-Null
      if ($LASTEXITCODE -eq 0) { $Esptool = @($VenvPy, '-m', 'esptool') } else { Warn 'pip could not install esptool.' }
    } else { Warn 'Could not create a Python venv.' }
  } else { Warn 'Python 3.8+ not found (get it from python.org or: winget install Python.Python.3.12).' }
  if (-not $Esptool) {
    if ($IsLinux -or $IsMacOS) { throw 'No usable Python/esptool. On Linux/macOS use flash.sh instead.' }
    Say 'Falling back to the standalone esptool release from Espressif'
    $ver = 'v5.6.0'
    $zip = Join-Path $Dir 'esptool.zip'
    Invoke-WebRequest -ErrorAction Stop -UseBasicParsing -Uri "https://github.com/espressif/esptool/releases/download/$ver/esptool-$ver-windows-amd64.zip" -OutFile $zip
    $etDir = Join-Path $Dir 'esptool'
    if (Test-Path $etDir) { Remove-Item -Recurse -Force $etDir }
    Expand-Archive -ErrorAction Stop -Path $zip -DestinationPath $etDir -Force
    $exe = Get-ChildItem -Path $etDir -Recurse -Filter 'esptool.exe' | Select-Object -First 1
    if (-not $exe) { throw 'esptool.exe not found in the downloaded archive.' }
    $Esptool = @($exe.FullName)
  }
  $etExe = $Esptool[0]; $etPre = @($Esptool | Select-Object -Skip 1)
  Say ('esptool ready: ' + ((& $etExe @etPre version 2>&1) | Select-Object -Last 1))

  # ---- 3. firmware (base64 in the repo -> .bin, checksum verified) ----
  Say "Downloading firmware from $Raw/firmware/"
  $b64 = (Invoke-WebRequest -ErrorAction Stop -UseBasicParsing -Uri "$Raw/firmware/esp-radar-heltec-v4.bin.b64").Content
  if ($b64 -is [byte[]]) { $b64 = [Text.Encoding]::ASCII.GetString($b64) }
  $want = (Invoke-WebRequest -ErrorAction Stop -UseBasicParsing -Uri "$Raw/firmware/esp-radar-heltec-v4.bin.sha256").Content
  if ($want -is [byte[]]) { $want = [Text.Encoding]::ASCII.GetString($want) }
  $want = $want.Trim().ToLower()
  $bytes = [Convert]::FromBase64String(($b64 -replace '\s', ''))
  [IO.File]::WriteAllBytes($Bin, $bytes)
  $got = (Get-FileHash -ErrorAction Stop -Algorithm SHA256 -Path $Bin).Hash.ToLower()
  if ($got -ne $want) { throw "Checksum mismatch (expected $want, got $got)." }
  Say ("Firmware OK: {0} bytes, sha256 {1}..." -f $bytes.Length, $got.Substring(0, 16))
  # Split the merged image so the NVS partition (0x9000-0xDFFF: saved WiFi + settings) is never written:
  #   part 1 = 0x0000-0x8FFF (bootloader + partition table), part 2 = 0xE000-end (otadata + app)
  if ($bytes.Length -le 0xE000) { throw 'Firmware image is too small.' }
  $P1 = Join-Path $Dir 'part-0x0000.bin'; $P2 = Join-Path $Dir 'part-0xe000.bin'
  $a = New-Object byte[] 0x9000; [Array]::Copy($bytes, 0, $a, 0, 0x9000); [IO.File]::WriteAllBytes($P1, $a)
  $b = New-Object byte[] ($bytes.Length - 0xE000); [Array]::Copy($bytes, 0xE000, $b, 0, $b.Length); [IO.File]::WriteAllBytes($P2, $b)

  # ---- 4. serial port ----
  $Port = $env:ESP_RADAR_PORT
  if (-not $Port) {
    Say 'Looking for the board (USB-C cable with data lines)...'
    $deadline = (Get-Date).AddSeconds($(if ($Dry) { 0 } else { 60 }))
    $warned = $false
    while (-not $Port) {
      $cands = @()
      try {
        # Espressif native USB (VID 303A), CP210x (10C4), CH34x (1A86) first
        $cands = @(Get-CimInstance -ClassName Win32_PnPEntity -ErrorAction Stop |
          Where-Object { $_.Name -match '\((COM\d+)\)' -and $_.PNPDeviceID -match 'VID_(303A|10C4|1A86)' } |
          ForEach-Object { if ($_.Name -match '\((COM\d+)\)') { $Matches[1] } })
      } catch {}
      if ($cands.Count -eq 0) { try { $cands = @([IO.Ports.SerialPort]::GetPortNames() | Sort-Object -Unique) } catch {} }
      if ($cands.Count -eq 1) { $Port = $cands[0]; break }
      if ($cands.Count -gt 1) {
        Write-Host ('Serial ports found: ' + ($cands -join ', '))
        if ($Dry) { $Port = $cands[0]; break }
        $Port = Read-Host 'Which port is the Heltec board? (e.g. COM5)'
        break
      }
      if ((Get-Date) -ge $deadline) {
        if ($Dry) { Warn 'No serial port found (expected in a dry run without a board).'; Say "Dry run finished: esptool + firmware are ready in $Dir"; return }
        $Port = Read-Host 'No board detected. Type the COM port (e.g. COM5), or press Enter to cancel'
        if (-not $Port) { Warn 'Cancelled. Tip: hold PRG, tap RST, release PRG (download mode), then run again.'; return }
        break
      }
      if (-not $warned) { Warn 'No serial port yet - plug in the Heltec V4 now (waiting up to 60 s)...'; $warned = $true }
      Start-Sleep -Seconds 2
    }
  }
  Say "Using port $Port"

  # ---- 5. flash ----
  if ($Dry) {
    Say 'DRY RUN - would run:'
    if ($env:ESP_RADAR_ERASE -eq '1') { Write-Host "   $($Esptool -join ' ') --chip esp32s3 --port $Port erase_flash" }
    Write-Host "   $($Esptool -join ' ') --chip esp32s3 --port $Port --baud 921600 write_flash 0x0 `"$P1`" 0xe000 `"$P2`""
    return
  }
  if ($env:ESP_RADAR_ERASE -eq '1') {
    Say 'Erasing flash...'
    & $etExe @etPre --chip esp32s3 --port $Port erase_flash
    if ($LASTEXITCODE -ne 0) { throw 'Erase failed. Hold PRG, tap RST, release PRG, then run again.' }
  }
  if ($env:ESP_RADAR_ERASE -eq '1') { Say 'Flash erased: saved WiFi settings are gone, you will get setup mode.' }
  else { Say 'Keeping your saved WiFi + settings (NVS at 0x9000 is not touched).' }
  Say 'Flashing (about 20 s)...'
  & $etExe @etPre --chip esp32s3 --port $Port --baud 921600 write_flash 0x0 $P1 0xe000 $P2
  if ($LASTEXITCODE -ne 0) {
    Warn 'Flash failed. Retrying at 115200 baud...'
    & $etExe @etPre --chip esp32s3 --port $Port --baud 115200 write_flash 0x0 $P1 0xe000 $P2
    if ($LASTEXITCODE -ne 0) { throw 'Still failing. Put the board in download mode: hold PRG, tap RST, release PRG - then run again. Close any serial monitor (Arduino IDE, etc.) that holds the port.' }
  }

  Write-Host @'

  ESP Radar v2 is flashed! Next steps:
    * Already set up before? It rejoins your WiFi by itself - open http://esp-radar.local
      (or the IP on the OLED). Keep the room still for the 10 s calibration.
    * New board / erased: the OLED shows "SETUP MODE". On your phone join WiFi
      ESP-Radar-XXXX (password: espradar), open  http://192.168.71.1  and pick your
      home 2.4 GHz WiFi. (v2 moved the setup AP from 192.168.4.1 to 192.168.71.1.)
    Short-press PRG = recalibrate  -  hold PRG 3 s = forget WiFi.
'@ -ForegroundColor Green
}

try { Install-EspRadar } catch { Write-Host "xx  $($_.Exception.Message)" -ForegroundColor Red }
