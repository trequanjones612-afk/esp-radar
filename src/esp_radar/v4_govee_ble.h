// Beta Govee BLE for bulbs with no LAN API (H6008 is one of those).
// No Govee cloud, no account, no API key.
//
// The ESP32-S3 build of arduino-esp32 3.3.x already runs NimBLE (CONFIG_BT_NIMBLE_ENABLED
// in the S3 sdkconfig). This uses that core BLE API, not a second NimBLE-Arduino copy,
// which would fight the core on 3.3+.
//
// Packets and UUID, from community projects (not from Govee):
//   * chvolkmann/govee_btled  govee_btled/bluetooth_led.py
//       UUID_CONTROL_CHARACTERISTIC = 00010203-0405-0607-0809-0a0b0c0d2b11
//       frame = 0x33, cmd, payload, zero-pad to 19 bytes, byte 19 = XOR of bytes 0..18
//   * fboyle2001/Govee-Homebridge README (sniffer-derived, same 20-byte layout):
//       on  33 01 01 00…00 33
//       off 33 01 00 00…00 32
//       brightness 33 04 <level 0x00-0xFF> 00…00 XOR
//   * Advertisement names ihoment_H6008_… seen in the wild:
//       Xample33/govee_H613_BTcontroller issue 1 (psi-cmd scan log).
//     Also accepted: Govee_ and GBK_ prefixes used by newer Govee firmware.
//
// H6008 writes are untested here. That issue also says the Wi-Fi bulb's command
// bytes were not identical to a BLE-only strip, so power/brightness may need a tweak.
//
// Keep-alive is only inside a command, never on a timer (a timer would hold the
// radio and drop CSI). H6006/H6008 at a weak RSSI ignore a single power write.
// The keep-alive frame used by Govee Homebridge sniffers is AA 01 00… checksum AB.
//
// Coexistence: BLE is inited on first use, then left idle (no scan, no connection).
// A scan is 4 s at 30 ms window / 100 ms interval (~30% airtime) and only when asked.
// A command is one short connection. Steady-state CSI should stay near the router's
// current rate; expect a dip only during that scan or connection. Not measured here.
#pragma once
#include <Arduino.h>
#include "v4_mem.h"
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLERemoteService.h>
#include <BLERemoteCharacteristic.h>

static const char *GOVEE_BLE_CHAR = "00010203-0405-0607-0809-0a0b0c0d2b11";
static const char *GOVEE_BLE_SVC = "00010203-0405-0607-0809-0a0b0c0d1910";

static const int GOVEE_BLE_MAX = 16;
struct GoveeBleHit {
  char mac[18];
  uint8_t addrType;
  int8_t rssi;
  char name[20];
  char model[8];
  bool used;
};
static GoveeBleHit *goveeHits = nullptr;
static int goveeHitN = 0;
static bool goveeBleReady = false;
static bool goveeBleScanning = false;
static BLEClient *goveeClient = nullptr;

static bool goveeBleStorage() {
  if (goveeHits) return true;
  goveeHits = (GoveeBleHit *)psAlloc(sizeof(GoveeBleHit) * GOVEE_BLE_MAX, "govee-ble");
  return goveeHits != nullptr;
}
static void goveeBleStorageFree() {
  free(goveeHits);
  goveeHits = nullptr;
  goveeHitN = 0;
}

static bool goveeNameOk(const String &n) {
  return n.startsWith("ihoment_") || n.startsWith("IHoment_") || n.startsWith("Govee_") || n.startsWith("govee_") ||
         n.startsWith("GBK_") || n.startsWith("gbk_");
}

static void goveeModel(const char *name, char *model, int cap) {
  model[0] = 0;
  const char *p = name;
  while (*p && !((*p == 'H' || *p == 'h') && p[1] >= '0' && p[1] <= '9')) p++;
  if (!*p) return;
  int i = 0;
  model[i++] = 'H';
  for (p++; *p >= '0' && *p <= '9' && i < cap - 1; p++) model[i++] = *p;
  model[i] = 0;
  if (i < 4) model[0] = 0;
}

class GoveeScanCb : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice dev) override {
    if (!goveeHits || !dev.haveName()) return;
    String n = dev.getName();
    if (!goveeNameOk(n)) return;
    String mac = dev.getAddress().toString();
    int slot = -1;
    for (int i = 0; i < goveeHitN; i++) if (!strcasecmp(goveeHits[i].mac, mac.c_str())) { slot = i; break; }
    if (slot < 0) {
      if (goveeHitN < GOVEE_BLE_MAX) slot = goveeHitN++;
      else {
        slot = 0;
        for (int i = 1; i < GOVEE_BLE_MAX; i++) if (goveeHits[i].rssi < goveeHits[slot].rssi) slot = i;
        if (dev.getRSSI() < goveeHits[slot].rssi) return;
      }
      memset(&goveeHits[slot], 0, sizeof(GoveeBleHit));
    }
    strncpy(goveeHits[slot].mac, mac.c_str(), sizeof goveeHits[slot].mac - 1);
    goveeHits[slot].addrType = dev.getAddress().getType();
    goveeHits[slot].rssi = (int8_t)dev.getRSSI();
    strncpy(goveeHits[slot].name, n.c_str(), sizeof goveeHits[slot].name - 1);
    goveeModel(n.c_str(), goveeHits[slot].model, sizeof goveeHits[slot].model);
    goveeHits[slot].used = true;
  }
};
static GoveeScanCb goveeScanCb;

static void goveeBleShutdown() {
  if (goveeClient && goveeClient->isConnected()) goveeClient->disconnect();
  goveeClient = nullptr;                 // BLEDevice::deinit deletes its client object
  if (goveeBleReady) {
    BLEDevice::deinit(true);             // release the controller so WiFi/CSI get the radio back
    goveeBleReady = false;
    Serial.println("# govee BLE deinit");
  }
  goveeBleScanning = false;
}

static bool goveeBleInit() {
  if (goveeBleReady) return true;
  if (!BLEDevice::init("esp-radar")) return false;
  goveeBleReady = true;
  Serial.println("# govee BLE init (only for this scan or write)");
  return true;
}

// Blocks for `seconds`. Caller must not be the async web task.
static int goveeBleScan(uint32_t seconds) {
  if (!goveeBleStorage() || !goveeBleInit()) return -1;
  if (seconds < 2) seconds = 2;
  if (seconds > 6) seconds = 6;
  goveeHitN = 0;
  memset(goveeHits, 0, sizeof(GoveeBleHit) * GOVEE_BLE_MAX);
  goveeBleScanning = true;
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&goveeScanCb, false);
  scan->setActiveScan(true);
  scan->setInterval(100);   // ms
  scan->setWindow(30);      // ms, ~30% of airtime so WiFi/CSI keep the rest
  Serial.printf("# govee BLE scan %lus, window 30/100 ms. CSI will dip until this returns, then should recover.\n",
                (unsigned long)seconds);
  scan->start(seconds, false);
  scan->clearResults();
  goveeBleScanning = false;
  int n = 0;
  for (int i = 0; i < goveeHitN; i++) if (goveeHits[i].used) n++;
  Serial.printf("# govee BLE scan done, %d bulb(s). Not a CSI measurement.\n", n);
  goveeBleShutdown();
  return n;
}

static void goveePacket(uint8_t pkt[20], uint8_t cmd, uint8_t a, uint8_t b = 0) {
  memset(pkt, 0, 20);
  pkt[0] = 0x33;
  pkt[1] = cmd;
  pkt[2] = a;
  pkt[3] = b;
  uint8_t x = 0;
  for (int i = 0; i < 19; i++) x ^= pkt[i];
  pkt[19] = x;
}
// AA 01 00… XOR = AB. Sent only while this command's connection is up.
static void goveeKeepalive(uint8_t pkt[20]) {
  memset(pkt, 0, 20);
  pkt[0] = 0xAA;
  pkt[1] = 0x01;
  uint8_t x = 0;
  for (int i = 0; i < 19; i++) x ^= pkt[i];
  pkt[19] = x;
}
// NimBLE: response=false is ble_gattc_write_no_rsp_flat. This characteristic is
// write-without-response. If that returns false, try a write-with-response once.
static bool goveeWritePkt(BLERemoteCharacteristic *ch, uint8_t pkt[20], bool &usedRsp) {
  if (ch->writeValue(pkt, 20, false)) { usedRsp = false; return true; }
  if (ch->writeValue(pkt, 20, true)) { usedRsp = true; return true; }
  return false;
}

static bool goveeBleWrite(const char *mac, uint8_t addrType, bool on, int briPct) {
  if (scanning) {
    snprintf(ltResult, sizeof ltResult, "BLE not started: WiFi scan in progress");
    return false;
  }
  if (!mac || !mac[0] || !goveeBleStorage() || !goveeBleInit()) {
    snprintf(ltResult, sizeof ltResult, "BLE init failed");
    return false;
  }
  if (!goveeClient) goveeClient = BLEDevice::createClient();
  if (!goveeClient) {
    snprintf(ltResult, sizeof ltResult, "BLE no client");
    goveeBleShutdown();
    return false;
  }
  if (goveeClient->isConnected()) goveeClient->disconnect();
  bool connected = false;
  for (int attempt = 0; attempt < 3 && !connected; attempt++) {
    if (scanning) break;
    BLEAddress pub(String(mac), addrType);
    if (goveeClient->connect(pub, addrType, 8000)) connected = true;
    else if (addrType == BLE_ADDR_PUBLIC &&
             goveeClient->connect(BLEAddress(String(mac), BLE_ADDR_RANDOM), BLE_ADDR_RANDOM, 8000))
      connected = true;
    else vTaskDelay(pdMS_TO_TICKS(400));
  }
  if (!connected) {
    snprintf(ltResult, sizeof ltResult, "BLE connect fail %s (3 tries, 8s)", mac);
    Serial.printf("# %s\n", ltResult);
    goveeBleShutdown();
    return false;
  }
  BLERemoteCharacteristic *ch = nullptr;
  BLERemoteService *svc = goveeClient->getService(GOVEE_BLE_SVC);
  if (svc) ch = svc->getCharacteristic(GOVEE_BLE_CHAR);
  if (!ch) {
    auto *all = goveeClient->getServices();
    if (all) {
      for (auto &kv : *all) {
        ch = kv.second->getCharacteristic(GOVEE_BLE_CHAR);
        if (ch) break;
      }
    }
  }
  if (!ch) {
    snprintf(ltResult, sizeof ltResult, "BLE connected %s, characteristic not found", mac);
    Serial.printf("# %s\n", ltResult);
    if (goveeClient->isConnected()) goveeClient->disconnect();
    goveeBleShutdown();
    return false;
  }
  bool rsp = false, ok = false;
  uint8_t pkt[20];
  goveeKeepalive(pkt);
  bool ka = goveeWritePkt(ch, pkt, rsp);
  int powerOk = 0;
  goveePacket(pkt, 0x01, on ? 0x01 : 0x00);
  for (int n = 0; n < 3; n++) {
    if (goveeWritePkt(ch, pkt, rsp)) powerOk++;
    vTaskDelay(pdMS_TO_TICKS(80));
  }
  bool briOk = true;
  if (on && powerOk) {
    int bri = constrain(briPct, 1, 100);
    uint8_t level = (uint8_t)((bri * 255) / 100);
    if (!level) level = 1;
    goveePacket(pkt, 0x04, level);
    briOk = false;
    for (int n = 0; n < 2; n++) if (goveeWritePkt(ch, pkt, rsp)) briOk = true;
  }
  goveeKeepalive(pkt);
  goveeWritePkt(ch, pkt, rsp);
  ok = powerOk > 0 && briOk;
  if (goveeClient->isConnected()) goveeClient->disconnect();
  snprintf(ltResult, sizeof ltResult, "BLE %s connected, char ok, keep-alive %s, power %d/3 %s%s",
           mac, ka ? "ok" : "fail", powerOk, rsp ? "with-response" : "no-response",
           on ? (briOk ? ", brightness ok" : ", brightness fail") : "");
  Serial.printf("# %s\n", ltResult);
  goveeBleShutdown();
  return ok;
}
