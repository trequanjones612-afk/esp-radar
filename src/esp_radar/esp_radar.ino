/*
  ESP Radar v2 - WiFi CSI motion meter, WiFi coverage mapper, network + device scanner
  for the Heltec WiFi LoRa 32 V4 (ESP32-S3 + SSD1306/SSD1315 128x64 OLED).

  Motion: watches how the WiFi signal from your router fluctuates (Channel State Information,
  per-subcarrier amplitude) and reports a 0-100 motion INTENSITY. It does not see, count or
  locate people.

  MIT License - see LICENSE.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SSD1306Wire.h>
#include <time.h>
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "lwip/etharp.h"
#include "lwip/tcpip.h"
#include "web_assets.h"
#include "oui_table.h"

#define FW_VERSION "2.0.0"

// ---------- Heltec WiFi LoRa 32 V4 pins (Heltec pins_arduino.h + Meshtastic variant) ----------
static const int PIN_VEXT = 36;   // Vext power switch, ACTIVE LOW (powers the OLED)
static const int PIN_OLED_SDA = 17;
static const int PIN_OLED_SCL = 18;
static const int PIN_OLED_RST = 21;
static const int PIN_PRG = 0;     // PRG / BOOT button, active low
static const int PIN_LED = 35;    // white LED

// ---------- tuning ----------
static const int   WIN = 50;                 // moving-variance window (packets, ~0.5-1 s)
static const uint32_t CAL_MS = 10000;        // calibration time
static const int   PING_INTERVAL_MS = 15;    // ICMP echo cadence to the gateway (-> CSI frames)
static const char *AP_PASS = "espradar";     // setup AP password (WPA2)
// SoftAP subnet. v1 used 192.168.4.1, which collides with many home LANs (and broke the gateway ping).
static IPAddress AP_IP(192, 168, 71, 1);
static const IPAddress AP_IP_ALT(172, 31, 71, 1);   // used if the home LAN is 192.168.71.0/24

// ---------- globals ----------
SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL);
AsyncWebServer server(80);
AsyncEventSource events("/events");
DNSServer dns;
Preferences prefs;     // namespace "espradar", keys ssid/pass/sens - unchanged since v1 so creds survive

String staSsid, staPass, apName, staIp = "", lastErr = "";
bool setupMode = false, staUp = false, mdnsUp = false;
volatile bool reqRecal = false, reqRestart = false;
uint32_t restartAt = 0;
volatile int sensitivity = 5;         // 1..10

// ---------- CSI plumbing ----------
struct CsiPkt { int8_t buf[128]; uint16_t len; int8_t rssi; };
static QueueHandle_t csiQ;
static volatile uint8_t apBssid[6];
static volatile bool bssidValid = false;
static volatile uint32_t csiAll = 0, csiOurs = 0, csiDrop = 0, pingOk = 0, arpTx = 0;
static esp_ping_handle_t pingH = nullptr;

struct State {
  float level = 0, prox = 0;
  bool motion = false, calibrating = true;
  float calProgress = 0;
  float score = 0;          // robust variance ratio vs baseline (1.0 = like the still room)
  float rssi = -100, pps = 0, ppsAll = 0, pingRate = 0;
  uint32_t evals = 0;
} st;
static portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
static State snap() { State s; portENTER_CRITICAL(&stMux); s = st; portEXIT_CRITICAL(&stMux); return s; }

static void csiCb(void *ctx, wifi_csi_info_t *info) {
  csiAll = csiAll + 1;
  if (!info || !info->buf || info->len < 128 || !bssidValid) return;
  for (int i = 0; i < 6; i++) if (info->mac[i] != apBssid[i]) return;   // any frame sent by our AP
  csiOurs = csiOurs + 1;
  CsiPkt p;
  memcpy(p.buf, info->buf, 128);       // first 128 bytes = LLTF, 64 subcarriers (imag, real)
  p.len = 128;
  p.rssi = info->rx_ctrl.rssi;
  if (xQueueSend(csiQ, &p, 0) != pdTRUE) csiDrop = csiDrop + 1;
}

// LLTF index 0..31 = subcarrier 0..31, 32..63 = -32..-1. Usable: 2..26, 38..63 (skip DC, guards, first word)
static inline bool usableSc(int k) { return (k >= 2 && k <= 26) || (k >= 38 && k <= 63); }
static const int NSC = 51;

static float ring[WIN][NSC];
static float rsum[NSC], rsq[NSC];
static float emaM[NSC], emaD[NSC];
static bool emaInit = false;
static int ringPos = 0, ringFill = 0;
static float rssiRing[WIN]; static float rssiSum = 0, rssiSq = 0;
static float baseVar[NSC];

// activity: one bucket per minute of uptime, last 60 minutes
struct Bucket { uint8_t avg, peak, motionPct; };
static Bucket act[60]; static int actCount = 0;
static float bSum = 0; static uint16_t bN = 0, bMot = 0; static uint8_t bPeak = 0; static uint32_t bMinute = 0;

static int cmpf(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }

static void csiTask(void *) {
  CsiPkt p;
  float amp[NSC], ratio[NSC];
  uint32_t calStart = 0, calN = 0; static double calVar[NSC]; double calRssi = 0;
  float rssiBase = -60, lvl = 0, prox = 0, scoreEma = 1;
  uint32_t lastAbove = 0, lastEval = 0, ppsT = millis(), lastOurs = 0, lastAll = 0, lastPing = 0;
  float pps = 0, ppsAll = 0, pingRate = 0;
  bool motion = false, calibrating = true;
  for (;;) {
    bool got = xQueueReceive(csiQ, &p, pdMS_TO_TICKS(50)) == pdTRUE;
    uint32_t now = millis();
    if (reqRecal) {
      reqRecal = false; calibrating = true; calStart = 0; calN = 0; calRssi = 0;
      memset(calVar, 0, sizeof calVar);
      ringFill = ringPos = 0; memset(rsum, 0, sizeof rsum); memset(rsq, 0, sizeof rsq);
      rssiSum = rssiSq = 0; lvl = 0; prox = 0; motion = false; emaInit = false; scoreEma = 1;
    }
    if (got) {
      int j = 0; float mean = 0;
      for (int k = 0; k < 64; k++) {
        if (!usableSc(k)) continue;
        float im = p.buf[2 * k], re = p.buf[2 * k + 1];
        amp[j] = sqrtf(re * re + im * im); mean += amp[j]; j++;
      }
      if (mean > 1.0f) {
        mean /= NSC;
        if (!emaInit) { for (int i = 0; i < NSC; i++) { emaM[i] = amp[i] / mean; emaD[i] = 0.05f * emaM[i] + 1e-3f; } emaInit = true; }
        float *slot = ring[ringPos];
        bool full = ringFill == WIN;
        for (int i = 0; i < NSC; i++) {
          float a = amp[i] / mean;                       // per-packet normalisation (cancels AGC gain steps)
          float dev = a - emaM[i];
          float lim = 3.0f * 1.4826f * emaD[i] + 1e-3f;  // Hampel-style outlier clamp
          if (dev > lim) a = emaM[i] + lim; else if (dev < -lim) a = emaM[i] - lim;
          emaD[i] = emaD[i] * 0.97f + fabsf(a - emaM[i]) * 0.03f;
          emaM[i] = emaM[i] * 0.98f + a * 0.02f;
          if (full) { rsum[i] -= slot[i]; rsq[i] -= slot[i] * slot[i]; }
          slot[i] = a; rsum[i] += a; rsq[i] += a * a;
        }
        if (full) { rssiSum -= rssiRing[ringPos]; rssiSq -= rssiRing[ringPos] * rssiRing[ringPos]; }
        rssiRing[ringPos] = p.rssi; rssiSum += p.rssi; rssiSq += (float)p.rssi * p.rssi;
        ringPos = (ringPos + 1) % WIN; if (!full) ringFill++;
      }
    }
    if (now - ppsT >= 1000) {
      float dt = (now - ppsT) / 1000.0f;
      uint32_t o = csiOurs, a = csiAll, pg = pingOk;
      pps = (o - lastOurs) / dt; ppsAll = (a - lastAll) / dt; pingRate = (pg - lastPing) / dt;
      lastOurs = o; lastAll = a; lastPing = pg; ppsT = now;
    }
    if (now - lastEval < 100) continue;      // evaluate at 10 Hz
    lastEval = now;
    float rssiMean = -100, rssiStd = 0;
    bool ready = ringFill >= WIN / 2;
    float var[NSC];
    if (ready) {
      for (int i = 0; i < NSC; i++) { float m = rsum[i] / ringFill; float v = rsq[i] / ringFill - m * m; var[i] = v > 0 ? v : 0; }
      rssiMean = rssiSum / ringFill;
      float rv = rssiSq / ringFill - rssiMean * rssiMean; rssiStd = rv > 0 ? sqrtf(rv) : 0;
    }
    float calProg = 0;
    if (calibrating) {
      if (ready) {
        if (calStart == 0) calStart = now;
        for (int i = 0; i < NSC; i++) calVar[i] += var[i];
        calN++; calRssi += rssiMean;
        calProg = min(1.0f, (now - calStart) / (float)CAL_MS);
        if (now - calStart >= CAL_MS && calN >= 30) {
          float tmp[NSC];
          for (int i = 0; i < NSC; i++) { baseVar[i] = calVar[i] / calN; tmp[i] = baseVar[i]; }
          qsort(tmp, NSC, sizeof(float), cmpf);
          float floorV = max(tmp[NSC / 2] * 0.25f, 1e-6f);    // no subcarrier may have a near-zero baseline
          for (int i = 0; i < NSC; i++) baseVar[i] = max(baseVar[i], floorV);
          rssiBase = calRssi / calN; calibrating = false; lvl = 0; scoreEma = 1;
          Serial.printf("# calibrated: median baseline var=%.6f rssi=%.1f samples=%lu csi=%.0f/s\n", tmp[NSC / 2], rssiBase, (unsigned long)calN, pps);
        }
      }
    } else if (ready) {
      for (int i = 0; i < NSC; i++) ratio[i] = var[i] / baseVar[i];
      qsort(ratio, NSC, sizeof(float), cmpf);
      float score = ratio[(NSC * 3) / 4];                   // 75th percentile: robust to a few noisy subcarriers
      scoreEma = scoreEma * 0.6f + score * 0.4f;
      float span = 1.0f + (11 - sensitivity) * 0.8f;        // sens 5 -> 25% at ~2.4x baseline variance
      float target = constrain(100.0f * (scoreEma - 1.0f) / span, 0.0f, 100.0f);
      if (pps < 5) target = 0;                              // not enough CSI to say anything
      lvl = lvl * 0.7f + target * 0.3f;
      float rssiTerm = constrain(rssiStd * 18.0f + (rssiBase - rssiMean) * 4.0f, 0.0f, 100.0f);
      float pt = lvl < 3 ? 0 : constrain(lvl * 0.75f + rssiTerm * 0.25f, 0.0f, 100.0f);
      prox = prox * 0.8f + pt * 0.2f;
      if (lvl >= 25) { motion = true; lastAbove = now; }
      else if (motion && lvl < 12 && now - lastAbove > 2000) motion = false;
      else if (lvl >= 12) lastAbove = now;
      if (!motion && lvl < 8) {                             // adaptive baseline while the room is quiet
        for (int i = 0; i < NSC; i++) baseVar[i] = baseVar[i] * 0.998f + max(var[i], 1e-7f) * 0.002f;
        rssiBase = rssiBase * 0.995f + rssiMean * 0.005f;
      }
    }
    // per-minute activity buckets
    uint32_t minute = now / 60000;
    if (minute != bMinute) {
      if (bN) {
        if (actCount == 60) { memmove(act, act + 1, sizeof(Bucket) * 59); actCount = 59; }
        act[actCount++] = { (uint8_t)(bSum / bN), bPeak, (uint8_t)(100 * bMot / bN) };
      }
      bSum = 0; bN = 0; bMot = 0; bPeak = 0; bMinute = minute;
    }
    if (!calibrating) { bSum += lvl; bN++; if (motion) bMot++; if (lvl > bPeak) bPeak = (uint8_t)lvl; }

    portENTER_CRITICAL(&stMux);
    st.level = calibrating ? 0 : lvl; st.prox = calibrating ? 0 : prox; st.motion = calibrating ? false : motion;
    st.calibrating = calibrating; st.calProgress = calProg; st.score = calibrating ? 0 : scoreEma;
    st.rssi = rssiMean; st.pps = pps; st.ppsAll = ppsAll; st.pingRate = pingRate; st.evals++;
    portEXIT_CRITICAL(&stMux);
  }
}

// ---------- traffic generator: ICMP echo + ARP probes to the gateway ----------
static void onPingOk(esp_ping_handle_t, void *) { pingOk = pingOk + 1; }
static struct netif *staNetif() {
  esp_netif_t *n = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  return n ? (struct netif *)esp_netif_get_netif_impl(n) : nullptr;
}
static void startPing() {
  if (pingH) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0)) return;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  ip_addr_t target; IP_ADDR4(&target, gw[0], gw[1], gw[2], gw[3]);
  cfg.target_addr = target;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = PING_INTERVAL_MS;
  cfg.timeout_ms = 300;
  cfg.data_size = 16;
  cfg.task_stack_size = 3072;
  cfg.task_prio = 2;
  cfg.interface = esp_netif_get_netif_impl_index(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"));  // never via the SoftAP
  esp_ping_callbacks_t cbs = {};
  cbs.on_ping_success = onPingOk;
  if (esp_ping_new_session(&cfg, &cbs, &pingH) == ESP_OK) esp_ping_start(pingH); else pingH = nullptr;
}
static void stopPing() { if (pingH) { esp_ping_stop(pingH); esp_ping_delete_session(pingH); pingH = nullptr; } }
// If the router rate-limits ICMP, unicast ARP replies from the gateway still give us frames to measure.
static void arpProbeGateway() {
  struct netif *nif = staNetif(); if (!nif) return;
  IPAddress gw = WiFi.gatewayIP(); ip4_addr_t ip; IP4_ADDR(&ip, gw[0], gw[1], gw[2], gw[3]);
  LOCK_TCPIP_CORE(); etharp_request(nif, &ip); UNLOCK_TCPIP_CORE();
  arpTx = arpTx + 1;
}

static void startCsi() {
  wifi_csi_config_t c = {};
  c.lltf_en = true; c.htltf_en = true; c.stbc_htltf2_en = true; c.ltf_merge_en = true;
  c.channel_filter_en = false; c.manu_scale = false; c.shift = 0;
  esp_wifi_set_csi_config(&c);
  esp_wifi_set_csi_rx_cb(csiCb, nullptr);
  esp_wifi_set_csi(true);
}

// ---------- motion events ----------
struct Ev { uint32_t ms; bool start; uint8_t peak; uint32_t durMs; };
static Ev evlog[30]; static int evCount = 0;
static void logEvent(bool start, uint8_t peak, uint32_t dur) {
  if (evCount == 30) { memmove(evlog, evlog + 1, sizeof(Ev) * 29); evCount = 29; }
  evlog[evCount++] = { millis(), start, peak, dur };
}

static String jsonEsc(const String &s) {
  String o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c >= 0x20) o += c; } return o;
}
static String macStr(const uint8_t *m) { char b[18]; snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return b; }

static String stateJson() {
  State s = snap();
  char b[640];
  snprintf(b, sizeof b,
    "{\"m\":%.1f,\"p\":%.1f,\"s\":\"%s\",\"cal\":%s,\"cp\":%.2f,\"rssi\":%.1f,\"pps\":%.0f,\"ppsAll\":%.0f,\"ping\":%.0f,"
    "\"score\":%.2f,\"sens\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"ap\":\"%s\",\"apip\":\"%s\",\"ch\":%d,"
    "\"up\":%lu,\"setup\":%s,\"sta\":%s,\"now\":%lu,\"v\":\"" FW_VERSION "\"}",
    s.level, s.prox, s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"),
    s.calibrating ? "true" : "false", s.calProgress, s.rssi, s.pps, s.ppsAll, s.pingRate, s.score,
    sensitivity, staIp.c_str(), jsonEsc(staSsid).c_str(), apName.c_str(), AP_IP.toString().c_str(), staUp ? (int)WiFi.channel() : 0,
    (unsigned long)(millis() / 1000), setupMode ? "true" : "false", staUp ? "true" : "false", (unsigned long)millis());
  return String(b);
}

static String eventsJson() {
  String o = "{\"now\":" + String(millis()) + ",\"ev\":[";
  for (int i = 0; i < evCount; i++) {
    if (i) o += ",";
    o += "{\"t\":" + String(evlog[i].ms) + ",\"start\":" + (evlog[i].start ? "true" : "false") + ",\"peak\":" + String(evlog[i].peak) + ",\"dur\":" + String(evlog[i].durMs) + "}";
  }
  return o + "]}";
}

static String activityJson() {
  // buckets are oldest -> newest; the last one ended at the start of the current uptime minute
  String o = "{\"now\":" + String(millis()) + ",\"minuteStart\":" + String((millis() / 60000) * 60000) + ",\"cur\":{\"avg\":" +
             String(bN ? (int)(bSum / bN) : 0) + ",\"peak\":" + String(bPeak) + ",\"mot\":" + String(bN ? (int)(100 * bMot / bN) : 0) + "},\"b\":[";
  for (int i = 0; i < actCount; i++) {
    if (i) o += ",";
    o += "[" + String(act[i].avg) + "," + String(act[i].peak) + "," + String(act[i].motionPct) + "]";
  }
  return o + "]}";
}

// ---------- RSSI survey sample (coverage mapper) ----------
struct Sample { bool busy = false; int want = 10, n = 0; int vals[30]; uint32_t nextAt = 0; uint32_t id = 0; } smp;
static void sampleTick() {
  if (!smp.busy || millis() < smp.nextAt) return;
  if (staUp) smp.vals[smp.n++] = WiFi.RSSI();
  smp.nextAt = millis() + 150;
  if (smp.n >= smp.want || !staUp) smp.busy = false;
}
static String sampleJson() {
  String o = "{\"id\":" + String(smp.id) + ",\"busy\":" + (smp.busy ? "true" : "false") + ",\"n\":" + String(smp.n);
  if (!smp.busy && smp.n > 0) {
    float sum = 0; int mn = 0, mx = -200;
    for (int i = 0; i < smp.n; i++) { sum += smp.vals[i]; mn = min(mn, smp.vals[i]); mx = max(mx, smp.vals[i]); }
    float avg = sum / smp.n, sq = 0; for (int i = 0; i < smp.n; i++) sq += (smp.vals[i] - avg) * (smp.vals[i] - avg);
    o += ",\"avg\":" + String(avg, 1) + ",\"min\":" + String(mn) + ",\"max\":" + String(mx) + ",\"sd\":" + String(sqrtf(sq / smp.n), 1);
    o += ",\"bssid\":\"" + (staUp ? WiFi.BSSIDstr() : String("")) + "\",\"ch\":" + String(staUp ? (int)WiFi.channel() : 0) + ",\"ssid\":\"" + jsonEsc(staSsid) + "\"";
  }
  return o + "}";
}

// ---------- WiFi scan (networks view, setup page, per-point AP survey) ----------
static String scanCache = "[]"; static uint32_t scanAt = 0, scanSeq = 0; static bool scanning = false;
static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "OPEN"; case WIFI_AUTH_WEP: return "WEP"; case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2"; case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2"; case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3"; case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3"; default: return "OTHER";
  }
}
static void startScan() { if (!scanning) { scanning = true; WiFi.scanDelete(); WiFi.scanNetworks(true, true, false, 120); } }
static void scanTick() {
  if (!scanning) return;
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  if (n < 0) return;
  String o = "[";
  for (int i = 0; i < n && i < 40; i++) {
    if (i) o += ",";
    o += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"bssid\":\"" + WiFi.BSSIDstr(i) + "\",\"ch\":" + String(WiFi.channel(i)) +
         ",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"auth\":\"" + authName(WiFi.encryptionType(i)) + "\",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
  }
  scanCache = o + "]"; scanAt = millis(); scanSeq++;
  WiFi.scanDelete();
}

// ---------- device discovery (ARP sweep of the home /24, chunked, non-blocking) ----------
struct Dev { uint8_t mac[6]; uint8_t ip4; uint32_t first, last; char name[16]; bool fresh, known; };
static Dev devs[96]; static int devCount = 0;
static bool devScanning = false; static int devNext = 1; static uint32_t devWaitUntil = 0, devScanStart = 0, devScanDone = 0;
static bool nbPending = false; static uint32_t nbUntil = 0;
static WiFiUDP nbUdp;
static uint8_t knownMacs[64][6]; static int knownCount = 0;

static const char *vendorOf(const uint8_t *m) {
  if (m[0] & 0x02) return "Private (randomized) MAC";
  uint32_t key = ((uint32_t)m[0] << 16) | ((uint32_t)m[1] << 8) | m[2];
  int lo = 0, hi = OUI_COUNT - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2; uint32_t e = pgm_read_dword(&OUI_TABLE[mid]); uint32_t k = e >> 8;
    if (k == key) return OUI_VENDORS[e & 0xff];
    if (k < key) lo = mid + 1; else hi = mid - 1;
  }
  return "";
}
static void loadKnown() {
  Preferences p; p.begin("espradar-dev", true);
  knownCount = min((int)(p.getBytesLength("known") / 6), 64);
  if (knownCount) p.getBytes("known", knownMacs, knownCount * 6);
  p.end();
}
static void saveKnown() { Preferences p; p.begin("espradar-dev", false); p.putBytes("known", knownMacs, knownCount * 6); p.end(); }
static bool isKnown(const uint8_t *m) { for (int i = 0; i < knownCount; i++) if (!memcmp(knownMacs[i], m, 6)) return true; return false; }

static void harvestArp() {
  struct netif *nif = staNetif(); if (!nif) return;
  uint32_t now = millis();
  LOCK_TCPIP_CORE();
  for (size_t i = 0; i < ARP_TABLE_SIZE; i++) {
    ip4_addr_t *ip; struct netif *n; struct eth_addr *eth;
    if (etharp_get_entry(i, &ip, &n, &eth) && n == nif) {
      uint8_t last = ip4_addr4(ip);
      int k = 0; for (; k < devCount; k++) if (!memcmp(devs[k].mac, eth->addr, 6)) break;
      if (k == devCount) {
        if (devCount >= 96) continue;
        memset(&devs[k], 0, sizeof(Dev)); memcpy(devs[k].mac, eth->addr, 6); devs[k].first = now; devCount++;
        devs[k].known = isKnown(eth->addr);
        devs[k].fresh = !devs[k].known && knownCount > 0;   // "new" only once a first scan established the baseline
      }
      devs[k].ip4 = last; devs[k].last = now;
    }
  }
  UNLOCK_TCPIP_CORE();
}
static void addSelf() {
  uint8_t m[6]; WiFi.macAddress(m); uint32_t now = millis();
  int k = 0; for (; k < devCount; k++) if (!memcmp(devs[k].mac, m, 6)) break;
  if (k == devCount && devCount < 96) { memset(&devs[k], 0, sizeof(Dev)); memcpy(devs[k].mac, m, 6); devs[k].first = now; devs[k].known = true; devCount++; }
  devs[k].ip4 = WiFi.localIP()[3]; devs[k].last = now; strncpy(devs[k].name, "esp-radar", 15);
}
static void sendNbstat(IPAddress ip) {
  static const uint8_t q[50] = { 0x52, 0x44, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20,
    'C','K','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A','A',
    0x00, 0x00, 0x21, 0x00, 0x01 };
  nbUdp.beginPacket(ip, 137); nbUdp.write(q, sizeof q); nbUdp.endPacket();
}
static void nbPoll() {
  int len;
  while ((len = nbUdp.parsePacket()) > 0) {
    uint8_t b[300]; int n = nbUdp.read(b, sizeof b);
    IPAddress from = nbUdp.remoteIP();
    if (n < 57 + 18) continue;
    int names = b[56];
    for (int i = 0; i < names && 57 + i * 18 + 18 <= n; i++) {
      const uint8_t *e = b + 57 + i * 18; uint16_t flags = (e[16] << 8) | e[17];
      if (e[15] != 0x00 || (flags & 0x8000)) continue;          // workstation name, unique
      char nm[16]; memcpy(nm, e, 15); nm[15] = 0; for (int t = 14; t >= 0 && nm[t] == ' '; t--) nm[t] = 0;
      for (int k = 0; k < devCount; k++) if (devs[k].ip4 == from[3] && !devs[k].name[0]) { strncpy(devs[k].name, nm, 15); }
      break;
    }
  }
}
static void devTick() {
  if (nbPending) { nbPoll(); if (millis() > nbUntil) nbPending = false; }
  if (!devScanning || millis() < devWaitUntil) return;
  if (devNext > 1) harvestArp();
  if (!staUp) { devScanning = false; return; }
  if (devNext > 254) {
    devScanning = false; devScanDone = millis(); addSelf();
    // remember MACs so later scans can flag new devices
    for (int k = 0; k < devCount && knownCount < 64; k++) if (!(devs[k].mac[0] & 0x02) && !isKnown(devs[k].mac)) memcpy(knownMacs[knownCount++], devs[k].mac, 6);
    saveKnown();
    IPAddress base = WiFi.localIP();
    for (int k = 0; k < devCount; k++) if (!devs[k].name[0]) sendNbstat(IPAddress(base[0], base[1], base[2], devs[k].ip4));
    nbPending = true; nbUntil = millis() + 2500;
    return;
  }
  struct netif *nif = staNetif(); if (!nif) { devScanning = false; return; }
  IPAddress me = WiFi.localIP();
  LOCK_TCPIP_CORE();
  for (int i = 0; i < 6 && devNext <= 254; i++, devNext++) {
    if (devNext == me[3]) continue;
    ip4_addr_t ip; IP4_ADDR(&ip, me[0], me[1], me[2], devNext);
    etharp_request(nif, &ip);
  }
  UNLOCK_TCPIP_CORE();
  devWaitUntil = millis() + 320;
}
static String devicesJson() {
  IPAddress base = staUp ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  IPAddress gw = WiFi.gatewayIP();
  String o = "{\"now\":" + String(millis()) + ",\"scanning\":" + (devScanning || nbPending ? "true" : "false") +
             ",\"progress\":" + String(devScanning ? (devNext * 100) / 255 : 100) + ",\"done\":" + String(devScanDone) +
             ",\"subnet\":\"" + String(base[0]) + "." + String(base[1]) + "." + String(base[2]) + ".0/24\",\"known\":" + String(knownCount) + ",\"d\":[";
  for (int k = 0; k < devCount; k++) {
    if (k) o += ",";
    String role = devs[k].ip4 == gw[3] ? "router" : (devs[k].ip4 == base[3] ? "this radar" : "");
    o += "{\"ip\":\"" + String(base[0]) + "." + String(base[1]) + "." + String(base[2]) + "." + String(devs[k].ip4) + "\",\"mac\":\"" + macStr(devs[k].mac) +
         "\",\"vendor\":\"" + vendorOf(devs[k].mac) + "\",\"name\":\"" + jsonEsc(devs[k].name) + "\",\"first\":" + String(devs[k].first) +
         ",\"last\":" + String(devs[k].last) + ",\"new\":" + (devs[k].fresh ? "true" : "false") + ",\"role\":\"" + role + "\"}";
  }
  return o + "]}";
}

// ---------- web server ----------
static void sendGz(AsyncWebServerRequest *r, const uint8_t *d, size_t n, const char *type) {
  AsyncWebServerResponse *res = r->beginResponse(200, type, d, n);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  r->send(res);
}
static void sendJson(AsyncWebServerRequest *r, const String &s) { r->send(200, "application/json", s); }

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (setupMode) { r->redirect("/setup"); return; }
    sendGz(r, INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html");
  });
  server.on("/app", HTTP_GET, [](AsyncWebServerRequest *r) { sendGz(r, INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html"); });
  server.on("/radar", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/app"); });
  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *r) { sendGz(r, SETUP_HTML_GZ, SETUP_HTML_GZ_LEN, "text/html"); });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, stateJson()); });
  server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, eventsJson()); });
  server.on("/api/activity", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, activityJson()); });
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    bool force = r->hasParam("force");
    if (force || scanAt == 0 || millis() - scanAt > 20000) startScan();
    sendJson(r, "{\"scanning\":" + String(scanning ? "true" : "false") + ",\"seq\":" + String(scanSeq) + ",\"age\":" +
                String(scanAt ? (millis() - scanAt) / 1000 : -1) + ",\"bssid\":\"" + (staUp ? WiFi.BSSIDstr() : String("")) + "\",\"nets\":" + scanCache + "}");
  });
  server.on("/api/sample", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    smp.want = r->hasParam("n", true) ? constrain(r->getParam("n", true)->value().toInt(), 3, 30) : 10;
    smp.n = 0; smp.busy = true; smp.nextAt = 0; smp.id++;
    sendJson(r, sampleJson());
  });
  server.on("/api/sample", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, sampleJson()); });
  server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, devicesJson()); });
  server.on("/api/devices/scan", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    if (!devScanning) { devScanning = true; devNext = 1; devWaitUntil = 0; devScanStart = millis(); }
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("ssid", true)) { r->send(400, "text/plain", "missing ssid"); return; }
    String s = r->getParam("ssid", true)->value();
    String p = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    if (s.length() == 0 || s.length() > 32 || p.length() > 64) { r->send(400, "text/plain", "bad ssid/password length"); return; }
    prefs.putString("ssid", s); prefs.putString("pass", p);
    sendJson(r, "{\"ok\":true}");
    reqRestart = true; restartAt = millis() + 1500;
  });
  server.on("/api/recal", HTTP_POST, [](AsyncWebServerRequest *r) { reqRecal = true; sendJson(r, "{\"ok\":true}"); });
  server.on("/api/sens", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (r->hasParam("v", true)) { sensitivity = constrain(r->getParam("v", true)->value().toInt(), 1, 10); prefs.putInt("sens", sensitivity); }
    sendJson(r, "{\"sens\":" + String(sensitivity) + "}");
  });
  server.on("/api/forget", HTTP_POST, [](AsyncWebServerRequest *r) {
    prefs.remove("ssid"); prefs.remove("pass");
    sendJson(r, "{\"ok\":true}");
    reqRestart = true; restartAt = millis() + 1500;
  });
  server.onNotFound([](AsyncWebServerRequest *r) {
    if (setupMode) r->redirect("http://" + AP_IP.toString() + "/setup"); else r->redirect("/");
  });
  events.onConnect([](AsyncEventSourceClient *c) { c->send(stateJson().c_str(), "state", millis(), 1000); });
  server.addHandler(&events);
  server.begin();
}

// ---------- OLED: real motion bar + last-2-minutes history, no decorative blips ----------
static uint8_t hist[128]; static int histN = 0;   // one column per second
static float secPeak = 0; static uint32_t secAt = 0;

static void oledInit() {
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);  // power on OLED (active low)
  delay(50);
  pinMode(PIN_OLED_RST, OUTPUT); digitalWrite(PIN_OLED_RST, LOW); delay(20); digitalWrite(PIN_OLED_RST, HIGH); delay(20);
  display.init();
  display.flipScreenVertically();
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
}

static void drawOled() {
  State s = snap();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  if (setupMode) {
    display.drawString(0, 0, "SETUP MODE  v" FW_VERSION);
    display.drawString(0, 14, "Join WiFi:");
    display.drawString(0, 26, apName);
    display.drawString(0, 38, String("pw: ") + AP_PASS);
    display.drawString(0, 52, "open " + AP_IP.toString());
    display.display(); return;
  }
  display.drawString(0, 0, staUp ? staIp : String("joining WiFi..."));
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 0, s.calibrating ? String("CAL") : String((int)s.level) + "%");
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawHorizontalLine(0, 12, 128);
  if (!staUp) {
    display.drawString(0, 16, lastErr.length() ? lastErr : String("connecting..."));
    display.drawString(0, 28, "Setup AP: " + apName.substring(10));
    display.drawString(0, 40, "pw espradar");
    display.drawString(0, 52, AP_IP.toString());
  } else if (s.calibrating) {
    display.drawString(0, 15, "CALIBRATING - keep still");
    display.drawProgressBar(0, 30, 127, 9, (uint8_t)(s.calProgress * 100));
    display.drawString(0, 44, String("CSI ") + (int)s.pps + "/s  ping " + (int)s.pingRate + "/s");
  } else {
    display.setFont(ArialMT_Plain_16);
    display.drawString(0, 13, s.pps < 5 ? "NO CSI" : (s.motion ? "MOTION" : "STILL"));
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 17, String((int)s.pps) + " csi/s");
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    // motion bar with threshold tick at 25%
    display.drawRect(0, 32, 128, 8);
    display.fillRect(1, 33, (int)(126 * s.level / 100), 6);
    for (int y = 30; y < 42; y += 2) display.setPixel(32, y);
    // history: last 128 s, one column per second (peak level)
    display.drawHorizontalLine(0, 63, 128);
    for (int i = 0; i < histN; i++) { int h = hist[i] * 20 / 100; if (h) display.drawVerticalLine(128 - histN + i, 63 - h, h); }
  }
  display.display();
}

// ---------- WiFi / AP ----------
static void startAp() {
  WiFi.softAPConfig(AP_IP, AP_IP, IPAddress(255, 255, 255, 0));
  WiFi.softAP(apName.c_str(), AP_PASS);
}

static uint32_t btnDown = 0; static bool btnLong = false;
static void handleButton() {
  bool pressed = digitalRead(PIN_PRG) == LOW;
  uint32_t now = millis();
  if (pressed && btnDown == 0) { btnDown = now; btnLong = false; }
  if (pressed && btnDown && !btnLong && now - btnDown > 3000) {
    btnLong = true;
    display.clear(); display.setFont(ArialMT_Plain_10); display.setTextAlignment(TEXT_ALIGN_LEFT);
    display.drawString(0, 20, "WiFi settings erased"); display.drawString(0, 34, "restarting..."); display.display();
    Serial.println("# PRG long-press: erasing WiFi credentials");
    prefs.remove("ssid"); prefs.remove("pass");
    reqRestart = true; restartAt = now + 1200;
  }
  if (!pressed && btnDown) {
    if (!btnLong && now - btnDown > 40) { reqRecal = true; Serial.println("# PRG: recalibrating"); }
    btnDown = 0;
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(PIN_PRG, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
  oledInit();
  display.setFont(ArialMT_Plain_16); display.drawString(10, 10, "ESP RADAR");
  display.setFont(ArialMT_Plain_10); display.drawString(10, 34, "motion - coverage - LAN"); display.drawString(10, 48, "v" FW_VERSION); display.display();

  prefs.begin("espradar", false);
  staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("pass", "");
  sensitivity = prefs.getInt("sens", 5);
  setupMode = staSsid.length() == 0;
  loadKnown();

  uint8_t mac[6]; WiFi.macAddress(mac);
  char nm[24]; snprintf(nm, sizeof nm, "ESP-Radar-%02X%02X", mac[4], mac[5]); apName = nm;

  csiQ = xQueueCreate(64, sizeof(CsiPkt));
  xTaskCreatePinnedToCore(csiTask, "csi", 8192, nullptr, 3, nullptr, 0);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);
  esp_wifi_set_ps(WIFI_PS_NONE);        // no modem sleep: the radio hears every frame
  startAp();
  if (setupMode) {
    dns.start(53, "*", AP_IP);
    startScan();
  } else {
    WiFi.setHostname("esp-radar");
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  startCsi();
  nbUdp.begin(41137);
  setupWeb();
  delay(600);
  Serial.println("# ESP Radar " FW_VERSION " | AP " + apName + " (pw " + AP_PASS + ") at " + AP_IP.toString());
  Serial.println("ms,level,state,proximity,rssi,csi_pps,csi_all_pps,ping_ok_ps,score");
}

void loop() {
  static uint32_t lastFrame = 0, lastSse = 0, lastCsv = 0, lastRetry = 0, staStart = millis(), lastArp = 0;
  static bool prevMotion = false; static float peak = 0; static uint32_t motionStart = 0;
  uint32_t now = millis();
  if (setupMode) dns.processNextRequest();
  handleButton();
  if (reqRestart && now > restartAt) ESP.restart();

  if (!setupMode) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !staUp) {
      staUp = true; staIp = WiFi.localIP().toString(); lastErr = "";
      IPAddress me = WiFi.localIP();
      if (me[0] == AP_IP[0] && me[1] == AP_IP[1] && me[2] == AP_IP[2]) {   // home LAN uses our AP subnet: move the AP
        AP_IP = AP_IP_ALT; startAp();
        Serial.println("# home LAN overlaps the setup AP subnet -> AP moved to " + AP_IP.toString());
      }
      uint8_t *b = WiFi.BSSID(); for (int i = 0; i < 6; i++) apBssid[i] = b[i]; bssidValid = true;
      if (!mdnsUp && MDNS.begin("esp-radar")) { MDNS.addService("http", "tcp", 80); mdnsUp = true; }
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      startPing(); reqRecal = true;
      Serial.printf("# WiFi up: %s ip=%s gw=%s ch=%d bssid=%s -> http://esp-radar.local\n", staSsid.c_str(), staIp.c_str(),
                    WiFi.gatewayIP().toString().c_str(), (int)WiFi.channel(), WiFi.BSSIDstr().c_str());
    } else if (!up && staUp) {
      staUp = false; bssidValid = false; stopPing(); staIp = ""; lastErr = "lost link"; staStart = now;
      Serial.println("# WiFi lost, reconnecting");
    }
    if (!up && now - staStart > 20000 && now - lastRetry > 15000) {
      lastRetry = now; lastErr = "can't join"; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str());
    }
  }

  State s = snap();
  // top up traffic with ARP probes if ICMP replies are scarce (router rate limiting)
  if (staUp && s.pingRate < 25 && now - lastArp >= 25 && !devScanning) { lastArp = now; arpProbeGateway(); }

  if (!s.calibrating) {
    if (s.motion) peak = max(peak, s.level);
    if (s.motion != prevMotion) {
      if (s.motion) { motionStart = now; logEvent(true, 0, 0); }
      else { logEvent(false, (uint8_t)peak, now - motionStart); peak = 0; }
      prevMotion = s.motion;
    }
  }
  digitalWrite(PIN_LED, s.motion ? ((now / 120) & 1) : LOW);

  secPeak = max(secPeak, s.level);
  if (now - secAt >= 1000) {
    secAt = now;
    if (histN == 128) { memmove(hist, hist + 1, 127); histN = 127; }
    hist[histN++] = (uint8_t)secPeak; secPeak = 0;
  }

  sampleTick(); scanTick(); devTick();

  if (now - lastFrame >= 50) { lastFrame = now; drawOled(); }
  if (now - lastSse >= 125) { lastSse = now; if (events.count()) events.send(stateJson().c_str(), "state", now); }
  if (now - lastCsv >= 100) {
    lastCsv = now;
    if (staUp) Serial.printf("%lu,%.1f,%s,%.1f,%.1f,%.0f,%.0f,%.0f,%.2f\n", (unsigned long)now, s.level,
      s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"), s.prox, s.rssi, s.pps, s.ppsAll, s.pingRate, s.score);
  }
  delay(2);
}
