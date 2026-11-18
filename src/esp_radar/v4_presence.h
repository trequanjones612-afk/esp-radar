// ESP Radar v4 — device health and home/away.
//
// Health comes from the ARP sweep the Devices tab already does, plus a slow background
// ARP pass and (when the sniffer is up) radio sightings. No port scans, no logins.
//
// Home/Away is "have the phones you marked been seen on this LAN", with a long leave
// debounce because Android sleeps its WiFi. It is not GPS. A phone that stays asleep
// longer than the debounce, or that only lives on 5 GHz and never answers, can read
// as away while the person is home. Untested on hardware.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <time.h>
#include <string.h>

static const int HMAX = 48;
static const uint32_t V4_LEAVE_MS = 12ul * 60ul * 1000ul;
static const uint32_t V4_ONLINE_MS = 3ul * 60ul * 1000ul;
static const uint8_t HF_PERSON = 0x01;
static const uint8_t HF_LATCH = 0x02;

struct __attribute__((packed)) HRec {
  uint8_t mac[6];
  uint8_t flags;
  char alias[16];
  uint16_t upMin, totMin;
  uint8_t flips;
  uint8_t nightN;
  uint16_t nightYday;
  uint8_t hours[3];
  uint8_t pad;
  uint32_t lastDropEpoch;
  uint32_t lastSeenEpoch;
};

struct PLog {
  uint32_t ms;
  uint32_t epoch;
  uint8_t arrive;
  uint8_t mac[6];
  char name[16];
};

static HRec hdb[HMAX];
static int hCount = 0;
static uint8_t hWasOn[HMAX];
static uint8_t hInit[HMAX];
static uint8_t hHits[HMAX];
static uint32_t hPrevMs[HMAX];
static uint32_t hPrevDev[HMAX];
static PLog plog[16];
static int pCount = 0;
static int tzEast = 0;
static uint8_t night0 = 19, night1 = 7;
static int sweepIp = 1;
static uint32_t prTick = 0, prSweep = 0, prPeople = 0, prSave = 0, prDecay = 0, prBoot = 0;
static bool prDirty = false;
static char homeWho[48] = "";
static const char *homeWord = "none";

static bool v4TimeOk(struct tm &t) {
  time_t e = time(nullptr);
  if (e < 1700000000) return false;
  e += (time_t)tzEast * 60;
  gmtime_r(&e, &t);
  return true;
}
static uint32_t v4Epoch() {
  time_t e = time(nullptr);
  return e > 1700000000 ? (uint32_t)e : 0;
}
static bool v4IsNight() {
  struct tm t;
  if (!v4TimeOk(t)) return false;
  int h = t.tm_hour;
  if (night0 == night1) return true;
  if (night0 < night1) return h >= night0 && h < night1;
  return h >= night0 || h < night1;
}
static int v4HomeCode() {
  if (!strcmp(homeWord, "home")) return 1;
  if (!strcmp(homeWord, "away")) return 2;
  if (!strcmp(homeWord, "checking")) return 3;
  return 0;
}
static const char *v4HomeLabel() {
  if (!strcmp(homeWord, "home")) return "HOME";
  if (!strcmp(homeWord, "away")) return "AWAY";
  if (!strcmp(homeWord, "checking")) return "CHK";
  return "----";
}

static int hFind(const uint8_t *mac) {
  for (int i = 0; i < hCount; i++) if (!memcmp(hdb[i].mac, mac, 6)) return i;
  return -1;
}
static int hEnsure(const uint8_t *mac) {
  int i = hFind(mac);
  if (i >= 0) return i;
  if (hCount >= HMAX) {
    int drop = 0;
    for (int k = 1; k < hCount; k++) if ((hdb[k].flags & HF_PERSON) == 0 && hdb[k].totMin < hdb[drop].totMin) drop = k;
    if (hdb[drop].flags & HF_PERSON) return -1;
    memset(&hdb[drop], 0, sizeof(HRec));
    memcpy(hdb[drop].mac, mac, 6);
    return drop;
  }
  memset(&hdb[hCount], 0, sizeof(HRec));
  memcpy(hdb[hCount].mac, mac, 6);
  return hCount++;
}

static void v4PresenceSave(bool force) {
  if (!force && !prDirty) return;
  Preferences p;
  p.begin("espradar-dev", false);
  p.putBytes("hlth", hdb, hCount * sizeof(HRec));
  p.putUChar("hn", (uint8_t)hCount);
  p.putBytes("plog", plog, sizeof plog);
  p.putUChar("pn", (uint8_t)pCount);
  p.end();
  Preferences q;
  q.begin("espradar-v4", false);
  q.putInt("tz", tzEast);
  q.putUChar("n0", night0);
  q.putUChar("n1", night1);
  q.end();
  prDirty = false;
  prSave = millis();
}

static void v4PresenceLoad() {
  prBoot = millis();
  Preferences p;
  p.begin("espradar-dev", true);
  int n = p.getUChar("hn", 0);
  if (n > HMAX) n = HMAX;
  if (n && p.getBytesLength("hlth") >= (size_t)n * sizeof(HRec)) {
    hCount = n;
    p.getBytes("hlth", hdb, hCount * sizeof(HRec));
  }
  int pn = p.getUChar("pn", 0);
  if (pn > 16) pn = 16;
  if (pn && p.getBytesLength("plog") >= sizeof(PLog) * (size_t)pn) {
    p.getBytes("plog", plog, sizeof plog);
    pCount = pn;
  }
  p.end();
  Preferences q;
  q.begin("espradar-v4", true);
  tzEast = q.getInt("tz", 0);
  night0 = q.getUChar("n0", 19);
  night1 = q.getUChar("n1", 7);
  q.end();
  uint32_t nowE = v4Epoch();
  for (int i = 0; i < hCount; i++) {
    hdb[i].alias[15] = 0;
    hInit[i] = 0;
    hWasOn[i] = 0;
    hHits[i] = 0;
    if ((hdb[i].flags & HF_LATCH) && nowE && hdb[i].lastSeenEpoch && nowE > hdb[i].lastSeenEpoch &&
        nowE - hdb[i].lastSeenEpoch > 12ul * 60ul) {
      hdb[i].flags = (uint8_t)(hdb[i].flags & ~HF_LATCH);
    }
  }
}

static void pPush(bool arrive, const uint8_t *mac, const char *name) {
  if (pCount == 16) { memmove(plog, plog + 1, sizeof(PLog) * 15); pCount = 15; }
  PLog &e = plog[pCount++];
  memset(&e, 0, sizeof e);
  e.ms = millis();
  e.epoch = v4Epoch();
  e.arrive = arrive ? 1 : 0;
  memcpy(e.mac, mac, 6);
  if (name) strncpy(e.name, name, 15);
  prDirty = true;
  Serial.printf("# presence %s %s\n", arrive ? "arrive" : "leave", e.name[0] ? e.name : "person");
}

static const char *hName(int i, int devIdx) {
  if (i >= 0 && hdb[i].alias[0]) return hdb[i].alias;
  if (devIdx >= 0 && devs[devIdx].name[0]) return devs[devIdx].name;
  if (i >= 0) {
    const char *v = vendorOf(hdb[i].mac);
    if (v && v[0] && strcmp(v, "Private (randomized) MAC") != 0) return v;
  }
  return "phone";
}

static int devIndex(const uint8_t *mac) {
  for (int k = 0; k < devCount; k++) if (!memcmp(devs[k].mac, mac, 6)) return k;
  return -1;
}

static bool ipForMac(const uint8_t *mac, IPAddress &out) {
  if (!staUp) return false;
  int k = devIndex(mac);
  if (k < 0 || !devs[k].ip4) return false;
  IPAddress me = WiFi.localIP();
  out = IPAddress(me[0], me[1], me[2], devs[k].ip4);
  return true;
}

static void v4SetClock(int eastMin, int n0, int n1, bool setNight) {
  if (eastMin < -14 * 60 || eastMin > 14 * 60) return;
  tzEast = eastMin;
  if (setNight) {
    night0 = (uint8_t)constrain(n0, 0, 23);
    night1 = (uint8_t)constrain(n1, 0, 23);
  }
  prDirty = true;
  v4PresenceSave(true);
}

static bool v4SetAlias(const uint8_t *mac, const char *alias) {
  int i = hEnsure(mac);
  if (i < 0) return false;
  strncpy(hdb[i].alias, alias ? alias : "", 15);
  hdb[i].alias[15] = 0;
  prDirty = true;
  v4PresenceSave(true);
  return true;
}

static int personCount() {
  int n = 0;
  for (int i = 0; i < hCount; i++) if (hdb[i].flags & HF_PERSON) n++;
  return n;
}

static bool v4SetPerson(const uint8_t *mac, bool on) {
  int i = hEnsure(mac);
  if (i < 0) return false;
  if (on && !(hdb[i].flags & HF_PERSON) && personCount() >= 8) return false;
  if (on) hdb[i].flags = (uint8_t)(hdb[i].flags | HF_PERSON);
  else hdb[i].flags = (uint8_t)(hdb[i].flags & ~(HF_PERSON | HF_LATCH));
  prDirty = true;
  v4PresenceSave(true);
  return true;
}

static void shiftHours(HRec &h, int steps, bool on) {
  uint32_t bits = (uint32_t)h.hours[0] | ((uint32_t)h.hours[1] << 8) | ((uint32_t)h.hours[2] << 16);
  for (int s = 0; s < steps && s < 24; s++) bits = ((bits << 1) | (on && s == steps - 1 ? 1u : 0u)) & 0xFFFFFFu;
  if (steps >= 24) bits = on ? 1u : 0u;
  h.hours[0] = bits & 0xff;
  h.hours[1] = (bits >> 8) & 0xff;
  h.hours[2] = (bits >> 16) & 0xff;
}

static void v4PresenceTick() {
  uint32_t now = millis();
  if (prBoot == 0) prBoot = now;
  if (now - prTick < 1000) return;
  prTick = now;
  if (!staUp) { homeWord = "none"; homeWho[0] = 0; return; }

  uint8_t macs[V4_WATCH][6];
  int wn = 0;
  auto pushm = [&](const uint8_t *m) {
    if (wn >= V4_WATCH || (m[0] & 1)) return;
    for (int i = 0; i < wn; i++) if (!memcmp(macs[i], m, 6)) return;
    memcpy(macs[wn], m, 6);
    wn++;
  };
  for (int i = 0; i < hCount; i++) if (hdb[i].flags & HF_PERSON) pushm(hdb[i].mac);
  for (int k = 0; k < devCount; k++) pushm(devs[k].mac);
  v4WatchSet(&macs[0][0], wn);

  if (!devScanning && now - prSweep >= 1000) {
    prSweep = now;
    IPAddress me = WiFi.localIP();
    for (int k = 0; k < 2; k++) {
      if (sweepIp == me[3]) sweepIp++;
      if (sweepIp > 254) sweepIp = 1;
      arpProbeIp(IPAddress(me[0], me[1], me[2], sweepIp));
      sweepIp++;
      if (sweepIp > 254) sweepIp = 1;
    }
    harvestArp();
  }
  if (now - prPeople >= 20000) {
    prPeople = now;
    for (int i = 0; i < hCount; i++) if (hdb[i].flags & HF_PERSON) {
      IPAddress ip;
      if (ipForMac(hdb[i].mac, ip)) arpProbeIp(ip);
    }
  }

  bool sniff = v4SnifferAlive();
  struct tm tmv;
  bool haveT = v4TimeOk(tmv);
  static int lastHour = -1;
  static int lastYday = -1;
  int hourStep = 0;
  if (haveT) {
    if (lastHour >= 0 && tmv.tm_hour != lastHour) {
      int diff = (tmv.tm_hour - lastHour + 24) % 24;
      hourStep = diff ? diff : 1;
    }
    lastHour = tmv.tm_hour;
  }

  int nPeople = 0, nHome = 0;
  homeWho[0] = 0;
  for (int i = 0; i < hCount; i++) {
    int di = devIndex(hdb[i].mac);
    uint32_t heard = v4WatchMs(hdb[i].mac);
    uint32_t devLast = di >= 0 ? devs[di].last : 0;
    bool radioFresh = heard && (now - heard) < V4_ONLINE_MS;
    bool arpFresh = devLast && (now - devLast) < V4_ONLINE_MS;
    bool online = sniff ? radioFresh : (radioFresh || arpFresh);
    if (!hInit[i]) { hWasOn[i] = online ? 1 : 0; hInit[i] = 1; }
    else if ((hWasOn[i] != 0) != online) {
      if (hdb[i].flips < 250) hdb[i].flips++;
      if (!online) {
        hdb[i].lastDropEpoch = v4Epoch();
        prDirty = true;
      }
      hWasOn[i] = online ? 1 : 0;
    }
    bool person = hdb[i].flags & HF_PERSON;
    if (person) {
      nPeople++;
      bool newRadio = heard && heard != hPrevMs[i];
      bool newArp = !sniff && devLast && devLast != hPrevDev[i];
      if (newRadio) hHits[i] = (uint8_t)min(250, hHits[i] + 1);
      if (newArp) hHits[i] = (uint8_t)min(250, hHits[i] + 1);
      if (heard) hPrevMs[i] = heard;
      if (devLast) hPrevDev[i] = devLast;
      bool latched = hdb[i].flags & HF_LATCH;
      uint32_t age = heard ? (now - heard) : UINT32_MAX;
      bool gone = sniff ? (!heard || age > V4_LEAVE_MS) : (!devLast || now - devLast > V4_LEAVE_MS);
      if ((newRadio || newArp) && hHits[i] >= 2 && !latched) {
        hdb[i].flags = (uint8_t)(hdb[i].flags | HF_LATCH);
        hdb[i].lastSeenEpoch = v4Epoch();
        pPush(true, hdb[i].mac, hName(i, di));
        latched = true;
      } else if (latched && (newRadio || newArp)) {
        hdb[i].lastSeenEpoch = v4Epoch();
      } else if (latched && gone) {
        hdb[i].flags = (uint8_t)(hdb[i].flags & ~HF_LATCH);
        hdb[i].lastDropEpoch = v4Epoch();
        pPush(false, hdb[i].mac, hName(i, di));
        latched = false;
        hHits[i] = 0;
      }
      if (!newRadio && !newArp && (!heard || age > 90000) && (!devLast || now - devLast > 90000)) hHits[i] = 0;
      if (latched) {
        nHome++;
        if (homeWho[0]) strncat(homeWho, ", ", sizeof homeWho - 1);
        strncat(homeWho, hName(i, di), sizeof homeWho - strlen(homeWho) - 1);
      }
    }
    if (hourStep) {
      shiftHours(hdb[i], hourStep, online);
      prDirty = true;
    } else if (online) {
      hdb[i].hours[0] = (uint8_t)(hdb[i].hours[0] | 0x01);
    }
    if (haveT && online && tmv.tm_hour < 6 && hdb[i].nightYday != (uint16_t)tmv.tm_yday) {
      if (hdb[i].nightN < 250) hdb[i].nightN++;
      hdb[i].nightYday = (uint16_t)tmv.tm_yday;
      prDirty = true;
    }
    (void)lastYday;
  }

  // per-minute uptime, using millis so it still works before NTP
  static uint32_t lastMinMark = 0;
  if (lastMinMark == 0) lastMinMark = now;
  if (now - lastMinMark >= 60000) {
    int steps = (int)((now - lastMinMark) / 60000);
    if (steps > 10) steps = 10;
    lastMinMark += (uint32_t)steps * 60000;
    for (int i = 0; i < hCount; i++) {
      bool on = hWasOn[i] && hInit[i];
      for (int s = 0; s < steps; s++) {
        if (hdb[i].totMin < 65535) hdb[i].totMin++;
        if (on && hdb[i].upMin < 65535) hdb[i].upMin++;
      }
    }
    prDirty = true;
  }

  if (nPeople == 0) homeWord = "none";
  else if (nHome > 0) homeWord = "home";
  else if (now - prBoot < 90000) homeWord = "checking";
  else homeWord = "away";

  if (now - prDecay >= 30ul * 60ul * 1000ul) {
    prDecay = now;
    for (int i = 0; i < hCount; i++) if (hdb[i].flips) { hdb[i].flips--; prDirty = true; }
  }

  // Friendlier automatic names for motion links (Devices-tab alias, else NetBIOS, else vendor).
  for (int slot = 1; slot < V4_LINKS; slot++) {
    if (!v4LinkNameAuto(slot)) continue;
    uint8_t mac[6];
    if (!v4LinkMac(slot, mac)) continue;
    int di = devIndex(mac);
    int hi = hFind(mac);
    const char *nm = nullptr;
    if (hi >= 0 && hdb[hi].alias[0]) nm = hdb[hi].alias;
    else if (di >= 0 && devs[di].name[0]) nm = devs[di].name;
    else {
      const char *v = vendorOf(mac);
      if (v && v[0] && strcmp(v, "Private (randomized) MAC") != 0) nm = v;
    }
    if (nm) v4SetAutoName(slot, nm);
  }

  static uint32_t lastRemember = 0;
  if (now - lastRemember > 10ul * 60ul * 1000ul) {
    lastRemember = now;
    bool ch = false;
    for (int k = 0; k < devCount && knownCount < 64; k++) {
      if (!(devs[k].mac[0] & 0x02) && !isKnown(devs[k].mac)) {
        memcpy(knownMacs[knownCount++], devs[k].mac, 6);
        ch = true;
      }
    }
    if (ch) saveKnown();
  }

  if (prDirty && now - prSave > 5ul * 60ul * 1000ul) v4PresenceSave(false);
}

static String v4DevExtra(int idx) {
  if (idx < 0 || idx >= devCount) return "";
  int i = hFind(devs[idx].mac);
  bool person = i >= 0 && (hdb[i].flags & HF_PERSON);
  bool online = i >= 0 && hInit[i] && hWasOn[i];
  int up = -1;
  if (i >= 0 && hdb[i].totMin >= 15) up = (int)((100ul * hdb[i].upMin) / hdb[i].totMin);
  int flap = i >= 0 ? hdb[i].flips : 0;
  int night = i >= 0 ? hdb[i].nightN : 0;
  uint32_t drop = i >= 0 ? hdb[i].lastDropEpoch : 0;
  uint32_t hours = 0;
  if (i >= 0) hours = (uint32_t)hdb[i].hours[0] | ((uint32_t)hdb[i].hours[1] << 8) | ((uint32_t)hdb[i].hours[2] << 16);
  const char *alias = i >= 0 ? hdb[i].alias : "";
  String o = ",\"alias\":\"";
  jsonAppendEsc(o, alias);
  o += "\",\"person\":" + String(person ? "true" : "false") + ",\"online\":" + String(online ? "true" : "false") +
       ",\"up\":" + String(up) + ",\"flap\":" + String(flap) + ",\"night\":" + String(night) +
       ",\"drop\":" + String(drop) + ",\"hours\":" + String(hours) +
       ",\"radio\":" + String(v4MacDirect(devs[idx].mac) ? "true" : "false");
  return o;
}

static String v4PresenceSuffix() {
  String o = ",\"home\":\"" + String(homeWord) + "\",\"who\":\"";
  jsonAppendEsc(o, homeWho);
  o += "\",\"deb\":12,\"time\":" + String(v4Epoch() ? "true" : "false") + ",\"tz\":" + String(tzEast) +
       ",\"night0\":" + String(night0) + ",\"night1\":" + String(night1);
  return o;
}

static String v4PresenceJson() {
  String o = "{\"home\":\"" + String(homeWord) + "\",\"who\":\"";
  jsonAppendEsc(o, homeWho);
  o += "\",\"debounceMin\":12,\"tz\":" + String(tzEast) + ",\"night0\":" + String((int)night0) +
       ",\"night1\":" + String((int)night1) + ",\"time\":" + String(v4Epoch() ? "true" : "false") + ",\"log\":[";
  for (int i = pCount - 1; i >= 0; i--) {
    if (i != pCount - 1) o += ",";
    o += "{\"t\":" + String(plog[i].ms) + ",\"epoch\":" + String(plog[i].epoch) + ",\"arrive\":" +
         String(plog[i].arrive ? "true" : "false") + ",\"mac\":\"" + macStr(plog[i].mac) + "\",\"name\":\"";
    jsonAppendEsc(o, plog[i].name);
    o += "\"}";
  }
  return o + "]}";
}
