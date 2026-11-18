/*
  ESP Radar v4 - WiFi CSI motion paths, coverage mapper, network + device scanner,
  device health, home/away, and an off-by-default local smart-light beta.
  Heltec WiFi LoRa 32 V4 (ESP32-S3 + SSD1306/SSD1315 128x64 OLED).

  Motion: Channel State Information along the path from a transmitter to this board.
  It does not see, count, identify, or locate people. v4 can watch the router and a few
  other 2.4 GHz devices; each meter is that device-to-board path, not a position.

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
#include <WiFiClient.h>
#include <WiFiClientSecure.h>
#include "mbedtls/base64.h"
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
#include "audit_defs.h"

#define FW_VERSION "4.1.2"

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
volatile bool reqRecal = false;
volatile int sensitivity = 5;         // 1..10

// ---------- v4 motion paths (router + other 2.4 GHz transmitters) ----------
static volatile uint32_t arpTx = 0;
static esp_ping_handle_t pingH = nullptr;
#include "v4_motion.h"

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
static void arpProbeIp(IPAddress ip) {
  struct netif *nif = staNetif(); if (!nif || ip[3] < 1 || ip[3] > 254) return;
  ip4_addr_t a; IP4_ADDR(&a, ip[0], ip[1], ip[2], ip[3]);
  LOCK_TCPIP_CORE(); etharp_request(nif, &a); UNLOCK_TCPIP_CORE();
}

// ---------- motion events ----------
struct Ev { uint32_t ms; bool start; uint8_t peak; uint32_t durMs; uint8_t link; char name[16]; };
static const int EV_N = 30;
static Ev *evlog = nullptr; static int evCount = 0;
static void logEvent(bool start, uint8_t peak, uint32_t dur, uint8_t link, const char *name) {
  if (!evlog) return;
  if (evCount >= EV_N) { memmove(evlog, evlog + 1, sizeof(Ev) * (EV_N - 1)); evCount = EV_N - 1; }
  Ev e{};
  e.ms = millis(); e.start = start; e.peak = peak; e.durMs = dur; e.link = link;
  if (name) strncpy(e.name, name, sizeof e.name - 1);
  evlog[evCount++] = e;
}

static String jsonEsc(const String &s) {
  String o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c >= 0x20) o += c; } return o;
}
static String macStr(const uint8_t *m) { char b[18]; snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]); return b; }

static void appendState(JBuf &b) {
  State s = snap();
  b.addf("{\"m\":%.1f,\"p\":%.1f,\"s\":\"%s\",\"cal\":%s,\"cp\":%.2f,\"rssi\":%.1f,\"pps\":%.0f,\"ppsAll\":%.0f,\"ping\":%.0f,"
         "\"score\":%.2f,\"sens\":%d,\"ip\":\"%s\",\"ssid\":\"",
         s.level, s.prox, s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"),
         s.calibrating ? "true" : "false", s.calProgress, s.rssi, s.pps, s.ppsAll, s.pingRate, s.score,
         sensitivity, staIp.c_str());
  b.esc(staSsid.c_str());
  b.addf("\",\"ap\":\"%s\",\"apip\":\"%u.%u.%u.%u\",\"ch\":%d,\"up\":%lu,\"setup\":%s,\"sta\":%s,\"now\":%lu,\"v\":\"" FW_VERSION "\"",
         apName.c_str(), AP_IP[0], AP_IP[1], AP_IP[2], AP_IP[3], staUp ? (int)WiFi.channel() : 0,
         (unsigned long)(millis() / 1000), setupMode ? "true" : "false", staUp ? "true" : "false", (unsigned long)millis());
}

static void appendEvents(JBuf &b) {
  b.addf("{\"now\":%lu,\"ev\":[", (unsigned long)millis());
  for (int i = 0; i < evCount && evlog; i++) {
    if (i) b.add(",");
    b.addf("{\"t\":%lu,\"start\":%s,\"peak\":%u,\"dur\":%lu,\"link\":%u,\"name\":\"",
           (unsigned long)evlog[i].ms, evlog[i].start ? "true" : "false", evlog[i].peak,
           (unsigned long)evlog[i].durMs, evlog[i].link);
    b.esc(evlog[i].name);
    b.add("\"}");
  }
  b.add("]}");
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
static char *scanCache = nullptr; static uint32_t scanAt = 0, scanSeq = 0; static bool scanning = false;
static bool scanPausedSniff = false;
static const char *authName(wifi_auth_mode_t a) {
  switch (a) {
    case WIFI_AUTH_OPEN: return "OPEN"; case WIFI_AUTH_WEP: return "WEP"; case WIFI_AUTH_WPA_PSK: return "WPA";
    case WIFI_AUTH_WPA2_PSK: return "WPA2"; case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2"; case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK: return "WPA3"; case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3"; default: return "OTHER";
  }
}
static void scanResumeSniff() {
  if (scanPausedSniff && sniffUser) v4ApplySniffer(true);
  scanPausedSniff = false;
}
static void startScan() {
  if (scanning) return;
  scanning = true;
  heapNote(heapScanMin);
  WiFi.scanDelete();
  if (sniffOn) {
    esp_wifi_set_promiscuous(false);
    sniffOn = false;
    scanPausedSniff = true;
    Serial.println("# wifi scan: promiscuous paused");
  }
  int r = WiFi.scanNetworks(true, true, false, 120);
  if (r != WIFI_SCAN_RUNNING) {
    scanning = false;
    WiFi.scanDelete();
    esp_wifi_scan_stop();
    scanResumeSniff();
    heapNote(heapScanMin);
    Serial.printf("# wifi scan failed to start (%d) heap=%u min=%u\n", r, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
  }
}
static void scanTick() {
  if (!scanning) return;
  heapNote(heapScanMin);
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return;
  scanning = false;
  if (!scanCache) scanCache = (char *)psAlloc(6 * 1024, "scan-json");
  if (n < 0 || !scanCache) {
    if (scanCache) { snprintf(scanCache, 8, "[]"); }
    WiFi.scanDelete();
    esp_wifi_scan_stop();
    scanResumeSniff();
    heapNote(heapScanMin);
    Serial.printf("# wifi scan failed (%d) heap=%u min=%u\n", n, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap());
    return;
  }
  JBuf b;
  b.p = scanCache; b.cap = 6 * 1024; b.begin();
  b.add("[");
  for (int i = 0; i < n && i < 40; i++) {
    if (i) b.add(",");
    String ss = WiFi.SSID(i);
    b.addf("{\"ssid\":\"");
    b.esc(ss.c_str());
    b.addf("\",\"bssid\":\"%s\",\"ch\":%d,\"rssi\":%d,\"auth\":\"%s\",\"open\":%s}",
           WiFi.BSSIDstr(i).c_str(), (int)WiFi.channel(i), (int)WiFi.RSSI(i), authName(WiFi.encryptionType(i)),
           WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false");
  }
  b.add("]");
  scanAt = millis(); scanSeq++;
  WiFi.scanDelete();
  scanResumeSniff();
  heapNote(heapScanMin);
  Serial.printf("# wifi scan %d nets heap=%u\n", n, (unsigned)ESP.getFreeHeap());
}

static void v4AppendDevExtra(JBuf &b, int idx);   // defined in v4_presence.h

// ---------- device discovery (ARP sweep of the home /24, chunked, non-blocking) ----------
struct Dev { uint8_t mac[6]; uint8_t ip4; uint32_t first, last; char name[16]; bool fresh, known; uint8_t auditSev; };
static const int DEV_N = 96;
static Dev *devs = nullptr; static int devCount = 0;
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
        if (!devs || devCount >= DEV_N) continue;
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
  if (!devs) return;
  if (k == devCount && devCount < DEV_N) { memset(&devs[k], 0, sizeof(Dev)); memcpy(devs[k].mac, m, 6); devs[k].first = now; devs[k].known = true; devCount++; }
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
static void appendDevices(JBuf &b) {
  IPAddress base = staUp ? WiFi.localIP() : IPAddress(0, 0, 0, 0);
  IPAddress gw = WiFi.gatewayIP();
  b.addf("{\"now\":%lu,\"scanning\":%s,\"progress\":%d,\"done\":%lu,\"subnet\":\"%u.%u.%u.0/24\",\"known\":%d,\"d\":[",
         (unsigned long)millis(), (devScanning || nbPending) ? "true" : "false",
         devScanning ? (devNext * 100) / 255 : 100, (unsigned long)devScanDone, base[0], base[1], base[2], knownCount);
  for (int k = 0; k < devCount && devs; k++) {
    if (k) b.add(",");
    const char *role = devs[k].ip4 == gw[3] ? "router" : (devs[k].ip4 == base[3] ? "this radar" : "");
    char mac[18];
    snprintf(mac, sizeof mac, "%02X:%02X:%02X:%02X:%02X:%02X", devs[k].mac[0], devs[k].mac[1], devs[k].mac[2], devs[k].mac[3], devs[k].mac[4], devs[k].mac[5]);
    b.addf("{\"ip\":\"%u.%u.%u.%u\",\"mac\":\"%s\",\"vendor\":\"", base[0], base[1], base[2], devs[k].ip4, mac);
    b.esc(vendorOf(devs[k].mac));
    b.add("\",\"name\":\"");
    b.esc(devs[k].name);
    b.addf("\",\"first\":%lu,\"last\":%lu,\"new\":%s,\"audit\":%u,\"role\":\"%s\"",
           (unsigned long)devs[k].first, (unsigned long)devs[k].last, devs[k].fresh ? "true" : "false", devs[k].auditSev, role);
    v4AppendDevExtra(b, k);
    b.add("}");
  }
  b.add("]}");
}

// ======================= v3: per-device security audit (pen-test-lite) =======================
// Scope: ONLY the board's own /24, for networks the user owns. Light checks, not nmap/Metasploit.
// Non-blocking: runs in its own task so motion sensing keeps going (it only shares the radio).

static portMUX_TYPE auditMux = portMUX_INITIALIZER_UNLOCKED;

static ACache acache[16];

static void auditEnqueue(uint8_t ip4) { portENTER_CRITICAL(&auditMux); int nx = (A.qt + 1) % 80; if (nx != A.qh) { A.queue[A.qt] = ip4; A.qt = nx; } portEXIT_CRITICAL(&auditMux); }
static int auditQlen() { portENTER_CRITICAL(&auditMux); int n = (A.qt - A.qh + 80) % 80; portEXIT_CRITICAL(&auditMux); return n; }
static bool auditPop(uint8_t &out) { portENTER_CRITICAL(&auditMux); bool ok = A.qh != A.qt; if (ok) { out = A.queue[A.qh]; A.qh = (A.qh + 1) % 80; } portEXIT_CRITICAL(&auditMux); return ok; }
static void auditClearQueue() { portENTER_CRITICAL(&auditMux); A.qh = A.qt = 0; portEXIT_CRITICAL(&auditMux); }
static void setPhase(const char *p, int done, int total) { portENTER_CRITICAL(&auditMux); strncpy(A.phase, p, sizeof(A.phase) - 1); A.phase[sizeof(A.phase) - 1] = 0; A.done = done; A.total = total; portEXIT_CRITICAL(&auditMux); }

static ACache *cacheFor(const uint8_t *mac) {
  for (auto &c : acache) if (c.used && !memcmp(c.mac, mac, 6)) return &c;
  return nullptr;
}
static void storeReport(const uint8_t *mac, uint8_t sev, const String &json) {
  ACache *slot = cacheFor(mac);
  if (!slot) { for (auto &c : acache) if (!c.used) { slot = &c; break; } }
  if (!slot) { slot = &acache[0]; for (auto &c : acache) if (c.when < slot->when) slot = &c; }   // LRU
  slot->used = true; memcpy(slot->mac, mac, 6); slot->sev = sev; slot->when = millis(); slot->json = json;
}

static String b64(const String &in) {
  size_t ol = 0; uint8_t out[160];
  if (mbedtls_base64_encode(out, sizeof out, &ol, (const uint8_t *)in.c_str(), in.length()) != 0) return "";
  out[ol] = 0; return String((char *)out);
}
static String hval(const String &head, const char *key) {
  String lk = head; lk.toLowerCase(); String k = String("\n") + key + ":"; int i = lk.indexOf(k);
  if (i < 0) return ""; i += k.length(); int e = head.indexOf('\n', i);
  String v = head.substring(i, e < 0 ? head.length() : e); v.trim(); return v;
}
static String printable(const String &in, int maxLen) {
  String o; for (char c : in) { if (o.length() >= maxLen) break; if (c == '\r' || c == '\n') { if (!o.endsWith(" ")) o += ' '; } else if ((uint8_t)c >= 0x20 && (uint8_t)c < 0x7f) o += c; }
  o.trim(); return o;
}

static int probePort(IPAddress ip, uint16_t port, uint32_t to) {   // 2 open, 1 filtered(timeout), 0 closed(refused)
  WiFiClient c; uint32_t t = millis(); bool ok = c.connect(ip, port, to); uint32_t el = millis() - t; c.stop();
  if (ok) return 2; return el >= (uint32_t)(to * 0.8f) ? 1 : 0;
}
static String grabBanner(IPAddress ip, uint16_t port) {
  WiFiClient c; if (!c.connect(ip, port, 1200)) return ""; String b; uint32_t t = millis();
  while (millis() - t < 900 && b.length() < 200) { while (c.available() && b.length() < 200) b += (char)c.read(); delay(10); }
  c.stop(); return printable(b, 160);
}
// GET / and capture status/Server/WWW-Authenticate/<title>. Returns true if we got an HTTP response.
static bool httpGet(IPAddress ip, uint16_t port, bool secure, int &status, String &server, String &wwwauth, String &title,
                    bool withAuth, const char *user, const char *pass) {
  status = -1; WiFiClient plain; WiFiClientSecure sec;
  WiFiClient *c;
  if (secure) { if (ESP.getFreeHeap() < 60000) return false; sec.setInsecure(); sec.setTimeout(6); c = &sec; }
  else { plain.setTimeout(3); c = &plain; }
  if (!c->connect(ip, port, secure ? 6000 : 2500)) return false;
  String req = "GET / HTTP/1.1\r\nHost: " + ip.toString() + "\r\nUser-Agent: ESP-Radar-Audit\r\nAccept: */*\r\nConnection: close\r\n";
  if (withAuth) req += "Authorization: Basic " + b64(String(user) + ":" + pass) + "\r\n";
  req += "\r\n"; c->print(req);
  uint32_t t0 = millis(); String head, body; bool inBody = false; int n = 0;
  while (c->connected() && millis() - t0 < 2500 && n < 8000) {
    while (c->available() && n < 8000) { char ch = c->read(); n++; if (!inBody) { head += ch; if (head.endsWith("\r\n\r\n")) inBody = true; } else if (body.length() < 4000) body += ch; }
    delay(4);
  }
  c->stop();
  if (head.length() < 12) return false;
  int sp = head.indexOf(' '); if (sp > 0) status = head.substring(sp + 1, sp + 4).toInt();
  server = printable(hval(head, "server"), 80);
  wwwauth = printable(hval(head, "www-authenticate"), 80);
  String lb = body; lb.toLowerCase(); int ti = lb.indexOf("<title"); 
  if (ti >= 0) { int gt = lb.indexOf('>', ti); int te = lb.indexOf("</title", gt); if (gt > 0 && te > gt) title = printable(body.substring(gt + 1, te), 60); }
  return true;
}
// Capped default-credential check: only when a Basic realm was seen. One pass, stop on first success.
static int tryDefaults(IPAddress ip, uint16_t port, bool secure, const uint8_t *mac, int &nTried) {
  nTried = 0;
  const char *vu = nullptr, *vp = nullptr;                 // optional vendor default (keeps total <= 8)
  String vend = vendorOf(mac);
  if (vend.indexOf("Ubiquiti") >= 0) { vu = "ubnt"; vp = "ubnt"; }
  int total = DEF_CREDS_N + (vu ? 1 : 0);
  for (int i = 0; i < total; i++) {
    if (A.cancel) return -1;
    const char *u = i < DEF_CREDS_N ? DEF_CREDS[i].u : vu;
    const char *p = i < DEF_CREDS_N ? DEF_CREDS[i].p : vp;
    char ph[40]; snprintf(ph, sizeof ph, "default login %d/%d", i + 1, total); setPhase(ph, i, total);
    int st; String sv, wa, ti;
    nTried++;
    if (httpGet(ip, port, secure, st, sv, wa, ti, true, u, p)) {
      if (st > 0 && st != 401 && st != 403) return i;       // accepted
    }
    delay(400);                                             // gentle: never a tight loop
  }
  return -1;
}

static void addF(String &fj, int &maxSev, int sev, const char *title, const String &detail, const char *tip) {
  if (sev > maxSev) maxSev = sev;
  if (fj.length() > 1) fj += ",";
  fj += "{\"sev\":" + String(sev) + ",\"title\":\"" + jsonEsc(title) + "\",\"detail\":\"" + jsonEsc(detail) + "\",\"tip\":\"" + jsonEsc(tip) + "\"}";
}
static void verHints(const String &text, String &fj, int &maxSev, const char *where) {
  if (text.length() < 2) return;
  for (int i = 0; i < VER_HINTS_N; i++) if (text.indexOf(VER_HINTS[i].needle) >= 0)
    addF(fj, maxSev, VER_HINTS[i].sev, "Dated software", String(where) + ": " + text + " - " + VER_HINTS[i].note,
         "Check the vendor for a firmware update; replace the device if it is end-of-life.");
}

static void auditOne(uint8_t ip4) {
  IPAddress me = WiFi.localIP();
  if (ip4 < 1 || ip4 > 254) return;
  IPAddress target(me[0], me[1], me[2], ip4);
  // find the device record / MAC
  int di = -1; for (int k = 0; k < devCount; k++) if (devs[k].ip4 == ip4) { di = k; break; }
  uint8_t mac[6] = {0}; if (di >= 0) memcpy(mac, devs[di].mac, 6); else { mac[5] = ip4; mac[0] = 0x02; }
  bool isSelf = ip4 == me[3], isRouter = ip4 == WiFi.gatewayIP()[3];
  A.curIp = ip4;
  String portsJson = "[", findJson = "["; int maxSev = 0, openCount = 0;
  bool httpOpen = false; uint16_t firstWebPort = 0; bool firstWebSecure = false; String firstWebAuth = "";

  for (int i = 0; i < AUDIT_NPORTS && !A.cancel; i++) {
    char ph[48]; snprintf(ph, sizeof ph, "scanning :%u (%s)", AUDIT_PORTS[i].port, AUDIT_PORTS[i].svc);
    setPhase(ph, i, AUDIT_NPORTS);
    int r = probePort(target, AUDIT_PORTS[i].port, 600);
    if (r == 2) {
      openCount++;
      if (portsJson.length() > 1) portsJson += ",";
      portsJson += "{\"port\":" + String(AUDIT_PORTS[i].port) + ",\"svc\":\"" + AUDIT_PORTS[i].svc + "\",\"state\":\"open\"}";
      uint8_t kind = AUDIT_PORTS[i].kind; uint16_t port = AUDIT_PORTS[i].port;
      String banner = "";
      if (kind == 3 || kind == 10 || kind == 6) banner = grabBanner(target, port);
      if (kind == 6) addF(findJson, maxSev, 3, "Telnet is open", "Port " + String(port) + " (Telnet) accepts connections. Telnet sends everything, including passwords, in clear text and is a classic IoT attack vector." + (banner.length() ? " Banner: " + banner : ""),
                          "Disable Telnet in the device settings; use SSH or the web UI instead.");
      else if (kind == 10) addF(findJson, maxSev, 2, "FTP is open", "Port 21 (FTP) is open - FTP is unencrypted." + (banner.length() ? " Banner: " + banner : ""),
                                "Turn off FTP if unused, or switch to SFTP/FTPS. Make sure anonymous access is disabled.");
      else if (kind == 5 || kind == 11) addF(findJson, maxSev, 2, "Windows file sharing exposed", "Port " + String(port) + " (SMB/RPC) is reachable. Exposed SMB has been the vector for worms like WannaCry.",
                                             "Limit file sharing to trusted networks and keep the OS patched.");
      else if (kind == 4) addF(findJson, maxSev, 2, "RTSP camera stream port open", "Port " + String(port) + " (RTSP) is open - this is often an IP camera video stream that may be viewable without a password.",
                               "Set a strong camera password and disable RTSP if you don't use it.");
      else if (kind == 7) addF(findJson, maxSev, 3, "Remote Desktop exposed", "Port 3389 (RDP) is open. RDP exposed on the LAN is risky and a common ransomware entry point.",
                               "Disable RDP if unused; require a strong password and network-level authentication.");
      else if (kind == 8) addF(findJson, maxSev, 2, "TR-069 remote management open", "Port 7547 (TR-069/CWMP) is open. This ISP remote-management service has had serious past vulnerabilities.",
                               "If your ISP doesn't require it, disable remote management on the router.");
      else if (kind == 9) addF(findJson, maxSev, 2, "MQTT broker open", "Port 1883 (MQTT) is open and is often left with no authentication, exposing smart-home messages.",
                               "Enable authentication/TLS on the broker, or firewall it to trusted devices.");
      else if (port == 32764) addF(findJson, maxSev, 3, "Possible router backdoor port", "Port 32764 is open - historically a backdoor on some consumer routers.",
                                   "Update the router firmware; replace it if the vendor no longer patches it.");
      if (kind == 1 || kind == 2) {
        bool secure = kind == 2;
        setPhase("reading web service", i, AUDIT_NPORTS);
        int st; String sv, wa, ti;
        if (httpGet(target, port, secure, st, sv, wa, ti, false, "", "")) {
          httpOpen = true; if (!firstWebPort && wa.indexOf("Basic") >= 0) { firstWebPort = port; firstWebSecure = secure; firstWebAuth = wa; }
          String d = String(secure ? "HTTPS" : "HTTP") + " on " + String(port) + ": status " + String(st) + (sv.length() ? ", Server: " + sv : "") + (ti.length() ? ", title: " + ti : "");
          if (secure) d += " (certificate not verified)";
          int sev = 0; const char *tip = "";
          bool loginish = wa.indexOf("Basic") >= 0 || ti.indexOf("login") >= 0 || ti.indexOf("Login") >= 0 || ti.indexOf("outer") >= 0 || ti.indexOf("dmin") >= 0;
          if (!secure && loginish) { sev = 2; tip = "Use HTTPS for the admin page if the device supports it, and set a strong password."; }
          else if (!secure && port == 80) { sev = 1; tip = "Plain HTTP admin interface - prefer HTTPS and a strong password."; }
          else if (secure) { sev = 1; tip = "Certificate isn't validated by this light audit; a browser warning usually means a self-signed cert."; }
          addF(findJson, maxSev, sev, secure ? "HTTPS service" : (loginish ? "Web login over plain HTTP" : "Web service"), d, tip[0] ? tip : "Informational.");
          verHints(sv, findJson, maxSev, "Server header");
        } else {
          String d = String(secure ? "HTTPS" : "HTTP") + " port " + String(port) + " open" + (secure ? " (TLS not inspected)" : " (no response captured)");
          addF(findJson, maxSev, 0, "Web port open", d, "Informational.");
        }
      }
      if (kind == 3 && banner.length()) verHints(banner, findJson, maxSev, String(AUDIT_PORTS[i].svc).c_str());
    } else {
      if (portsJson.length() > 1) portsJson += ",";
      portsJson += "{\"port\":" + String(AUDIT_PORTS[i].port) + ",\"svc\":\"" + AUDIT_PORTS[i].svc + "\",\"state\":\"" + (r == 1 ? "filtered" : "closed") + "\"}";
    }
  }
  portsJson += "]";

  // default-credential check (capped, opt-outable), only against a Basic-auth web port
  int defResult = -2;       // -2 skipped, -1 none, >=0 index found
  int defTried = 0;
  if (!A.cancel && A.def && firstWebPort) {
    defResult = tryDefaults(target, firstWebPort, firstWebSecure, mac, defTried);
    if (defResult >= 0) {
      const char *u = defResult < DEF_CREDS_N ? DEF_CREDS[defResult].u : "ubnt";
      addF(findJson, maxSev, 4, "Default login accepted", String("A factory-default username/password worked on the web login (port ") + firstWebPort + "). User \"" + u + "\" with a well-known default password (pair " + String(defResult + 1) + ").",
           "Change this device's admin password immediately to a unique strong one.");
    }
  }
  findJson += "]";

  if (!A.cancel) {
    if (openCount == 0) addF(findJson = "[", maxSev, 0, "No common ports open", "None of the " + String(AUDIT_NPORTS) + " audited ports responded. The device may be firewalled, asleep, or only use ports outside this list.", "Nothing to do."), findJson += "]";
    const char *names[] = {"OK", "Low", "Medium", "High", "Critical"};
    String rep = "{\"ip\":\"" + target.toString() + "\",\"mac\":\"" + macStr(mac) + "\",\"vendor\":\"" + vendorOf(mac) + "\",\"sev\":" + String(maxSev) +
                 ",\"sevName\":\"" + names[maxSev] + "\",\"when\":" + String(millis()) + ",\"role\":\"" + String(isRouter ? "router" : (isSelf ? "this radar" : "")) +
                 "\",\"open\":" + String(openCount) + ",\"def\":" + (A.def ? "true" : "false") + ",\"defTried\":" + String(defTried) +
                 ",\"defResult\":\"" + (defResult == -2 ? "skipped" : (defResult == -1 ? "none" : "found")) + "\",\"ports\":" + portsJson + ",\"findings\":" + findJson + "}";
    storeReport(mac, (uint8_t)maxSev, rep);
    if (di >= 0) devs[di].auditSev = (uint8_t)(maxSev + 1);    // 0 = unscanned; 1..5 = OK..Critical
    A.scanned++;
    Serial.printf("# audit %s sev=%s open=%d def=%s\n", target.toString().c_str(), names[maxSev], openCount, defResult >= 0 ? "FOUND" : (defResult == -1 ? "none" : "skipped"));
  }
}

static void auditTask(void *) {
  for (;;) {
    uint8_t ip4;
    if (!A.running && auditPop(ip4)) {
      A.running = true; A.cancel = false;
      if (staUp) auditOne(ip4);
      A.running = false; A.curIp = 0;
      if (auditQlen() == 0) setPhase("", 0, 0);
    } else delay(120);
  }
}

static String auditStatusJson(int wantIp) {
  String o = "{\"running\":" + String(A.running ? "true" : "false") + ",\"cur\":" + String(A.curIp) +
             ",\"done\":" + String(A.done) + ",\"total\":" + String(A.total) + ",\"qlen\":" + String(auditQlen()) +
             ",\"scanned\":" + String(A.scanned) + ",\"def\":" + String(A.def ? "true" : "false") + ",\"phase\":\"" + jsonEsc(A.phase) + "\"";
  if (wantIp > 0) {
    IPAddress me = WiFi.localIP(); String rep = "null";
    for (int k = 0; k < devCount; k++) if (devs[k].ip4 == wantIp) { ACache *c = cacheFor(devs[k].mac); if (c) rep = c->json; break; }
    o += ",\"report\":" + rep;
  }
  return o + "}";
}
static String auditSummaryJson() {
  int cnt[5] = {0, 0, 0, 0, 0}, tot = 0;
  for (auto &c : acache) if (c.used) { cnt[c.sev]++; tot++; }
  return "{\"total\":" + String(tot) + ",\"ok\":" + String(cnt[0]) + ",\"low\":" + String(cnt[1]) + ",\"medium\":" + String(cnt[2]) +
         ",\"high\":" + String(cnt[3]) + ",\"critical\":" + String(cnt[4]) + ",\"running\":" + String(A.running ? "true" : "false") +
         ",\"qlen\":" + String(auditQlen()) + "}";
}
// validate "a.b.c.d" is on our own /24
static bool sameSubnet(const String &ipStr, uint8_t &last) {
  IPAddress ip; if (!ip.fromString(ipStr)) return false; IPAddress me = WiFi.localIP();
  if (!staUp || ip[0] != me[0] || ip[1] != me[1] || ip[2] != me[2]) return false;
  if (ip[3] < 1 || ip[3] > 254) return false; last = ip[3]; return true;
}
// ======================= end v3 audit =======================

static bool safeMode = false;
static esp_reset_reason_t bootReason = ESP_RST_UNKNOWN;
static bool crashReset(esp_reset_reason_t r) {
  return r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT || r == ESP_RST_BROWNOUT;
}
static const char *rstName(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

#include "v4_presence.h"
#include "v4_lights.h"

static void appendFull(JBuf &b) {
  b.begin();
  appendState(b);
  if (b.n && b.p[b.n - 1] == '}') b.n--;
  v4AppendMotion(b);
  v4AppendPresence(b);
  v4AppendBeta(b);
  const char *why = (bootReason == ESP_RST_SW && rstMagic == RST_MAGIC) ? rstWhy : "";
  b.addf(",\"rst\":\"%s\",\"why\":\"", rstName(bootReason));
  b.esc(why);
  b.addf("\",\"heap\":%u,\"minHeap\":%u,\"ps\":%u,\"psFree\":%u,\"scanMin\":%u,\"pageMin\":%u,\"safe\":%s}",
         (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(),
         (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram(),
         (unsigned)heapScanMin, (unsigned)heapPageMin, safeMode ? "true" : "false");
}

// ---------- web server ----------
static void sendGz(AsyncWebServerRequest *r, const uint8_t *d, size_t n, const char *type) {
  AsyncWebServerResponse *res = r->beginResponse(200, type, d, n);
  res->addHeader("Content-Encoding", "gzip");
  res->addHeader("Cache-Control", "no-cache");
  r->send(res);
}
static void sendBuf(AsyncWebServerRequest *r) {
  heapNote(heapPageMin);
  r->send(200, "application/json", JB.c_str());
  heapNote(heapPageMin);
}
static bool beginJson() {
  if (!jbTake()) return false;
  JB.begin();
  return true;
}
static void sendJson(AsyncWebServerRequest *r, const String &s) {
  heapNote(heapPageMin);
  r->send(200, "application/json", s);
  heapNote(heapPageMin);
}

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (setupMode) { r->redirect("/setup"); return; }
    sendGz(r, INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html");
  });
  server.on("/app", HTTP_GET, [](AsyncWebServerRequest *r) { sendGz(r, INDEX_HTML_GZ, INDEX_HTML_GZ_LEN, "text/html"); });
  server.on("/radar", HTTP_GET, [](AsyncWebServerRequest *r) { r->redirect("/app"); });
  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *r) { sendGz(r, SETUP_HTML_GZ, SETUP_HTML_GZ_LEN, "text/html"); });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    appendFull(JB); sendBuf(r); jbGive();
  });
  server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    appendEvents(JB); sendBuf(r); jbGive();
  });
  server.on("/api/activity", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    v4AppendActivity(JB); sendBuf(r); jbGive();
  });
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    bool force = r->hasParam("force");
    if (force || scanAt == 0 || millis() - scanAt > 20000) startScan();
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    JB.addf("{\"scanning\":%s,\"seq\":%lu,\"age\":%ld,\"bssid\":\"%s\",\"nets\":%s}",
            scanning ? "true" : "false", (unsigned long)scanSeq, scanAt ? (long)((millis() - scanAt) / 1000) : -1L,
            staUp ? WiFi.BSSIDstr().c_str() : "", scanCache ? scanCache : "[]");
    sendBuf(r); jbGive();
  });
  server.on("/api/sample", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    smp.want = r->hasParam("n", true) ? constrain(r->getParam("n", true)->value().toInt(), 3, 30) : 10;
    smp.n = 0; smp.busy = true; smp.nextAt = 0; smp.id++;
    sendJson(r, sampleJson());
  });
  server.on("/api/sample", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, sampleJson()); });
  server.on("/api/devices", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    appendDevices(JB); sendBuf(r); jbGive();
  });
  server.on("/api/audit/status", HTTP_GET, [](AsyncWebServerRequest *r) {
    int ip = r->hasParam("ip") ? r->getParam("ip")->value().toInt() : 0;
    sendJson(r, auditStatusJson(ip));
  });
  server.on("/api/audit/summary", HTTP_GET, [](AsyncWebServerRequest *r) { sendJson(r, auditSummaryJson()); });
  server.on("/api/audit", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    if (r->hasParam("def", true)) A.def = r->getParam("def", true)->value() == "1";
    uint8_t last;
    if (!r->hasParam("ip", true) || !sameSubnet(r->getParam("ip", true)->value(), last)) { r->send(400, "application/json", "{\"error\":\"IP must be on this board's own /24\"}"); return; }
    A.cancel = false; auditEnqueue(last);
    sendJson(r, "{\"queued\":1,\"ip\":" + String(last) + "}");
  });
  server.on("/api/audit/all", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    if (r->hasParam("def", true)) A.def = r->getParam("def", true)->value() == "1";
    A.cancel = false; int n = 0; IPAddress me = WiFi.localIP();
    for (int k = 0; k < devCount; k++) if (devs[k].ip4 != me[3]) { auditEnqueue(devs[k].ip4); n++; }
    sendJson(r, "{\"queued\":" + String(n) + "}");
  });
  server.on("/api/audit/cancel", HTTP_POST, [](AsyncWebServerRequest *r) { A.cancel = true; auditClearQueue(); sendJson(r, "{\"ok\":true}"); });
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
    armRestart("wifi credentials saved");
  });
  server.on("/api/recal", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (r->hasParam("link", true)) v4RequestRecal(r->getParam("link", true)->value().toInt());
    else reqRecal = true;
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/links", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    v4AppendTalkers(JB); sendBuf(r); jbGive();
  });
  server.on("/api/links/auto", HTTP_POST, [](AsyncWebServerRequest *r) {
    sendJson(r, "{\"ok\":true,\"added\":" + String(v4AutoPick()) + "}");
  });
  server.on("/api/links/clear", HTTP_POST, [](AsyncWebServerRequest *r) {
    v4LinkClear(r->hasParam("slot", true) ? r->getParam("slot", true)->value().toInt() : -1);
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/links/set", HTTP_POST, [](AsyncWebServerRequest *r) {
    int slot = r->hasParam("slot", true) ? r->getParam("slot", true)->value().toInt() : -1;
    String macs = r->hasParam("mac", true) ? r->getParam("mac", true)->value() : "";
    String name = r->hasParam("name", true) ? r->getParam("name", true)->value() : "";
    String room = r->hasParam("room", true) ? r->getParam("room", true)->value() : "";
    if (slot == 0) { v4LinkSet(0, nullptr, name.c_str(), room.c_str(), true); sendJson(r, "{\"ok\":true}"); return; }
    uint8_t mac[6];
    if (!parseMacStr(macs.c_str(), mac) || !v4LinkSet(slot, mac, name.c_str(), room.c_str(), true)) {
      r->send(400, "application/json", "{\"error\":\"bad link\"}"); return;
    }
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/sniff", HTTP_POST, [](AsyncWebServerRequest *r) {
    bool on = !r->hasParam("v", true) || r->getParam("v", true)->value() != "0";
    v4SniffUser(on);
    sendJson(r, "{\"sniff\":" + String(on ? "true" : "false") + "}");
  });
  server.on("/api/presence", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    v4AppendPresenceJson(JB); sendBuf(r); jbGive();
  });
  server.on("/api/device", HTTP_POST, [](AsyncWebServerRequest *r) {
    String macs = r->hasParam("mac", true) ? r->getParam("mac", true)->value() : "";
    uint8_t mac[6];
    if (!parseMacStr(macs.c_str(), mac)) { r->send(400, "application/json", "{\"error\":\"bad mac\"}"); return; }
    if (r->hasParam("alias", true)) v4SetAlias(mac, r->getParam("alias", true)->value().c_str());
    if (r->hasParam("person", true) && !v4SetPerson(mac, r->getParam("person", true)->value() != "0")) {
      r->send(400, "application/json", "{\"error\":\"up to 8 people\"}"); return;
    }
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/clock", HTTP_POST, [](AsyncWebServerRequest *r) {
    int tz = r->hasParam("tz", true) ? r->getParam("tz", true)->value().toInt() : tzEast;
    bool setN = r->hasParam("night0", true) && r->hasParam("night1", true);
    v4SetClock(tz, setN ? r->getParam("night0", true)->value().toInt() : 19, setN ? r->getParam("night1", true)->value().toInt() : 7, setN);
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/lights", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    v4AppendLights(JB); sendBuf(r); jbGive();
  });
  server.on("/api/lights/ble", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!betaOn) { r->send(409, "application/json", "{\"error\":\"turn Beta on first\"}"); return; }
    v4GoveeBleRequest();
    sendJson(r, "{\"ok\":true,\"scanning\":true}");
  });
  server.on("/api/lights/scan", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!staUp) { r->send(409, "application/json", "{\"error\":\"not connected to home WiFi\"}"); return; }
    bool whole = r->hasParam("whole", true) && r->getParam("whole", true)->value() == "1";
    v4LightScan(whole);
    if (!beginJson()) { r->send(503, "application/json", "{\"error\":\"busy\"}"); return; }
    v4AppendLights(JB); sendBuf(r); jbGive();
  });
  server.on("/api/lights/ip", HTTP_POST, [](AsyncWebServerRequest *r) {
    IPAddress ip;
    String s = r->hasParam("ip", true) ? r->getParam("ip", true)->value() : "";
    if (!ip.fromString(s)) { r->send(400, "application/json", "{\"error\":\"bad ip\"}"); return; }
    v4LightManual(ip);
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/lights/tuya", HTTP_POST, [](AsyncWebServerRequest *r) {
    String dev = r->hasParam("dev", true) ? r->getParam("dev", true)->value() : "";
    String ip = r->hasParam("ip", true) ? r->getParam("ip", true)->value() : "";
    String key = r->hasParam("key", true) ? r->getParam("key", true)->value() : "";
    int ver = r->hasParam("ver", true) ? r->getParam("ver", true)->value().toInt() : 33;
    bool clear = r->hasParam("clear", true) && r->getParam("clear", true)->value() == "1";
    if (!v4TuyaKeySet(dev.c_str(), ip.c_str(), key.c_str(), ver, clear)) {
      r->send(400, "application/json", "{\"error\":\"need a LAN ip\"}"); return;
    }
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/lights/beta", HTTP_POST, [](AsyncWebServerRequest *r) {
    bool on = r->hasParam("v", true) && r->getParam("v", true)->value() == "1";
    v4SetBeta(on);
    sendJson(r, "{\"beta\":" + String(on ? "true" : "false") + "}");
  });
  server.on("/api/lights/test", HTTP_POST, [](AsyncWebServerRequest *r) {
    String id = r->hasParam("id", true) ? r->getParam("id", true)->value() : "";
    String op = r->hasParam("op", true) ? r->getParam("op", true)->value() : "on";
    int bri = r->hasParam("bri", true) ? r->getParam("bri", true)->value().toInt() : 80;
    if (!v4LightTest(id.c_str(), op.c_str(), bri)) { r->send(409, "application/json", "{\"error\":\"turn Beta on first, or unknown light\"}"); return; }
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/lights/rule", HTTP_POST, [](AsyncWebServerRequest *r) {
    auto pv = [&](const char *k, const char *d) { return r->hasParam(k, true) ? r->getParam(k, true)->value() : String(d); };
    v4RuleSet(pv("i", "0").toInt(), pv("used", "1").toInt(), pv("link", "-1").toInt(), pv("room", "").c_str(), pv("ids", "").c_str(),
              pv("timeout", "5").toInt(), pv("night", "0").toInt(), pv("home", "0").toInt(), pv("bri", "80").toInt());
    sendJson(r, "{\"ok\":true}");
  });
  server.on("/api/sens", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (r->hasParam("v", true)) { sensitivity = constrain(r->getParam("v", true)->value().toInt(), 1, 10); prefs.putInt("sens", sensitivity); }
    sendJson(r, "{\"sens\":" + String(sensitivity) + "}");
  });
  server.on("/api/forget", HTTP_POST, [](AsyncWebServerRequest *r) {
    prefs.remove("ssid"); prefs.remove("pass");
    sendJson(r, "{\"ok\":true}");
    armRestart("wifi forgotten");
  });
  server.onNotFound([](AsyncWebServerRequest *r) {
    if (setupMode) r->redirect("http://" + AP_IP.toString() + "/setup"); else r->redirect("/");
  });
  events.onConnect([](AsyncEventSourceClient *c) {
    if (events.count() > 2) return;
    if (!jbTake()) return;
    appendFull(JB);
    c->send(JB.c_str(), "state", millis(), 1000);
    jbGive();
  });
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
  display.drawString(128, 0, s.calibrating ? String("CAL") : String(v4HomeLabel()));
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
    LinkView loud; v4Loud(loud);
    bool any = v4AnyMotion();
    display.setFont(ArialMT_Plain_16);
    display.drawString(0, 13, s.pps < 5 && !any ? "NO CSI" : (any ? "MOTION" : "STILL"));
    display.setFont(ArialMT_Plain_10);
    display.setTextAlignment(TEXT_ALIGN_RIGHT);
    display.drawString(128, 17, String((int)(loud.active ? loud.level : s.level)) + "% " + String(loud.name).substring(0, 8));
    display.setTextAlignment(TEXT_ALIGN_LEFT);
    // motion bar with threshold tick at 25%
    display.drawRect(0, 32, 128, 8);
    display.fillRect(1, 33, (int)(126 * (loud.active ? loud.level : s.level) / 100), 6);
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
    armRestart("PRG long-press erased WiFi");
  }
  if (!pressed && btnDown) {
    if (!btnLong && now - btnDown > 40) { reqRecal = true; Serial.println("# PRG: recalibrating"); }
    btnDown = 0;
  }
}

void setup() {
  bootReason = esp_reset_reason();
  Serial.begin(115200);
  Serial.printf("# reset %s\n", rstName(bootReason));
  if (bootReason == ESP_RST_SW && rstMagic == RST_MAGIC) Serial.printf("# restart reason: %s\n", rstWhy);
  rstMagic = 0;
  jbInit();
  v4MotionMem();
  evlog = (Ev *)psAlloc(sizeof(Ev) * EV_N, "events");
  devs = (Dev *)psAlloc(sizeof(Dev) * DEV_N, "devices");
  v4PresenceMem();
  pinMode(PIN_PRG, INPUT_PULLUP);
  pinMode(PIN_LED, OUTPUT); digitalWrite(PIN_LED, LOW);
  oledInit();
  display.setFont(ArialMT_Plain_16); display.drawString(10, 10, "ESP RADAR");
  display.setFont(ArialMT_Plain_10); display.drawString(10, 34, "paths - health - home"); display.drawString(10, 48, "v" FW_VERSION); display.display();

  prefs.begin("espradar", false);
  staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("pass", "");
  sensitivity = prefs.getInt("sens", 5);
  setupMode = staSsid.length() == 0;
  loadKnown();
  v4LoadLinks();
  v4PresenceLoad();
  ltLoad();
  v4LightsStart();

  uint8_t mac[6]; WiFi.macAddress(mac);
  v4SetSelf(mac);
  char nm[24]; snprintf(nm, sizeof nm, "ESP-Radar-%02X%02X", mac[4], mac[5]); apName = nm;

  v4Start();
  xTaskCreatePinnedToCore(auditTask, "audit", 12288, nullptr, 1, nullptr, 1);

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
  heapBoot = ESP.getFreeHeap();
  Serial.printf("# mem boot internal=%u min=%u psram=%u freePsram=%u\n",
                (unsigned)heapBoot, (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getPsramSize(), (unsigned)ESP.getFreePsram());
  Serial.println("# ESP Radar " FW_VERSION " | AP " + apName + " (pw " + AP_PASS + ") at " + AP_IP.toString());
  Serial.println("ms,level,state,proximity,rssi,csi_pps,csi_all_pps,ping_ok_ps,score,home,paths");
}

void loop() {
  static uint32_t lastFrame = 0, lastSse = 0, lastCsv = 0, lastRetry = 0, staStart = millis(), lastArp = 0, lastProbe = 0, lastDecay = 0;
  static bool prevLink[V4_LINKS]; static float peakL[V4_LINKS]; static uint32_t motAt[V4_LINKS];
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
      uint8_t self[6]; WiFi.macAddress(self); v4SetSelf(self);
      v4SetBssid(WiFi.BSSID());
      v4ApplySniffer(true);
      if (!mdnsUp && MDNS.begin("esp-radar")) { MDNS.addService("http", "tcp", 80); mdnsUp = true; }
      configTime(0, 0, "pool.ntp.org", "time.google.com");
      startPing(); reqRecal = true;
      Serial.printf("# WiFi up: %s ip=%s gw=%s ch=%d bssid=%s -> http://esp-radar.local\n", staSsid.c_str(), staIp.c_str(),
                    WiFi.gatewayIP().toString().c_str(), (int)WiFi.channel(), WiFi.BSSIDstr().c_str());
    } else if (!up && staUp) {
      staUp = false; v4ClearBssid(); stopPing(); staIp = ""; lastErr = "lost link"; staStart = now;
      Serial.println("# WiFi lost, reconnecting");
    }
    if (!up && now - staStart > 20000 && now - lastRetry > 15000) {
      lastRetry = now; lastErr = "can't join"; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str());
    }
  }

  State s = snap();
  // top up traffic with ARP probes if ICMP replies are scarce (router rate limiting)
  if (staUp && s.pingRate < 25 && now - lastArp >= 25 && !devScanning) { lastArp = now; arpProbeGateway(); }
  if (staUp && !devScanning && now - lastProbe >= 160) {
    lastProbe = now;
    uint8_t mac[6]; IPAddress ip;
    if (v4NextProbe(mac) && ipForMac(mac, ip)) arpProbeIp(ip);
  }

  for (int i = 0; i < V4_LINKS; i++) {
    LinkView v; v4View(i, v);
    if (!v.active || v.calibrating || !strcmp(v.st, "wait") || !strcmp(v.st, "low")) { prevLink[i] = false; continue; }
    if (v.motion) peakL[i] = max(peakL[i], v.level);
    if (v.motion != prevLink[i]) {
      if (v.motion) { motAt[i] = now; logEvent(true, 0, 0, (uint8_t)i, v.name); }
      else { logEvent(false, (uint8_t)peakL[i], now - motAt[i], (uint8_t)i, v.name); peakL[i] = 0; }
      prevLink[i] = v.motion;
    }
  }
  digitalWrite(PIN_LED, v4AnyMotion() ? ((now / 120) & 1) : LOW);

  LinkView loud; v4Loud(loud);
  secPeak = max(secPeak, (loud.active && !loud.calibrating) ? loud.level : s.level);
  if (now - secAt >= 1000) {
    secAt = now;
    if (histN == 128) { memmove(hist, hist + 1, 127); histN = 127; }
    hist[histN++] = (uint8_t)secPeak; secPeak = 0;
  }

  sampleTick(); scanTick(); devTick();
  v4PresenceTick();
  if (now - lastDecay >= 20000) { lastDecay = now; v4DecayTalkers(); }
  static uint32_t lastHealth = 0;
  if (now - lastHealth >= 10000) {
    lastHealth = now;
    Serial.printf("# up=%lu rst=%s heap=%u min=%u psram=%u scanMin=%u pageMin=%u safe=%d csi=%.0f\n",
                  (unsigned long)(now / 1000), rstName(bootReason), (unsigned)ESP.getFreeHeap(),
                  (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getFreePsram(),
                  (unsigned)heapScanMin, (unsigned)heapPageMin, safeMode ? 1 : 0, s.pps);
  }

  if (now - lastFrame >= 50) { lastFrame = now; drawOled(); }
  if (now - lastSse >= 250) {
    lastSse = now;
    if (events.count() && jbTake()) {
      appendFull(JB);
      events.send(JB.c_str(), "state", now);
      jbGive();
      heapNote(heapPageMin);
    }
  }
  if (now - lastCsv >= 100) {
    lastCsv = now;
    if (staUp) Serial.printf("%lu,%.1f,%s,%.1f,%.1f,%.0f,%.0f,%.0f,%.2f,%s,%d\n", (unsigned long)now, s.level,
      s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"), s.prox, s.rssi, s.pps, s.ppsAll, s.pingRate, s.score,
      v4HomeLabel(), v4MotionCount());
  }
  delay(2);
}
