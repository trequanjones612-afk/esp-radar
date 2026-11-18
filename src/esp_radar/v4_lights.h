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

static const int LT_MAX = 8;
static const int RULE_MAX = 4;
static const uint8_t BR_GOVEE = 1, BR_WIZ = 2, BR_LIFX = 3;

struct Light {
  uint8_t brand;
  uint8_t ip[4];
  char id[24];
  char name[20];
  char model[12];
  uint8_t on;
  uint8_t bri;
  uint32_t seen;
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

static Light lts[LT_MAX];
static int ltN = 0;
static Rule rules[RULE_MAX];
static bool betaOn = false;
static bool ltSocks = false;
static uint32_t ltScanAt = 0;
static uint8_t ltSeq = 1;
static WiFiUDP gUdp, wUdp, lUdp;

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

static void ltSave() {
  RuleSave s[RULE_MAX];
  memset(s, 0, sizeof s);
  for (int i = 0; i < RULE_MAX; i++) {
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
  p.putBytes("rules", s, sizeof s);
  p.end();
}
static void ltLoad() {
  Preferences p;
  p.begin("espradar-v4", true);
  betaOn = p.getUChar("beta", 0) != 0;
  RuleSave s[RULE_MAX];
  memset(s, 0, sizeof s);
  if (p.getBytesLength("rules") == sizeof s) p.getBytes("rules", s, sizeof s);
  p.end();
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
}

static int ltFind(const char *id) {
  if (!id || !id[0]) return -1;
  for (int i = 0; i < ltN; i++) if (!strcmp(lts[i].id, id)) return i;
  return -1;
}
static int ltUpsert(uint8_t brand, const IPAddress &ip, const char *id, const char *name, const char *model) {
  if (!id || !id[0] || !onLan(ip)) return -1;
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

static void goveeScan() {
  static const char *scan = "{\"msg\":{\"cmd\":\"scan\",\"data\":{\"account_topic\":\"reserve\"}}}";
  udpText(gUdp, IPAddress(239, 255, 255, 250), 4001, scan);
  udpText(gUdp, IPAddress(255, 255, 255, 255), 4001, scan);
  if (staUp) {
    IPAddress me = WiFi.localIP();
    udpText(gUdp, IPAddress(me[0], me[1], me[2], 255), 4001, scan);
    for (int i = 0; i < ltN; i++) if (lts[i].brand == BR_GOVEE)
      udpText(gUdp, IPAddress(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]), 4001, scan);
  }
}
static void wizScan() {
  static const char *q = "{\"method\":\"getSystemConfig\",\"params\":{}}";
  udpText(wUdp, IPAddress(255, 255, 255, 255), 38899, q);
  if (staUp) {
    IPAddress me = WiFi.localIP();
    udpText(wUdp, IPAddress(me[0], me[1], me[2], 255), 38899, q);
  }
}
static void lifxScan() {
  uint8_t pkt[36];
  lifxHdr(pkt, 36, 2, true);
  udpSend(lUdp, IPAddress(255, 255, 255, 255), 56700, pkt, 36);
}

static void lightSet(int i, bool on, int bri) {
  if (i < 0 || i >= ltN) return;
  IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
  if (!onLan(ip)) return;
  if (bri < 1) bri = 1;
  if (bri > 100) bri = 100;
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
  Serial.printf("# light %s %s bri=%d (%s)\n", lts[i].name, on ? "on" : "off", bri,
                lts[i].brand == BR_GOVEE ? "govee" : (lts[i].brand == BR_WIZ ? "wiz" : "lifx"));
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
    ltUpsert(BR_GOVEE, ip, id, name, sku);
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
  if (!jstr(js, "mac", mac, sizeof mac) && !strstr(js, "moduleName") && !strstr(js, "getSystemConfig")) return;
  jstr(js, "moduleName", mod, sizeof mod);
  if (!mac[0]) snprintf(mac, sizeof mac, "wiz-%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
  char name[20];
  snprintf(name, sizeof name, "WiZ %s", mod[0] ? mod : "light");
  // moduleName can be long; the name field is 19 chars + NUL
  name[19] = 0;
  ltUpsert(BR_WIZ, from, mac, name, "WiZ");
}
static void ingestLifx(const IPAddress &from) {
  char id[24];
  snprintf(id, sizeof id, "lifx-%u.%u.%u.%u", from[0], from[1], from[2], from[3]);
  ltUpsert(BR_LIFX, from, id, "LIFX", "LAN");
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

static void v4LightsTick() {
  if (!betaOn || !staUp) return;
  if (!ltEnsure()) return;
  uint32_t now = millis();
  ltPoll();
  if (now - ltScanAt > 12000) {
    ltScanAt = now;
    goveeScan();
    wizScan();
    lifxScan();
  }
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

static void v4SetBeta(bool on) {
  betaOn = on;
  ltSave();
  if (on && staUp) { ltEnsure(); ltScanAt = 0; }
  Serial.printf("# lights beta %s\n", on ? "ON" : "off");
}
static bool v4Beta() { return betaOn; }

static bool v4LightTest(const char *id, const char *op, int bri) {
  if (!betaOn) return false;
  if (!ltEnsure()) return false;
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
  if (idx < 0 || idx >= RULE_MAX) return;
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

static const char *brandName(uint8_t b) { return b == BR_GOVEE ? "govee" : (b == BR_WIZ ? "wiz" : (b == BR_LIFX ? "lifx" : "?")); }

static String v4LightsJson() {
  String o = "{\"beta\":" + String(betaOn ? "true" : "false") + ",\"night\":" + String(v4IsNight() ? "true" : "false") +
             ",\"home\":\"" + String(v4HomeCode() == 1 ? "home" : (v4HomeCode() == 2 ? "away" : (v4HomeCode() == 3 ? "checking" : "none"))) +
             "\",\"lights\":[";
  for (int i = 0; i < ltN; i++) {
    if (i) o += ",";
    IPAddress ip(lts[i].ip[0], lts[i].ip[1], lts[i].ip[2], lts[i].ip[3]);
    o += "{\"id\":\"";
    jsonAppendEsc(o, lts[i].id);
    o += "\",\"brand\":\"" + String(brandName(lts[i].brand)) + "\",\"ip\":\"" + ip.toString() + "\",\"name\":\"";
    jsonAppendEsc(o, lts[i].name);
    o += "\",\"model\":\"";
    jsonAppendEsc(o, lts[i].model);
    o += "\",\"on\":" + String(lts[i].on) + ",\"bri\":" + String(lts[i].bri) + "}";
  }
  o += "],\"rules\":[";
  for (int i = 0; i < RULE_MAX; i++) {
    if (i) o += ",";
    const Rule &r = rules[i];
    o += "{\"i\":" + String(i) + ",\"used\":" + String(r.used ? "true" : "false") + ",\"link\":" + String((int)r.link) +
         ",\"room\":\"";
    jsonAppendEsc(o, r.room);
    o += "\",\"timeout\":" + String(r.timeoutMin) + ",\"night\":" + String((r.flags & 1) ? "true" : "false") +
         ",\"home\":" + String((r.flags & 2) ? "true" : "false") + ",\"bri\":" + String(r.bri) +
         ",\"held\":" + String(r.held ? "true" : "false") + ",\"ids\":[";
    for (int k = 0; k < r.nId; k++) {
      if (k) o += ",";
      o += "\"";
      jsonAppendEsc(o, r.id[k]);
      o += "\"";
    }
    o += "]}";
  }
  return o + "]}";
}

static String v4LightsSuffix() { return String(",\"beta\":") + (betaOn ? "true" : "false"); }
