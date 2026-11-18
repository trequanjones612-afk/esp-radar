// ESP Radar v4 — beta local smart lights. Off unless the user turns Beta on.
//
// Govee LAN API (UDP, no account, no API key): the user must enable "LAN Control"
// on each bulb in the Govee Home app. Discovery is multicast 239.255.255.250:4001
// plus a broadcast, replies on UDP 4002, commands to the bulb's :4003.
// WiZ (UDP 38899 JSON) and LIFX (UDP 56700 binary, on/off only) use the same rule
// engine so another local-API brand can be added the same way. No cloud calls.
//
// eero and other meshes sometimes drop client-to-client multicast. Untested on hardware.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <Preferences.h>
#include <string.h>
#include "v4_tuya.h"
#include "v4_govee_ble.h"

static const int LT_MAX = 40;
static const int RULE_MAX = 4;
static const int TUYA_KEYS = 24;
static const uint8_t BR_GOVEE = 1, BR_WIZ = 2, BR_LIFX = 3, BR_TUYA = 4, BR_GOVEE_BLE = 5;

struct Light {
  uint8_t brand;
  uint8_t ip[4];
  char id[24];
  char name[20];
  char model[12];
  uint8_t on;
  uint8_t bri;
  uint8_t flags;        // bit0: tuya, waiting for a local key
  int8_t rssi;          // last BLE advertisement, 0 if unknown
  uint32_t seen;
};

struct __attribute__((packed)) LightSave {
  uint8_t brand;
  uint8_t ip[4];
  uint8_t flags;
  char id[24];
  char name[20];
  char model[12];
};

struct __attribute__((packed)) TuyaSec {
  char id[24];
  char key[17];
  uint8_t ver;          // 33, 34 or 35. Never copied into JSON or logs.
};

struct Rule {
  uint8_t used;
  int8_t link;          // -1 any path, -2 room name, else 0..5
  char room[16];
  char id[4][24];
  uint8_t nId;
  uint8_t timeoutMin;
  uint8_t flags;        // bit0 night-only, bit1 home-only
  uint8_t bri;
  uint8_t held;
  uint8_t armed;
  uint32_t offAt;
};

struct __attribute__((packed)) RuleSave {
  uint8_t used;
  int8_t link;
  char room[16];
  char id[4][24];
  uint8_t nId, timeoutMin, flags, bri;
};

static Light *lts = nullptr;
static int ltN = 0;
static Rule *rules = nullptr;
static bool betaOn = false;
static bool ltSocks = false;
static bool tuyaBound = false;
static uint32_t ltScanAt = 0;
static uint8_t ltSeq = 1;
static WiFiUDP gUdp, wUdp, lUdp, tUdp6, tUdp7;
static TuyaSec *tuyaKeys = nullptr;
static const size_t TUYA_BYTES = sizeof(TuyaSec) * TUYA_KEYS;
static bool lightsDirty = false;
static int qIdx = -1;
static bool qOn = false;
static uint8_t qBri = 80;
static int bleQ = -1;
static bool bleQOn = false;
static uint8_t bleQBri = 80;
static bool bleScanReq = false;
struct LanCmd { int8_t i; uint8_t on; uint8_t bri; };
static LanCmd lanQ[8];
static uint8_t lanQn = 0;
static portMUX_TYPE lanMux = portMUX_INITIALIZER_UNLOCKED;
static uint8_t scanOct[254];
static int scanN = 0, scanUdp = 0, scanTcp = 0;
static uint32_t nextUdp = 0, nextTcp = 0;
struct LtDiag {
  uint16_t gSent, gRx, wSent, wRx, lSent, lRx;
  uint16_t tHeard, tOpen, tFrame;
  uint16_t hosts, done;
  bool scanning, whole;
  char phase[28];
} diag;

static const char *jfind(const char *json, const char *key) {
  char pat[40];
  snprintf(pat, sizeof pat, "\"%s\"", key);
  const char *p = strstr(json, pat);
  if (!p) return nullptr;
  p += strlen(pat);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
  if (*p != ':') return nullptr;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  return p;
}
static bool jstr(const char *json, const char *key, char *out, int n) {
  const char *p = jfind(json, key);
  if (!p || *p != '"') return false;
  p++;
  int i = 0;
  while (*p && *p != '"' && i < n - 1) {
    if (*p == '\\' && p[1]) p++;
    out[i++] = *p++;
  }
  out[i] = 0;
  return i > 0;
}
static int jintv(const char *json, const char *key, int def) {
  const char *p = jfind(json, key);
  if (!p) return def;
  return atoi(p);
}

static bool onLan(const IPAddress &ip) {
  if (!staUp) return false;
  IPAddress me = WiFi.localIP();
  return ip[0] == me[0] && ip[1] == me[1] && ip[2] == me[2] && ip[3] > 0 && ip[3] < 255 && ip != me;
}

static bool ltAlloc();
static void ltRead(Preferences &p);
static void ltSave() {
  RuleSave s[RULE_MAX];
  memset(s, 0, sizeof s);
  if (rules) for (int i = 0; i < RULE_MAX; i++) {
    s[i].used = rules[i].used;
    s[i].link = rules[i].link;
    memcpy(s[i].room, rules[i].room, sizeof s[i].room);
    memcpy(s[i].id, rules[i].id, sizeof s[i].id);
    s[i].nId = rules[i].nId;
    s[i].timeoutMin = rules[i].timeoutMin;
    s[i].flags = rules[i].flags;
    s[i].bri = rules[i].bri;
  }
  Preferences p;
  p.begin("espradar-v4", false);
  p.putUChar("beta", betaOn ? 1 : 0);
  if (rules) p.putBytes("rules", s, sizeof s);
  LightSave *ls = lts ? (LightSave *)psAlloc(sizeof(LightSave) * LT_MAX, "light-save") : nullptr;
  int n = ltN;
  if (n > LT_MAX) n = LT_MAX;
  if (ls) {
    for (int i = 0; i < n; i++) {
      ls[i].brand = lts[i].brand;
      memcpy(ls[i].ip, lts[i].ip, 4);
      ls[i].flags = lts[i].flags;
      memcpy(ls[i].id, lts[i].id, sizeof ls[i].id);
      memcpy(ls[i].name, lts[i].name, sizeof ls[i].name);
      memcpy(ls[i].model, lts[i].model, sizeof ls[i].model);
    }
    p.putUChar("ln", (uint8_t)n);
    p.putBytes("lights", ls, n * sizeof(LightSave));
    free(ls);
  }
  if (tuyaKeys) p.putBytes("tuya", tuyaKeys, TUYA_BYTES);
  p.end();
  lightsDirty = false;
}
static void ltRead(Preferences &p) {
  if (!lts || !rules) return;
  RuleSave s[RULE_MAX];
  memset(s, 0, sizeof s);
  if (p.getBytesLength("rules") == sizeof s) p.getBytes("rules", s, sizeof s);
  for (int i = 0; i < RULE_MAX; i++) {
    memset(&rules[i], 0, sizeof(Rule));
    rules[i].used = s[i].used;
    rules[i].link = s[i].link;
    memcpy(rules[i].room, s[i].room, 15);
    memcpy(rules[i].id, s[i].id, sizeof rules[i].id);
    rules[i].nId = s[i].nId > 4 ? 4 : s[i].nId;
    rules[i].timeoutMin = s[i].timeoutMin ? s[i].timeoutMin : 5;
    rules[i].flags = s[i].flags;
    rules[i].bri = s[i].bri ? s[i].bri : 80;
  }
  int n = p.getUChar("ln", 0);
  if (n > LT_MAX) n = LT_MAX;
  if (n && lts && p.getBytesLength("lights") >= (size_t)n * sizeof(LightSave)) {
    LightSave *ls = (LightSave *)psAlloc(sizeof(LightSave) * LT_MAX, "light-load");
    if (!ls) return;
    p.getBytes("lights", ls, n * sizeof(LightSave));
    ltN = n;
    for (int i = 0; i < n; i++) {
      memset(&lts[i], 0, sizeof(Light));
      lts[i].brand = ls[i].brand;
      memcpy(lts[i].ip, ls[i].ip, 4);
      lts[i].flags = ls[i].flags;
      memcpy(lts[i].id, ls[i].id, sizeof lts[i].id);
      memcpy(lts[i].name, ls[i].name, sizeof lts[i].name);
      memcpy(lts[i].model, ls[i].model, sizeof lts[i].model);
      lts[i].id[sizeof lts[i].id - 1] = 0;
      lts[i].name[sizeof lts[i].name - 1] = 0;
      lts[i].seen = 1;
    }
    free(ls);
  }
  if (tuyaKeys && p.getBytesLength("tuya") == TUYA_BYTES) p.getBytes("tuya", tuyaKeys, TUYA_BYTES);
  if (tuyaKeys) for (int i = 0; i < TUYA_KEYS; i++) tuyaKeys[i].id[sizeof tuyaKeys[i].id - 1] = tuyaKeys[i].key[16] = 0;
}

static void ltLoad() {
  Preferences p;
  p.begin("espradar-v4", true);
  betaOn = p.getUChar("beta", 0) != 0;
  if (betaOn && crashReset(bootReason)) {
    p.end();
    betaOn = false;
    safeMode = true;
    Preferences w;
    w.begin("espradar-v4", false);
    w.putUChar("beta", 0);
    w.end();
    Serial.printf("# safe mode: last reset %s, Beta forced off\n", rstName(bootReason));
    return;
  }
  if (!betaOn) { p.end(); return; }
  if (!ltAlloc()) { p.end(); betaOn = false; Serial.println("# beta left off: no memory for light tables"); return; }
  ltRead(p);
  p.end();
}

static int ltFind(const char *id) {
  if (!lts || !id || !id[0]) return -1;
  for (int i = 0; i < ltN; i++) if (!strcmp(lts[i].id, id)) return i;
  return -1;
}
static int ltUpsert(uint8_t brand, const IPAddress &ip, const char *id, const char *name, const char *model) {
  if (!lts || !id || !id[0]) return -1;
  if (brand != BR_GOVEE_BLE && !onLan(ip)) return -1;
  int i = ltFind(id);
  if (i < 0) {
    if (ltN < LT_MAX) i = ltN++;
    else {
      i = 0;
      for (int k = 1; k < LT_MAX; k++) if (lts[k].seen < lts[i].seen) i = k;
    }
    memset(&lts[i], 0, sizeof(Light));
    strncpy(lts[i].id, id, sizeof lts[i].id - 1);
  }
  lts[i].brand = brand;
  lts[i].ip[0] = ip[0]; lts[i].ip[1] = ip[1]; lts[i].ip[2] = ip[2]; lts[i].ip[3] = ip[3];
  if (name && name[0]) { strncpy(lts[i].name, name, sizeof lts[i].name - 1); lts[i].name[sizeof lts[i].name - 1] = 0; }
  if (model && model[0]) { strncpy(lts[i].model, model, sizeof lts[i].model - 1); lts[i].model[sizeof lts[i].model - 1] = 0; }
  lts[i].seen = millis();
  return i;
}

static bool udpSend(WiFiUDP &u, const IPAddress &ip, uint16_t port, const uint8_t *buf, size_t n) {
  if (!u.beginPacket(ip, port)) return false;
  u.write(buf, n);
  return u.endPacket() == 1;
}
static bool udpText(WiFiUDP &u, const IPAddress &ip, uint16_t port, const char *msg) {
  return udpSend(u, ip, port, (const uint8_t *)msg, strlen(msg));
}

static void lifxHdr(uint8_t *p, uint16_t size, uint16_t type, bool tagged) {
  memset(p, 0, size);
  p[0] = size & 0xff;
  p[1] = size >> 8;
  uint16_t proto = (uint16_t)(1024 | (1 << 12) | (tagged ? (1 << 13) : 0));
  p[2] = proto & 0xff;
  p[3] = proto >> 8;
  p[4] = 'R'; p[5] = 'A'; p[6] = 'D'; p[7] = 'R';
  p[22] = 0;
  p[23] = ltSeq++;
  p[32] = type & 0xff;
  p[33] = type >> 8;
}

static bool ltEnsure() {
  if (ltSocks) return true;
  bool ok = gUdp.begin(4002) && wUdp.begin(38899) && lUdp.begin(56700);
  ltSocks = ok;
  if (!ok) Serial.println("# lights: UDP bind failed");
  return ok;
}

static bool ltAlloc() {
  if (!lts) lts = (Light *)psAlloc(sizeof(Light) * LT_MAX, "lights");
  if (!rules) rules = (Rule *)psAlloc(sizeof(Rule) * RULE_MAX, "rules");
  if (!tuyaKeys) tuyaKeys = (TuyaSec *)psAlloc(TUYA_BYTES, "tuya-keys");
  goveeBleStorage();
  return lts && rules && tuyaKeys;
}
static void ltStopSockets() {
  if (ltSocks) { gUdp.stop(); wUdp.stop(); lUdp.stop(); ltSocks = false; }
  if (tuyaBound) { tUdp6.stop(); tUdp7.stop(); tuyaBound = false; }
}
static void ltFree() {
  ltStopSockets();
  free(lts); lts = nullptr; ltN = 0;
  free(rules); rules = nullptr;
  if (tuyaKeys) { memset(tuyaKeys, 0, TUYA_BYTES); free(tuyaKeys); tuyaKeys = nullptr; }
  goveeBleStorageFree();
}

static int tuyaSlot(const char *id) {
  if (!tuyaKeys) return -1;
  if (!id || !id[0]) return -1;
  int freeSlot = -1;
  for (int i = 0; i < TUYA_KEYS; i++) {
    if (!tuyaKeys[i].id[0] && freeSlot < 0) freeSlot = i;
    if (tuyaKeys[i].id[0] && !strcmp(tuyaKeys[i].id, id)) return i;
  }
  return freeSlot;
}
static bool tuyaHasKey(const char *id) {
  int i = tuyaSlot(id);
  return i >= 0 && tuyaKeys[i].id[0] && !strcmp(tuyaKeys[i].id, id) && tuyaKeys[i].key[0];
}
static int tuyaVerOf(const char *id) {
  int i = tuyaSlot(id);
  if (i < 0 || !tuyaKeys[i].id[0] || strcmp(tuyaKeys[i].id, id)) return 33;
  return tuyaKeys[i].ver ? tuyaKeys[i].ver : 33;
}

static const char *GOVEE_SCAN = "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
static const char *WIZ_PROBE = "{\"method\":\"getPilot\",\"params\":{}}";

static void probeHost(const IPAddress &ip) {
  if (!onLan(ip)) return;
  if (udpText(gUdp, ip, 4001, GOVEE_SCAN)) diag.gSent++;
  if (udpText(wUdp, ip, 38899, WIZ_PROBE)) diag.wSent++;
  uint8_t pkt[36];
  lifxHdr(pkt, 36, 2, false);
  if (udpSend(lUdp, ip, 56700, pkt, 36)) diag.lSent++;
}

static void broadcastScan() {
  if (udpText(gUdp, IPAddress(239, 255, 255, 250), 4001, GOVEE_SCAN)) diag.gSent++;
  if (udpText(gUdp, IPAddress(255, 255, 255, 255), 4001, GOVEE_SCAN)) diag.gSent++;
  if (staUp) {
    IPAddress me = WiFi.localIP();
    if (udpText(gUdp, IPAddress(me[0], me[1], me[2], 255), 4001, GOVEE_SCAN)) diag.gSent++;
    if (udpText(wUdp, IPAddress(me[0], me[1], me[2], 255), 38899, WIZ_PROBE)) diag.wSent++;
  }
  if (udpText(wUdp, IPAddress(255, 255, 255, 255), 38899, WIZ_PROBE)) diag.wSent++;
  uint8_t pkt[36];
  lifxHdr(pkt, 36, 2, true);
  if (udpSend(lUdp, IPAddress(255, 255, 255, 255), 56700, pkt, 36)) diag.lSent++;
}

static void lanSend(int i, bool on, int bri) {
  IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
  char buf[160];
  if (lts[i].brand == BR_GOVEE) {
    snprintf(buf, sizeof buf, "{\"msg\":{\"cmd\":\"turn\",\"data\":{\"value\":%d}}}", on ? 1 : 0);
    udpText(gUdp, ip, 4003, buf);
    if (on) {
      snprintf(buf, sizeof buf, "{\"msg\":{\"cmd\":\"brightness\",\"data\":{\"value\":%d}}}", bri);
      udpText(gUdp, ip, 4003, buf);
    }
  } else if (lts[i].brand == BR_WIZ) {
    if (on) snprintf(buf, sizeof buf, "{\"method\":\"setPilot\",\"params\":{\"state\":true,\"dimming\":%d}}", constrain(bri, 10, 100));
    else snprintf(buf, sizeof buf, "{\"method\":\"setPilot\",\"params\":{\"state\":false}}");
    udpText(wUdp, ip, 38899, buf);
  } else if (lts[i].brand == BR_LIFX) {
    uint8_t pkt[42];
    lifxHdr(pkt, 42, 117, false);
    uint16_t level = on ? 65535 : 0;
    pkt[36] = level & 0xff;
    pkt[37] = level >> 8;
    udpSend(lUdp, ip, 56700, pkt, 42);
  }
  lts[i].on = on ? 1 : 0;
  if (on) lts[i].bri = (uint8_t)bri;
  Serial.printf("# light %s %s bri=%d brand=%u\n", lts[i].name, on ? "on" : "off", bri, lts[i].brand);
}

static void lanFlush() {
  LanCmd batch[8];
  uint8_t n;
  portENTER_CRITICAL(&lanMux);
  n = lanQn;
  if (n > 8) n = 8;
  memcpy(batch, lanQ, n * sizeof(LanCmd));
  lanQn = 0;
  portEXIT_CRITICAL(&lanMux);
  for (uint8_t k = 0; k < n; k++) lanSend(batch[k].i, batch[k].on, batch[k].bri);
}

static void lightSet(int i, bool on, int bri) {
  if (i < 0 || i >= ltN) return;
  IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
  if (lts[i].brand != BR_GOVEE_BLE && !onLan(ip)) return;
  if (bri < 1) bri = 1;
  if (bri > 100) bri = 100;
  if (lts[i].brand == BR_GOVEE || lts[i].brand == BR_WIZ || lts[i].brand == BR_LIFX) {
    portENTER_CRITICAL(&lanMux);
    if (lanQn < 8) lanQ[lanQn++] = LanCmd{(int8_t)i, (uint8_t)(on ? 1 : 0), (uint8_t)bri};
    portEXIT_CRITICAL(&lanMux);
    return;
  }
  if (lts[i].brand == BR_TUYA) {
    qIdx = i; qOn = on; qBri = (uint8_t)bri;
    return;
  } else if (lts[i].brand == BR_GOVEE_BLE) {
    bleQ = i; bleQOn = on; bleQBri = (uint8_t)bri;
    return;
  }
  lts[i].on = on ? 1 : 0;
  if (on) lts[i].bri = (uint8_t)bri;
  Serial.printf("# light %s %s bri=%d brand=%u\n", lts[i].name, on ? "on" : "off", bri, lts[i].brand);
}

static void ingestGovee(const char *js, const IPAddress &from) {
  char id[24] = "", sku[12] = "", ipb[16] = "";
  bool scan = strstr(js, "\"scan\"") != nullptr;
  bool status = strstr(js, "onOff") != nullptr;
  if (scan) {
    jstr(js, "device", id, sizeof id);
    jstr(js, "sku", sku, sizeof sku);
    jstr(js, "ip", ipb, sizeof ipb);
    IPAddress ip = from;
    IPAddress parsed;
    if (!onLan(ip) && parsed.fromString(ipb) && onLan(parsed)) ip = parsed;
    if (!id[0]) snprintf(id, sizeof id, "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    char name[20];
    snprintf(name, sizeof name, "Govee %s", sku[0] ? sku : "light");
    if (ltUpsert(BR_GOVEE, ip, id, name, sku) >= 0) { diag.gRx++; lightsDirty = true; }
  }
  if (status) {
    for (int i = 0; i < ltN; i++) {
      IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
      if (ip == from) {
        int on = jintv(js, "onOff", -1);
        int br = jintv(js, "brightness", -1);
        if (on == 0 || on == 1) lts[i].on = (uint8_t)on;
        if (br >= 0 && br <= 100) lts[i].bri = (uint8_t)br;
        lts[i].seen = millis();
      }
    }
  }
}
static void ingestWiz(const char *js, const IPAddress &from) {
  char mac[20] = "", mod[24] = "";
  if (!jstr(js, "mac", mac, sizeof mac) && !strstr(js, "moduleName") && !strstr(js, "getSystemConfig") && !strstr(js, "getPilot")) return;
  jstr(js, "moduleName", mod, sizeof mod);
  if (!mac[0]) snprintf(mac, sizeof mac, "wiz-%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
  char name[20];
  snprintf(name, sizeof name, "WiZ %s", mod[0] ? mod : "light");
  // moduleName can be long; the name field is 19 chars + NUL
  name[19] = 0;
  if (ltUpsert(BR_WIZ, from, mac, name, "WiZ") >= 0) { diag.wRx++; lightsDirty = true; }
}
static void ingestLifx(const IPAddress &from) {
  char id[24];
  snprintf(id, sizeof id, "lifx-%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
  if (ltUpsert(BR_LIFX, from, id, "LIFX", "LAN") >= 0) { diag.lRx++; lightsDirty = true; }
}

static void ltPoll() {
  if (!ltSocks) return;
  char buf[520];
  int n;
  while ((n = gUdp.parsePacket()) > 0) {
    int r = gUdp.read((uint8_t *)buf, sizeof buf - 1);
    if (r < 0) r = 0;
    buf[r] = 0;
    IPAddress from = gUdp.remoteIP();
    if (onLan(from)) ingestGovee(buf, from);
  }
  while ((n = wUdp.parsePacket()) > 0) {
    int r = wUdp.read((uint8_t *)buf, sizeof buf - 1);
    if (r < 0) r = 0;
    buf[r] = 0;
    IPAddress from = wUdp.remoteIP();
    if (onLan(from)) ingestWiz(buf, from);
  }
  while ((n = lUdp.parsePacket()) > 0) {
    int r = lUdp.read((uint8_t *)buf, sizeof buf - 1);
    if (r < 0) r = 0;
    IPAddress from = lUdp.remoteIP();
    if (onLan(from) && r >= 36) ingestLifx(from);
  }
}

static bool ruleMotion(const Rule &r) {
  if (r.link == -1) return v4AnyMotion();
  if (r.link == -2) return v4RoomMotion(r.room);
  return v4LinkMotion(r.link);
}

static void noteTuya(const IPAddress &ip, const char *gw, const char *verStr, bool fromUdp) {
  if (!onLan(ip)) return;
  if (gw && gw[0]) {
    for (int k = 0; k < ltN; k++) {
      if (lts[k].brand == BR_TUYA && lts[k].ip[0] == ip[0] && lts[k].ip[1] == ip[1] && lts[k].ip[2] == ip[2] && lts[k].ip[3] == ip[3] &&
          (!strncmp(lts[k].id, "tuya-", 5) || !strncmp(lts[k].id, "ip-", 3))) {
        strncpy(lts[k].id, gw, sizeof lts[k].id - 1);
        lts[k].id[sizeof lts[k].id - 1] = 0;
      }
    }
  }
  char id[24];
  if (gw && gw[0]) strncpy(id, gw, sizeof id - 1), id[sizeof id - 1] = 0;
  else snprintf(id, sizeof id, "tuya-%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  int ver = 33;
  if (verStr && verStr[0] == '3' && verStr[1] == '.') ver = 30 + (verStr[2] - '0');
  if (ver != 33 && ver != 34 && ver != 35) ver = 33;
  char name[20];
  snprintf(name, sizeof name, "Tuya %s", verStr && verStr[0] ? verStr : "light");
  int i = ltUpsert(BR_TUYA, ip, id, name, "needs key");
  if (i >= 0) {
    if (!tuyaHasKey(lts[i].id)) lts[i].flags = (uint8_t)(lts[i].flags | 1);
    else lts[i].flags = (uint8_t)(lts[i].flags & ~1);
    lightsDirty = true;
  }
  if (fromUdp) diag.tHeard++;
  (void)ver;
}

static void tuyaUdpPoll(WiFiUDP &u) {
  char buf[400];
  int n;
  while ((n = u.parsePacket()) > 0) {
    int r = u.read((uint8_t *)buf, sizeof buf - 1);
    if (r < 0) r = 0;
    buf[r] = 0;
    IPAddress from = u.remoteIP();
    if (!onLan(from)) continue;
    if (buf[0] != '{') continue;
    char gw[24] = "", ver[8] = "", ipb[16] = "";
    jstr(buf, "gwId", gw, sizeof gw);
    if (!gw[0]) jstr(buf, "devId", gw, sizeof gw);
    jstr(buf, "version", ver, sizeof ver);
    jstr(buf, "ip", ipb, sizeof ipb);
    IPAddress ip = from;
    IPAddress parsed;
    if (parsed.fromString(ipb) && onLan(parsed)) ip = parsed;
    if (gw[0] || strstr(buf, "\"gwId\"") || strstr(buf, "productKey")) noteTuya(ip, gw, ver, true);
  }
}

static void rememberOct(uint8_t oct, uint8_t *seen) {
  if (!oct || oct == 255 || seen[oct]) return;
  IPAddress me = WiFi.localIP();
  if (oct == me[3]) return;
  if (scanN >= (int)sizeof scanOct) return;
  seen[oct] = 1;
  scanOct[scanN++] = oct;
}

static bool scanPending = false;
static bool scanWholeReq = false;
static bool manualPending = false;
static IPAddress manualIp;

static void startScan(bool whole) {
  memset(&diag, 0, sizeof diag);
  diag.scanning = true;
  diag.whole = whole;
  scanN = scanUdp = scanTcp = 0;
  uint8_t seen[256];
  memset(seen, 0, sizeof seen);
  if (whole) {
    for (int a = 1; a <= 254; a++) rememberOct((uint8_t)a, seen);
  } else {
    for (int i = 0; i < devCount; i++) rememberOct(devs[i].ip4, seen);
    for (int i = 0; i < ltN; i++) rememberOct(lts[i].ip[3], seen);
  }
  diag.hosts = (uint16_t)scanN;
  strncpy(diag.phase, scanN ? "broadcast" : "no hosts", sizeof diag.phase - 1);
  if (!tuyaBound) { tUdp6.begin(6666); tUdp7.begin(6667); tuyaBound = true; }
  broadcastScan();
  nextUdp = millis();
  nextTcp = millis() + 40;
  Serial.printf("# light scan %s, %d hosts (unicast; broadcast kept too)\n", whole ? "whole /24" : "device list", scanN);
}

static void v4LightScan(bool whole) {
  if (!betaOn) return;
  scanWholeReq = whole;
  scanPending = true;
}

static void v4LightManual(const IPAddress &ip) {
  if (!onLan(ip)) return;
  manualIp = ip;
  manualPending = true;
}

static void doManual() {
  IPAddress ip = manualIp;
  probeHost(ip);
  char id[28];
  snprintf(id, sizeof id, "ip-%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  int i = -1;
  for (int k = 0; k < ltN; k++) if (lts[k].ip[0] == ip[0] && lts[k].ip[1] == ip[1] && lts[k].ip[2] == ip[2] && lts[k].ip[3] == ip[3]) { i = k; break; }
  if (i < 0) ltUpsert(BR_TUYA, ip, id, "Manual IP", "waiting");
  lightsDirty = true;
}

static bool v4TuyaKeySet(const char *devId, const char *ipStr, const char *key, int ver, bool clearKey) {
  IPAddress ip;
  if (!ip.fromString(ipStr) || !onLan(ip)) return false;
  if (ver != 33 && ver != 34 && ver != 35) ver = 33;
  char id[24];
  if (devId && devId[0]) {
    strncpy(id, devId, sizeof id - 1);
    id[sizeof id - 1] = 0;
  } else snprintf(id, sizeof id, "tuya-%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
  int li = -1;
  for (int k = 0; k < ltN; k++) if (lts[k].ip[3] == ip[3] && lts[k].ip[0] == ip[0] && lts[k].brand == BR_TUYA) { li = k; break; }
  if (li < 0) li = ltUpsert(BR_TUYA, ip, id, "Tuya", "local");
  if (li < 0) return false;
  if (devId && devId[0] && strcmp(lts[li].id, id)) {
    strncpy(lts[li].id, id, sizeof lts[li].id - 1);
    lts[li].id[sizeof lts[li].id - 1] = 0;
  }
  int s = tuyaSlot(lts[li].id);
  if (s < 0) return false;
  if (clearKey) {
    memset(&tuyaKeys[s], 0, sizeof(TuyaSec));
    lts[li].flags = (uint8_t)(lts[li].flags | 1);
  } else {
    strncpy(tuyaKeys[s].id, lts[li].id, sizeof tuyaKeys[s].id - 1);
    tuyaKeys[s].id[sizeof tuyaKeys[s].id - 1] = 0;
    if (key && key[0]) {
      strncpy(tuyaKeys[s].key, key, 16);
      tuyaKeys[s].key[16] = 0;
    }
    tuyaKeys[s].ver = (uint8_t)ver;
    if (tuyaKeys[s].key[0]) lts[li].flags = (uint8_t)(lts[li].flags & ~1);
    else lts[li].flags = (uint8_t)(lts[li].flags | 1);
  }
  lightsDirty = true;
  ltSave();
  return true;
}

static void scanPump() {
  if (!diag.scanning) return;
  uint32_t now = millis();
  IPAddress me = WiFi.localIP();
  if (scanUdp < scanN && now >= nextUdp) {
    nextUdp = now + 70;
    IPAddress ip(me[0], me[1], me[2], scanOct[scanUdp]);
    probeHost(ip);
    scanUdp++;
    diag.done = (uint16_t)scanUdp;
    snprintf(diag.phase, sizeof diag.phase, "unicast %d/%d", scanUdp, scanN);
  }
  if (scanTcp < scanN && now >= nextTcp) {
    nextTcp = now + 350;                 // one TCP socket, then a gap so lwIP can drain
    IPAddress ip(me[0], me[1], me[2], scanOct[scanTcp]);
    int hit = tuyaPortProbe(ip);
    if (hit) {
      diag.tOpen++;
      if (hit == 2) diag.tFrame++;
      noteTuya(ip, "", hit == 2 ? "3.3" : "", false);
    }
    scanTcp++;
  }
  if (scanUdp >= scanN && scanTcp >= scanN) {
    diag.scanning = false;
    strncpy(diag.phase, "done", sizeof diag.phase - 1);
    if (tuyaBound) { tUdp6.stop(); tUdp7.stop(); tuyaBound = false; }
    if (lightsDirty) ltSave();
    Serial.printf("# light scan done govee %u/%u wiz %u/%u lifx %u/%u tuya heard %u open %u framed %u\n",
                  diag.gRx, diag.gSent, diag.wRx, diag.wSent, diag.lRx, diag.lSent, diag.tHeard, diag.tOpen, diag.tFrame);
  }
}

static void v4GoveeBleRequest() { bleScanReq = true; }

static void goveeBleMerge() {
  IPAddress z(0, 0, 0, 0);
  for (int h = 0; h < goveeHitN; h++) {
    if (!goveeHits[h].used) continue;
    char label[20];
    snprintf(label, sizeof label, "Govee %s", goveeHits[h].model[0] ? goveeHits[h].model : "BLE");
    int i = ltUpsert(BR_GOVEE_BLE, z, goveeHits[h].mac, label, goveeHits[h].model);
    if (i >= 0) {
      lts[i].rssi = goveeHits[h].rssi;
      lts[i].flags = (uint8_t)(lts[i].flags & ~1);
      lightsDirty = true;
    }
  }
}

static void goveeBlePump() {
  if (bleScanReq && !goveeBleScanning) {
    bleScanReq = false;
    goveeBleScan(4);
    goveeBleMerge();
    return;
  }
  if (bleQ < 0 || bleQ >= ltN) return;
  int i = bleQ;
  bleQ = -1;
  uint8_t typ = BLE_ADDR_PUBLIC;
  for (int h = 0; h < goveeHitN; h++) if (!strcasecmp(goveeHits[h].mac, lts[i].id)) { typ = goveeHits[h].addrType; break; }
  bool ok = goveeBleWrite(lts[i].id, typ, bleQOn, bleQBri);
  if (ok) {
    lts[i].on = bleQOn ? 1 : 0;
    if (bleQOn) lts[i].bri = bleQBri;
  }
}

static void tuyaPump() {
  if (qIdx < 0 || qIdx >= ltN) return;
  int i = qIdx;
  qIdx = -1;
  if (lts[i].brand != BR_TUYA) return;
  int s = tuyaSlot(lts[i].id);
  if (s < 0 || !tuyaKeys[s].key[0] || strcmp(tuyaKeys[s].id, lts[i].id)) {
    Serial.printf("# light %s found, needs a local key\n", lts[i].name);
    return;
  }
  char key[17];
  memcpy(key, tuyaKeys[s].key, 17);
  int ver = tuyaKeys[s].ver ? tuyaKeys[s].ver : 33;
  IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
  bool ok = tuyaControl(ip, lts[i].id, key, ver, qOn, qBri);
  memset(key, 0, sizeof key);
  if (ok) {
    lts[i].on = qOn ? 1 : 0;
    if (qOn) lts[i].bri = qBri;
  }
  Serial.printf("# tuya %s %s ver=%d reply=%s\n", lts[i].name, qOn ? "on" : "off", ver, ok ? "yes" : "no");
}

static TaskHandle_t ltTaskHandle = nullptr;

// Runs on its own task. loop() must not call this: connect/select/BLE block the caller.
static void v4LightsWorker() {
  if (!betaOn || !staUp || !lts) return;
  if (!ltEnsure()) return;
  if (scanPending) { scanPending = false; startScan(scanWholeReq); }
  if (manualPending) { manualPending = false; doManual(); }
  lanFlush();
  uint32_t now = millis();
  ltPoll();
  if (tuyaBound) { tuyaUdpPoll(tUdp6); tuyaUdpPoll(tUdp7); }
  if (bleScanReq || bleQ >= 0) goveeBlePump();
  else if (qIdx >= 0) tuyaPump();
  else scanPump();
  if (!diag.scanning && now - ltScanAt > 25000) {
    ltScanAt = now;
    broadcastScan();
  }
  static uint32_t lastLtSave = 0;
  if (lightsDirty && !diag.scanning && now - lastLtSave > 8000) { lastLtSave = now; ltSave(); }
  for (int i = 0; i < RULE_MAX; i++) {
    Rule &r = rules[i];
    if (!r.used || r.nId == 0) continue;
    bool mot = ruleMotion(r);
    bool allow = true;
    if ((r.flags & 1) && !v4IsNight()) allow = false;
    if ((r.flags & 2) && v4HomeCode() != 1) allow = false;
    if (mot && allow) {
      if (!r.held) {
        for (int k = 0; k < r.nId; k++) {
          int li = ltFind(r.id[k]);
          if (li >= 0) lightSet(li, true, r.bri ? r.bri : 80);
        }
        r.held = 1;
      }
      r.armed = 0;
    } else if (r.held && !mot && allow) {
      if (!r.armed) {
        uint32_t mins = r.timeoutMin ? r.timeoutMin : 5;
        r.offAt = now + mins * 60000ul;
        r.armed = 1;
      }
      if ((int32_t)(now - r.offAt) >= 0) {
        for (int k = 0; k < r.nId; k++) {
          int li = ltFind(r.id[k]);
          if (li >= 0) lightSet(li, false, r.bri);
        }
        r.held = 0;
        r.armed = 0;
      }
    } else if (r.held && !allow) {
      for (int k = 0; k < r.nId; k++) {
        int li = ltFind(r.id[k]);
        if (li >= 0) lightSet(li, false, r.bri);
      }
      r.held = 0;
      r.armed = 0;
    }
  }
}

static void ltTask(void *) {
  for (;;) {
    if (!betaOn) break;
    v4LightsWorker();
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  ltTaskHandle = nullptr;
  vTaskDelete(nullptr);
}
static void v4LightsStart() {
  if (!betaOn || !lts || ltTaskHandle) return;
  xTaskCreatePinnedToCore(ltTask, "lights", 10240, nullptr, 1, &ltTaskHandle, 1);
}

static void v4SetBeta(bool on) {
  if (on) {
    if (!ltAlloc()) { Serial.println("# beta refused, no memory for light tables"); return; }
    betaOn = true;
    Preferences p;
    p.begin("espradar-v4", true);
    ltRead(p);
    p.end();
    ltScanAt = 0;
    ltSave();
    v4LightsStart();
  } else {
    betaOn = false;
    ltSave();
    for (int i = 0; i < 80 && ltTaskHandle; i++) vTaskDelay(pdMS_TO_TICKS(50));
    ltFree();
  }
  Serial.printf("# lights beta %s\n", betaOn ? "ON" : "off");
}
static bool v4Beta() { return betaOn; }

static bool v4LightTest(const char *id, const char *op, int bri) {
  if (!betaOn) return false;
  if (op && !strcmp(op, "offall")) {
    for (int i = 0; i < ltN; i++) lightSet(i, false, 1);
    return true;
  }
  int i = ltFind(id);
  if (i < 0) return false;
  bool on = op && !strcmp(op, "on");
  lightSet(i, on, bri > 0 ? bri : (lts[i].bri ? lts[i].bri : 80));
  return true;
}

static void v4RuleSet(int idx, int used, int link, const char *room, const char *ids, int timeout, int night, int home, int bri) {
  if (!rules || idx < 0 || idx >= RULE_MAX) return;
  Rule &r = rules[idx];
  memset(&r, 0, sizeof r);
  r.used = used ? 1 : 0;
  r.link = (int8_t)constrain(link, -2, V4_LINKS - 1);
  if (room) { strncpy(r.room, room, 15); r.room[15] = 0; }
  r.nId = 0;
  if (ids && r.used) {
    char buf[120];
    strncpy(buf, ids, sizeof buf - 1);
    buf[sizeof buf - 1] = 0;
    char *save = nullptr;
    for (char *tok = strtok_r(buf, ",", &save); tok && r.nId < 4; tok = strtok_r(nullptr, ",", &save)) {
      while (*tok == ' ') tok++;
      if (!*tok) continue;
      strncpy(r.id[r.nId], tok, 23);
      r.id[r.nId][23] = 0;
      r.nId++;
    }
  }
  int t = timeout ? timeout : 5;
  r.timeoutMin = (uint8_t)constrain(t, 1, 180);
  r.flags = (uint8_t)((night ? 1 : 0) | (home ? 2 : 0));
  r.bri = (uint8_t)constrain(bri ? bri : 80, 1, 100);
  ltSave();
}

static const char *brandName(uint8_t b) {
  if (b == BR_GOVEE) return "govee";
  if (b == BR_WIZ) return "wiz";
  if (b == BR_LIFX) return "lifx";
  if (b == BR_TUYA) return "tuya";
  if (b == BR_GOVEE_BLE) return "govee-ble";
  return "?";
}

static void v4AppendLights(JBuf &b) {
  int home = v4HomeCode();
  b.addf("{\"beta\":%s,\"v\":\"4.1\",\"night\":%s,\"home\":\"%s\",\"diag\":{\"scanning\":%s,\"phase\":\"",
         betaOn ? "true" : "false", v4IsNight() ? "true" : "false",
         home == 1 ? "home" : (home == 2 ? "away" : (home == 3 ? "checking" : "none")),
         diag.scanning ? "true" : "false");
  b.esc(diag.phase);
  b.addf("\",\"hosts\":%u,\"done\":%u,\"govee\":[%u,%u],\"wiz\":[%u,%u],\"lifx\":[%u,%u],\"tuyaHeard\":%u,\"tuyaOpen\":%u,\"tuyaFrame\":%u,\"bleScanning\":%s},\"lights\":[",
         diag.hosts, diag.done, diag.gSent, diag.gRx, diag.wSent, diag.wRx, diag.lSent, diag.lRx,
         diag.tHeard, diag.tOpen, diag.tFrame, (goveeBleScanning || bleScanReq) ? "true" : "false");
  for (int i = 0; i < ltN && lts; i++) {
    if (i) b.add(",");
    bool keyed = lts[i].brand == BR_TUYA && tuyaHasKey(lts[i].id);
    b.add("{\"id\":\"");
    b.esc(lts[i].id);
    b.addf("\",\"brand\":\"%s\",\"ip\":\"%u.%u.%u.%u\",\"name\":\"", brandName(lts[i].brand), lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
    b.esc(lts[i].name);
    b.add("\",\"model\":\"");
    b.esc(lts[i].model);
    b.addf("\",\"on\":%u,\"bri\":%u,\"rssi\":%d,\"needsKey\":%s,\"hasKey\":%s,\"ver\":%d}",
           lts[i].on, lts[i].bri, (int)lts[i].rssi,
           (lts[i].brand == BR_TUYA && !keyed) ? "true" : "false", keyed ? "true" : "false",
           lts[i].brand == BR_TUYA ? tuyaVerOf(lts[i].id) : 0);
  }
  b.add("],\"rules\":[");
  for (int i = 0; i < RULE_MAX && rules; i++) {
    if (i) b.add(",");
    const Rule &r = rules[i];
    b.addf("{\"i\":%d,\"used\":%s,\"link\":%d,\"room\":\"", i, r.used ? "true" : "false", (int)r.link);
    b.esc(r.room);
    b.addf("\",\"timeout\":%u,\"night\":%s,\"home\":%s,\"bri\":%u,\"held\":%s,\"ids\":[",
           r.timeoutMin, (r.flags & 1) ? "true" : "false", (r.flags & 2) ? "true" : "false", r.bri, r.held ? "true" : "false");
    for (int k = 0; k < r.nId; k++) {
      if (k) b.add(",");
      b.add("\"");
      b.esc(r.id[k]);
      b.add("\"");
    }
    b.add("]}");
  }
  b.add("]}");
}

static void v4AppendBeta(JBuf &b) {
  b.addf(",\"beta\":%s", betaOn ? "true" : "false");
}
