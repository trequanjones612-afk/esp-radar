// ESP Radar v4 — multi-device motion paths.
//
// v3 measured CSI only on frames whose transmitter MAC is the associated AP (the router
// answers our pings, so those frames are addressed to the board). Espressif's ESP32-S3
// CSI guide says a station otherwise only receives the AP, and that promiscuous / sniffer
// mode is how you get CSI from other transmitters. This file does that.
//
// What a link means: the radio path between that transmitter and this board, on the
// 2.4 GHz channel the board is joined to. It is not a position and not a person count.
// Devices that stay on 5 GHz (common on an eero mesh) never transmit on this channel,
// so they cannot be links. Untested on Heltec V4 hardware — the API matches IDF 5.5 /
// arduino-esp32 3.3.13, which is what v3's working router CSI uses.
//
// If a chosen device is heard but its CSI buffer is missing or too slow (<5 frames/s),
// that link falls back to RSSI variance. The UI labels it as the weaker signal.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <Preferences.h>
#include <esp_wifi.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// Defined in the sketch before this file is included.
static const char *vendorOf(const uint8_t *m);

static const int V4_LINKS = 6;          // 0 = router, 1..5 = other transmitters
static const int V4_WIN = 50;           // same moving-variance window as v3
static const int V4_NSC = 51;           // usable LLTF subcarriers (skips the invalid first word)
static const int V4_TALK = 16;
static const int V4_WATCH = 48;
static const int V4_Q = 256;            // int16 amplitude scale (1/256). Quantization is far below CSI noise.

struct State {
  float level = 0, prox = 0;
  bool motion = false, calibrating = true;
  float calProgress = 0;
  float score = 0;
  float rssi = -100, pps = 0, ppsAll = 0, pingRate = 0;
  uint32_t evals = 0;
};

struct LinkView {
  float level = 0, score = 0, pps = 0, rssi = -100, calProgress = 0;
  bool motion = false, calibrating = true, active = false;
  uint8_t src = 0;          // 0 none, 1 csi, 2 rssi
  char st[8] = "wait";
  char name[20] = "";
  char room[16] = "";
  uint8_t mac[6] = {0};
  bool nameAuto = true;
};

struct V4Talk {
  uint8_t mac[6];
  uint16_t hits;
  int8_t rssi;
};

static State st;
static portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
static State snap() { State s; portENTER_CRITICAL(&stMux); s = st; portEXIT_CRITICAL(&stMux); return s; }

static volatile uint8_t apBssid[6];
static volatile bool bssidValid = false;
static volatile uint32_t csiAll = 0, csiOurs = 0, csiDrop = 0, pingOk = 0;
static volatile uint32_t sniffPkts = 0;
static volatile bool sniffOn = false;
static bool sniffUser = true;             // user can turn the extra-device sniffer off; router CSI stays

static inline void v4OnPingOk() { pingOk = pingOk + 1; }

struct CsiPkt { int8_t buf[128]; uint16_t len; int8_t rssi; uint8_t link; };
static QueueHandle_t csiQ;

struct LinkCfg {
  uint8_t mac[6];
  char name[20];
  char room[16];
  uint8_t used;
  uint8_t locked;       // user picked this slot; auto-pick won't replace it
  uint8_t nameAuto;     // name was generated; a friendlier alias may replace it
};

struct LinkRT {
  int16_t ring[V4_WIN][V4_NSC];
  float rsum[V4_NSC], rsq[V4_NSC], emaM[V4_NSC], emaD[V4_NSC], baseVar[V4_NSC], calVar[V4_NSC];
  float rssiRing[V4_WIN];
  float rssiSum, rssiSq, rssiBase, rssiBaseStd, calRssi, calRssiStd;
  float level, prox, scoreEma, calProgress, pps, rssi;
  int ringPos, ringFill, roPos, roFill;
  int8_t rssiOnly[24];
  uint32_t calStart, calN, calRssiN, lastAbove, csiCount, rssiCount;
  bool emaInit, calibrating, motion, active;
  uint8_t src;
};

static LinkCfg linkCfg[V4_LINKS];
static LinkRT rt[V4_LINKS];
static LinkView pub[V4_LINKS];
static portMUX_TYPE radioMux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t cfgGen = 1, appliedGen = 0;
static volatile int recalWhich = -2;     // -2 none, -1 all, else one link
static uint8_t selfMac[6];
static bool selfValid = false;

struct WatchSlot { uint8_t mac[6]; uint32_t ms; uint8_t direct; uint8_t used; };
static WatchSlot watch[V4_WATCH];
static int watchN = 0;
static V4Talk talkers[V4_TALK];

// One bucket per minute for the router path (the same hour chart v3 drew).
struct V4Bucket { uint8_t avg, peak, motionPct; };
static V4Bucket act[60];
static int actCount = 0;
static float bSum = 0;
static uint16_t bN = 0, bMot = 0;
static uint8_t bPeak = 0;
static uint32_t bMinute = 0;

static int cmpf(const void *a, const void *b) {
  float x = *(const float *)a, y = *(const float *)b;
  return x < y ? -1 : x > y;
}
static bool macEq(const uint8_t *a, const uint8_t *b) { return memcmp(a, b, 6) == 0; }
static bool macGroup(const uint8_t *m) { return (m[0] & 1) || (m[0] == 0 && m[1] == 0 && m[2] == 0 && m[3] == 0 && m[4] == 0 && m[5] == 0); }
static bool macSelf(const uint8_t *m) { return selfValid && macEq(m, selfMac); }
static bool macAp(const uint8_t *m) {
  if (!bssidValid) return false;
  uint8_t b[6];
  for (int i = 0; i < 6; i++) b[i] = apBssid[i];
  return macEq(m, b);
}

static void resetLink(int i) {
  LinkRT &L = rt[i];
  memset(L.ring, 0, sizeof L.ring);
  memset(L.rsum, 0, sizeof L.rsum);
  memset(L.rsq, 0, sizeof L.rsq);
  memset(L.calVar, 0, sizeof L.calVar);
  memset(L.rssiOnly, 0, sizeof L.rssiOnly);
  L.rssiSum = L.rssiSq = 0;
  L.ringPos = L.ringFill = L.roPos = L.roFill = 0;
  L.emaInit = false;
  L.calibrating = true;
  L.calStart = 0;
  L.calN = L.calRssiN = 0;
  L.calRssi = L.calRssiStd = 0;
  L.level = L.prox = 0;
  L.scoreEma = 1;
  L.calProgress = 0;
  L.motion = false;
  L.src = 0;
  L.lastAbove = 0;
  L.rssiBase = -60;
  L.rssiBaseStd = 0.8f;
  L.rssi = -100;
}

static void radioSee(const uint8_t *mac, int8_t rssi, bool direct) {
  if (!mac || macGroup(mac) || macSelf(mac)) return;
  uint32_t now = millis();
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < watchN; i++) {
    if (watch[i].used && macEq(watch[i].mac, mac)) {
      watch[i].ms = now;
      if (direct) watch[i].direct = 1;
      break;
    }
  }
  if (direct && !macAp(mac)) {
    int slot = -1, weakest = 0;
    for (int i = 0; i < V4_TALK; i++) {
      if (talkers[i].hits && macEq(talkers[i].mac, mac)) { slot = i; break; }
      if (!talkers[i].hits) { slot = i; break; }
      if (talkers[i].hits < talkers[weakest].hits) weakest = i;
    }
    if (slot < 0) slot = weakest;
    if (!talkers[slot].hits || macEq(talkers[slot].mac, mac) || talkers[slot].hits < 8) {
      if (!macEq(talkers[slot].mac, mac)) { memset(&talkers[slot], 0, sizeof(V4Talk)); memcpy(talkers[slot].mac, mac, 6); }
      if (talkers[slot].hits < 60000) talkers[slot].hits++;
      talkers[slot].rssi = rssi;
    }
  }
  int link = -1;
  if (macAp(mac)) link = 0;
  else if (direct) {
    for (int i = 1; i < V4_LINKS; i++) if (linkCfg[i].used && macEq(linkCfg[i].mac, mac)) { link = i; break; }
  }
  if (link >= 0) {
    LinkRT &L = rt[link];
    L.rssiOnly[L.roPos] = rssi;
    L.roPos = (L.roPos + 1) % 24;
    if (L.roFill < 24) L.roFill++;
    L.rssiCount++;
    L.rssi = rssi;
  }
  portEXIT_CRITICAL(&radioMux);
}

static int matchLink(const uint8_t *mac) {
  int found = -1;
  portENTER_CRITICAL(&radioMux);
  if (macAp(mac)) found = 0;
  else {
    for (int i = 1; i < V4_LINKS; i++) if (linkCfg[i].used && macEq(linkCfg[i].mac, mac)) { found = i; break; }
  }
  portEXIT_CRITICAL(&radioMux);
  return found;
}

static void csiCb(void *ctx, wifi_csi_info_t *info) {
  (void)ctx;
  csiAll = csiAll + 1;
  if (!csiQ || !info || !info->buf || !bssidValid) return;
  int link = matchLink(info->mac);
  if (link < 0) return;
  if (info->len < 128) return;                 // not a full LLTF; RSSI for this MAC is counted in the sniffer
  // Keep the queue for real CSI. When the AP is busy, shed extra router frames so other links still fit.
  if (link == 0 && uxQueueMessagesWaiting(csiQ) > 40) { csiDrop = csiDrop + 1; return; }
  CsiPkt p;
  memcpy(p.buf, info->buf, 128);
  p.len = 128;
  p.rssi = info->rx_ctrl.rssi;
  p.link = (uint8_t)link;
  if (xQueueSend(csiQ, &p, 0) != pdTRUE) csiDrop = csiDrop + 1;
  else if (link == 0) csiOurs = csiOurs + 1;
}

static void promiscCb(void *buf, wifi_promiscuous_pkt_type_t type) {
  if (type != WIFI_PKT_DATA) return;
  const wifi_promiscuous_pkt_t *pp = (const wifi_promiscuous_pkt_t *)buf;
  if (pp->rx_ctrl.sig_len < 24 || pp->rx_ctrl.rx_state != 0) return;
  const uint8_t *h = pp->payload;
  if (((h[0] >> 2) & 0x3) != 2) return;        // data
  bool toDs = h[1] & 0x01, fromDs = h[1] & 0x02;
  sniffPkts = sniffPkts + 1;
  int8_t rssi = pp->rx_ctrl.rssi;
  if (toDs && !fromDs) radioSee(h + 10, rssi, true);          // STA → AP: we hear that device's radio
  else if (fromDs && !toDs) radioSee(h + 16, rssi, false);    // AP → STA: source is addr3 (presence, not a CSI path)
}

static inline bool usableSc(int k) { return (k >= 2 && k <= 26) || (k >= 38 && k <= 63); }

static void ingest(int link, const int8_t *buf, int8_t rssi) {
  LinkRT &L = rt[link];
  if (!L.active && link != 0) return;
  float amp[V4_NSC];
  int j = 0;
  float mean = 0;
  for (int k = 0; k < 64; k++) {
    if (!usableSc(k)) continue;
    float im = buf[2 * k], re = buf[2 * k + 1];
    amp[j] = sqrtf(re * re + im * im);
    mean += amp[j];
    j++;
  }
  if (j != V4_NSC || mean <= 1.0f) return;
  mean /= V4_NSC;
  if (!L.emaInit) {
    for (int i = 0; i < V4_NSC; i++) { L.emaM[i] = amp[i] / mean; L.emaD[i] = 0.05f * L.emaM[i] + 1e-3f; }
    L.emaInit = true;
  }
  bool full = L.ringFill == V4_WIN;
  for (int i = 0; i < V4_NSC; i++) {
    float a = amp[i] / mean;
    float dev = a - L.emaM[i];
    float lim = 3.0f * 1.4826f * L.emaD[i] + 1e-3f;
    if (dev > lim) a = L.emaM[i] + lim;
    else if (dev < -lim) a = L.emaM[i] - lim;
    L.emaD[i] = L.emaD[i] * 0.97f + fabsf(a - L.emaM[i]) * 0.03f;
    L.emaM[i] = L.emaM[i] * 0.98f + a * 0.02f;
    int q = (int)lrintf(fminf(fmaxf(a, -30.f), 30.f) * V4_Q);
    if (q > 32767) q = 32767;
    if (q < -32768) q = -32768;
    float aq = q / (float)V4_Q;
    if (full) {
      float old = L.ring[L.ringPos][i] / (float)V4_Q;
      L.rsum[i] -= old;
      L.rsq[i] -= old * old;
    }
    L.ring[L.ringPos][i] = (int16_t)q;
    L.rsum[i] += aq;
    L.rsq[i] += aq * aq;
  }
  if (full) { L.rssiSum -= L.rssiRing[L.ringPos]; L.rssiSq -= L.rssiRing[L.ringPos] * L.rssiRing[L.ringPos]; }
  L.rssiRing[L.ringPos] = rssi;
  L.rssiSum += rssi;
  L.rssiSq += (float)rssi * rssi;
  L.ringPos = (L.ringPos + 1) % V4_WIN;
  if (!full) L.ringFill++;
  L.csiCount++;
  L.rssi = rssi;
}

static void evalLink(int idx, uint32_t now) {
  LinkRT &L = rt[idx];
  float var[V4_NSC], ratio[V4_NSC];
  bool ready = L.ringFill >= V4_WIN / 2;
  float rssiMean = -100, rssiStd = 0;
  if (ready) {
    for (int i = 0; i < V4_NSC; i++) {
      float m = L.rsum[i] / L.ringFill;
      float v = L.rsq[i] / L.ringFill - m * m;
      var[i] = v > 0 ? v : 0;
    }
    rssiMean = L.rssiSum / L.ringFill;
    float rv = L.rssiSq / L.ringFill - rssiMean * rssiMean;
    rssiStd = rv > 0 ? sqrtf(rv) : 0;
  }
  int roN = 0;
  int8_t ro[24];
  portENTER_CRITICAL(&radioMux);
  roN = L.roFill;
  for (int i = 0; i < roN && i < 24; i++) ro[i] = L.rssiOnly[i];
  portEXIT_CRITICAL(&radioMux);
  float roStd = 0, roMean = -100;
  if (roN >= 8) {
    float s = 0;
    for (int i = 0; i < roN; i++) s += ro[i];
    roMean = s / roN;
    float v = 0;
    for (int i = 0; i < roN; i++) { float d = ro[i] - roMean; v += d * d; }
    roStd = sqrtf(v / roN);
  }
  if (L.calibrating) {
    if (L.calStart == 0 && (ready || roN >= 8)) L.calStart = now;
    if (ready) { for (int i = 0; i < V4_NSC; i++) L.calVar[i] += var[i]; L.calN++; L.calRssi += rssiMean; }
    if (roN >= 8) { L.calRssiStd += roStd; L.calRssiN++; }
    L.calProgress = L.calStart ? min(1.0f, (now - L.calStart) / (float)CAL_MS) : 0;
    bool csiOk = L.calN >= 30;
    bool rssiOk = L.calRssiN >= 20;
    if (L.calStart && now - L.calStart >= CAL_MS && (csiOk || rssiOk)) {
      if (csiOk) {
        float tmp[V4_NSC];
        for (int i = 0; i < V4_NSC; i++) { L.baseVar[i] = L.calVar[i] / L.calN; tmp[i] = L.baseVar[i]; }
        qsort(tmp, V4_NSC, sizeof(float), cmpf);
        float floorV = max(tmp[V4_NSC / 2] * 0.25f, 1e-6f);
        for (int i = 0; i < V4_NSC; i++) L.baseVar[i] = max(L.baseVar[i], floorV);
        L.rssiBase = L.calRssi / L.calN;
        Serial.printf("# calibrated link %d (%s) csi median var=%.6f samples=%lu pps=%.0f\n",
                      idx, pub[idx].name[0] ? pub[idx].name : "?", tmp[V4_NSC / 2], (unsigned long)L.calN, L.pps);
      }
      if (rssiOk) L.rssiBaseStd = max(0.35f, L.calRssiStd / L.calRssiN);
      L.calibrating = false;
      L.level = 0;
      L.scoreEma = 1;
      L.src = csiOk ? 1 : 2;
    }
    return;
  }
  float span = 1.0f + (11 - sensitivity) * 0.8f;
  bool useCsi = ready && L.pps >= 5 && L.baseVar[0] > 0;
  if (useCsi) {
    for (int i = 0; i < V4_NSC; i++) ratio[i] = var[i] / L.baseVar[i];
    qsort(ratio, V4_NSC, sizeof(float), cmpf);
    float score = ratio[(V4_NSC * 3) / 4];
    L.scoreEma = L.scoreEma * 0.6f + score * 0.4f;
    float target = constrain(100.0f * (L.scoreEma - 1.0f) / span, 0.0f, 100.0f);
    L.level = L.level * 0.7f + target * 0.3f;
    float rssiTerm = constrain(rssiStd * 18.0f + (L.rssiBase - rssiMean) * 4.0f, 0.0f, 100.0f);
    float pt = L.level < 3 ? 0 : constrain(L.level * 0.75f + rssiTerm * 0.25f, 0.0f, 100.0f);
    L.prox = L.prox * 0.8f + pt * 0.2f;
    L.src = 1;
    if (!L.motion && L.level < 8) {
      for (int i = 0; i < V4_NSC; i++) L.baseVar[i] = L.baseVar[i] * 0.998f + max(var[i], 1e-7f) * 0.002f;
      L.rssiBase = L.rssiBase * 0.995f + rssiMean * 0.005f;
    }
  } else if (roN >= 12 && L.rssiBaseStd > 0 && L.pps < 5) {
    // Weaker motion signal: how much this transmitter's RSSI is wandering vs its quiet baseline.
    float score = roStd / L.rssiBaseStd;
    float spanR = span * 1.8f;                 // RSSI jitter trips more easily; keep the same 25% word conservative
    L.scoreEma = L.scoreEma * 0.6f + score * 0.4f;
    float target = constrain(100.0f * (L.scoreEma - 1.0f) / spanR, 0.0f, 100.0f);
    L.level = L.level * 0.7f + target * 0.3f;
    L.prox = L.prox * 0.8f + L.level * 0.2f;
    L.src = 2;
    L.rssi = roMean;
  } else {
    L.level = L.level * 0.8f;
    if (L.level < 1) L.level = 0;
    L.src = 0;
  }
  if (L.src) {
    if (L.level >= 25) { L.motion = true; L.lastAbove = now; }
    else if (L.motion && L.level < 12 && now - L.lastAbove > 2000) L.motion = false;
    else if (L.level >= 12) L.lastAbove = now;
  } else L.motion = false;
}

static const char *linkState(const LinkRT &L) {
  if (!L.active) return "off";
  if (L.csiCount == 0 && L.rssiCount < 8 && L.calStart == 0) return "wait";
  if (L.calibrating) return "cal";
  if (L.src == 0) return "low";
  return L.motion ? "motion" : "still";
}

static void publish(float ppsAll, float pingRate) {
  LinkView next[V4_LINKS];
  for (int i = 0; i < V4_LINKS; i++) {
    LinkView &v = next[i];
    const LinkRT &L = rt[i];
    v.level = L.calibrating ? 0 : L.level;
    v.score = L.calibrating ? 0 : L.scoreEma;
    v.pps = L.pps;
    v.rssi = L.rssi;
    v.calProgress = L.calProgress;
    v.motion = L.calibrating ? false : L.motion;
    v.calibrating = L.calibrating;
    v.active = L.active;
    v.src = L.src;
    strncpy(v.st, linkState(L), sizeof v.st - 1);
    portENTER_CRITICAL(&radioMux);
    memcpy(v.mac, linkCfg[i].mac, 6);
    strncpy(v.name, linkCfg[i].name, sizeof v.name - 1);
    strncpy(v.room, linkCfg[i].room, sizeof v.room - 1);
    v.nameAuto = linkCfg[i].nameAuto;
    if (i == 0) v.active = bssidValid || L.active;
    portEXIT_CRITICAL(&radioMux);
    v.name[sizeof v.name - 1] = 0;
    v.room[sizeof v.room - 1] = 0;
    v.st[sizeof v.st - 1] = 0;
  }
  portENTER_CRITICAL(&stMux);
  // st stays the router path so the CSV and the original meter fields keep their v3 meaning.
  const LinkRT &R = rt[0];
  st.level = R.calibrating || !R.active ? 0 : R.level;
  st.prox = R.calibrating || !R.active ? 0 : R.prox;
  st.motion = R.calibrating || !R.active ? false : R.motion;
  st.calibrating = !R.active || R.calibrating;
  st.calProgress = R.active ? R.calProgress : 0;
  st.score = R.calibrating ? 0 : R.scoreEma;
  st.rssi = R.rssi;
  st.pps = R.pps;
  st.ppsAll = ppsAll;
  st.pingRate = pingRate;
  st.evals++;
  for (int i = 0; i < V4_LINKS; i++) pub[i] = next[i];
  portEXIT_CRITICAL(&stMux);
}

static void csiTask(void *) {
  CsiPkt p;
  uint32_t lastEval = 0, ppsT = millis(), lastOurs = 0, lastAll = 0, lastPing = 0;
  uint32_t lastCsi[V4_LINKS] = {0}, lastRssi[V4_LINKS] = {0};
  float ppsAll = 0, pingRate = 0;
  for (;;) {
    bool got = xQueueReceive(csiQ, &p, pdMS_TO_TICKS(50)) == pdTRUE;
    uint32_t now = millis();
    if (reqRecal) { reqRecal = false; recalWhich = -1; }
    int which = recalWhich;
    if (which != -2) {
      recalWhich = -2;
      if (which < 0) { for (int i = 0; i < V4_LINKS; i++) if (rt[i].active || i == 0) resetLink(i); }
      else if (which < V4_LINKS) resetLink(which);
    }
    if (appliedGen != cfgGen) {
      LinkCfg cfg[V4_LINKS];
      uint32_t g;
      portENTER_CRITICAL(&radioMux);
      memcpy(cfg, linkCfg, sizeof cfg);
      g = cfgGen;
      portEXIT_CRITICAL(&radioMux);
      for (int i = 0; i < V4_LINKS; i++) {
        bool on = cfg[i].used || (i == 0 && bssidValid);
        if (on && !rt[i].active) resetLink(i);
        if (!on && i != 0) { rt[i].active = false; rt[i].motion = false; rt[i].level = 0; }
        else rt[i].active = on;
      }
      appliedGen = g;
    }
    if (got && p.link < V4_LINKS) ingest(p.link, p.buf, p.rssi);
    if (now - ppsT >= 1000) {
      float dt = (now - ppsT) / 1000.0f;
      uint32_t o = csiOurs, a = csiAll, pg = pingOk;
      ppsAll = (a - lastAll) / dt;
      pingRate = (pg - lastPing) / dt;
      lastOurs = o;
      lastAll = a;
      lastPing = pg;
      ppsT = now;
      for (int i = 0; i < V4_LINKS; i++) {
        uint32_t c = rt[i].csiCount, r = rt[i].rssiCount;
        // Prefer CSI rate. RSSI frames/s is only a fallback indicator when CSI is idle.
        float csiPps = (c - lastCsi[i]) / dt;
        float rssiPps = (r - lastRssi[i]) / dt;
        rt[i].pps = csiPps >= 1 ? csiPps : rssiPps;
        lastCsi[i] = c;
        lastRssi[i] = r;
      }
      (void)lastOurs;
    }
    if (now - lastEval < 100) continue;
    lastEval = now;
    if (bssidValid) rt[0].active = true;
    for (int i = 0; i < V4_LINKS; i++) if (rt[i].active) evalLink(i, now);
    uint32_t minute = now / 60000;
    if (minute != bMinute) {
      if (bN) {
        if (actCount == 60) { memmove(act, act + 1, sizeof(V4Bucket) * 59); actCount = 59; }
        act[actCount++] = { (uint8_t)(bSum / bN), bPeak, (uint8_t)(100 * bMot / bN) };
      }
      bSum = 0; bN = 0; bMot = 0; bPeak = 0; bMinute = minute;
    }
    if (rt[0].active && !rt[0].calibrating) {
      bSum += rt[0].level;
      bN++;
      if (rt[0].motion) bMot++;
      if (rt[0].level > bPeak) bPeak = (uint8_t)rt[0].level;
    }
    publish(ppsAll, pingRate);
  }
}

static void v4SaveLinks() {
  LinkCfg copy[V4_LINKS];
  portENTER_CRITICAL(&radioMux);
  memcpy(copy, linkCfg, sizeof copy);
  portEXIT_CRITICAL(&radioMux);
  Preferences p;
  p.begin("espradar-v4", false);
  p.putBytes("links", copy, sizeof copy);
  p.putUChar("sniff", sniffUser ? 1 : 0);
  p.end();
}

static void v4LoadLinks() {
  Preferences p;
  p.begin("espradar-v4", true);
  if (p.getBytesLength("links") == sizeof linkCfg) p.getBytes("links", linkCfg, sizeof linkCfg);
  sniffUser = p.getUChar("sniff", 1) != 0;
  p.end();
  // Slot 0 is always "the AP we're joined to". Keep a custom name, drop a stale BSSID until association.
  memset(linkCfg[0].mac, 0, 6);
  linkCfg[0].used = 0;
  linkCfg[0].locked = 1;
  if (!linkCfg[0].name[0]) strncpy(linkCfg[0].name, "Router", sizeof linkCfg[0].name - 1);
  for (int i = 0; i < V4_LINKS; i++) {
    linkCfg[i].name[sizeof linkCfg[i].name - 1] = 0;
    linkCfg[i].room[sizeof linkCfg[i].room - 1] = 0;
  }
  cfgGen = cfgGen + 1;
}

static void v4SetSelf(const uint8_t *m) { memcpy(selfMac, m, 6); selfValid = true; }

static void v4ApplySniffer(bool enable);

static void v4ClearBssid() {
  portENTER_CRITICAL(&radioMux);
  bssidValid = false;
  linkCfg[0].used = 0;
  cfgGen = cfgGen + 1;
  portEXIT_CRITICAL(&radioMux);
  v4ApplySniffer(false);
}

static void v4SetBssid(const uint8_t *b) {
  bool diff = !bssidValid;
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < 6; i++) if (apBssid[i] != b[i]) diff = true;
  for (int i = 0; i < 6; i++) apBssid[i] = b[i];
  bssidValid = true;
  memcpy(linkCfg[0].mac, b, 6);
  linkCfg[0].used = 1;
  if (!linkCfg[0].name[0]) strncpy(linkCfg[0].name, "Router", sizeof linkCfg[0].name - 1);
  cfgGen = cfgGen + 1;
  portEXIT_CRITICAL(&radioMux);
  if (diff) { recalWhich = 0; }     // a new mesh node is a different radio; re-baseline that path only
}

static void v4ApplySniffer(bool enable) {
  if (!enable || !sniffUser) {
    esp_wifi_set_promiscuous(false);
    sniffOn = false;
    return;
  }
  wifi_promiscuous_filter_t f = {};
  f.filter_mask = WIFI_PROMIS_FILTER_MASK_DATA;
  esp_wifi_set_promiscuous_filter(&f);
  esp_wifi_set_promiscuous_rx_cb(promiscCb);
  esp_err_t e = esp_wifi_set_promiscuous(true);
  sniffOn = e == ESP_OK;
  Serial.printf("# sniffer %s (%d) — CSI from other transmitters needs this. Untested on this board.\n", sniffOn ? "on" : "FAILED", (int)e);
}

static void v4SniffUser(bool on) {
  sniffUser = on;
  v4SaveLinks();
  v4ApplySniffer(on);
}

static void startCsi() {
  wifi_csi_config_t c = {};
  c.lltf_en = true;
  c.htltf_en = true;
  c.stbc_htltf2_en = true;
  c.ltf_merge_en = true;
  c.channel_filter_en = false;
  c.manu_scale = false;
  c.shift = 0;
  c.dump_ack_en = false;           // keep the router path on data frames, same as v3
  esp_wifi_set_csi_config(&c);
  esp_wifi_set_csi_rx_cb(csiCb, nullptr);
  esp_wifi_set_csi(true);
}

static void v4Start() {
  csiQ = xQueueCreate(64, sizeof(CsiPkt));
  xTaskCreatePinnedToCore(csiTask, "csi", 10240, nullptr, 3, nullptr, 0);
}

static bool v4View(int i, LinkView &out) {
  if (i < 0 || i >= V4_LINKS) return false;
  portENTER_CRITICAL(&stMux);
  out = pub[i];
  portEXIT_CRITICAL(&stMux);
  return true;
}

static bool v4AnyMotion() {
  bool m = false;
  portENTER_CRITICAL(&stMux);
  for (int i = 0; i < V4_LINKS; i++) if (pub[i].active && pub[i].motion) m = true;
  portEXIT_CRITICAL(&stMux);
  return m;
}

static bool v4LinkMotion(int i) {
  if (i < 0 || i >= V4_LINKS) return false;
  portENTER_CRITICAL(&stMux);
  bool m = pub[i].active && pub[i].motion;
  portEXIT_CRITICAL(&stMux);
  return m;
}

static bool v4RoomMotion(const char *room) {
  if (!room || !room[0]) return false;
  bool m = false;
  portENTER_CRITICAL(&stMux);
  for (int i = 0; i < V4_LINKS; i++) if (pub[i].active && pub[i].motion && pub[i].room[0] && strcmp(pub[i].room, room) == 0) m = true;
  portEXIT_CRITICAL(&stMux);
  return m;
}

// Loudest active path (prefers one that is currently in motion). Router if nothing else is alive.
static bool v4Loud(LinkView &out) {
  portENTER_CRITICAL(&stMux);
  int best = 0;
  float bestL = -1;
  for (int i = 0; i < V4_LINKS; i++) {
    if (!pub[i].active || pub[i].calibrating) continue;
    if (strcmp(pub[i].st, "wait") == 0 || strcmp(pub[i].st, "low") == 0) continue;
    float w = pub[i].level + (pub[i].motion ? 1000 : 0);
    if (w > bestL) { bestL = w; best = i; }
  }
  out = pub[best];
  portEXIT_CRITICAL(&stMux);
  return true;
}

static void v4RequestRecal(int which) { recalWhich = which; }

static int v4MotionCount() {
  int n = 0;
  portENTER_CRITICAL(&stMux);
  for (int i = 0; i < V4_LINKS; i++) if (pub[i].active && pub[i].motion) n++;
  portEXIT_CRITICAL(&stMux);
  return n;
}

static bool v4NextProbe(uint8_t mac[6]) {
  static int rot = 1;
  portENTER_CRITICAL(&radioMux);
  for (int k = 0; k < V4_LINKS - 1; k++) {
    int i = 1 + ((rot + k) % (V4_LINKS - 1));
    if (linkCfg[i].used) {
      memcpy(mac, linkCfg[i].mac, 6);
      rot = i % (V4_LINKS - 1) + 1;
      portEXIT_CRITICAL(&radioMux);
      return true;
    }
  }
  portEXIT_CRITICAL(&radioMux);
  return false;
}

static void v4WatchSet(const uint8_t *macs, int n) {
  if (n > V4_WATCH) n = V4_WATCH;
  portENTER_CRITICAL(&radioMux);
  WatchSlot keep[V4_WATCH];
  int kn = watchN;
  memcpy(keep, watch, sizeof keep);
  watchN = n;
  for (int i = 0; i < n; i++) {
    memcpy(watch[i].mac, macs + i * 6, 6);
    watch[i].used = 1;
    watch[i].ms = 0;
    watch[i].direct = 0;
    for (int k = 0; k < kn; k++) if (keep[k].used && macEq(keep[k].mac, watch[i].mac)) {
      watch[i].ms = keep[k].ms;
      watch[i].direct = keep[k].direct;
      break;
    }
  }
  portEXIT_CRITICAL(&radioMux);
}

// ms since this MAC was heard on the radio, or UINT32_MAX if never.
static uint32_t v4WatchMs(const uint8_t *mac) {
  uint32_t ms = 0;
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < watchN; i++) if (watch[i].used && macEq(watch[i].mac, mac)) { ms = watch[i].ms; break; }
  portEXIT_CRITICAL(&radioMux);
  return ms;
}

static uint32_t v4WatchAge(const uint8_t *mac) {
  uint32_t ms = 0;
  bool hit = false;
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < watchN; i++) if (watch[i].used && macEq(watch[i].mac, mac) && watch[i].ms) { ms = watch[i].ms; hit = true; break; }
  portEXIT_CRITICAL(&radioMux);
  if (!hit) return UINT32_MAX;
  return millis() - ms;
}

static bool v4MacDirect(const uint8_t *mac) {
  bool d = false;
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < watchN; i++) if (watch[i].used && macEq(watch[i].mac, mac)) { d = watch[i].direct; break; }
  portEXIT_CRITICAL(&radioMux);
  return d;
}

static uint32_t sniffPrev = 0, sniffSeenMs = 0;
static bool v4SnifferAlive() {
  uint32_t c = sniffPkts;
  if (c != sniffPrev) { sniffPrev = c; sniffSeenMs = millis(); }
  return sniffOn && sniffSeenMs && (millis() - sniffSeenMs < 20000);
}
static bool v4SnifferOn() { return sniffOn; }
static bool v4SnifferWanted() { return sniffUser; }

static void v4DecayTalkers() {
  portENTER_CRITICAL(&radioMux);
  for (int i = 0; i < V4_TALK; i++) {
    talkers[i].hits = (uint16_t)(talkers[i].hits * 3 / 4);
    if (talkers[i].hits == 0) memset(&talkers[i], 0, sizeof(V4Talk));
  }
  portEXIT_CRITICAL(&radioMux);
}

static bool v4LinkMac(int i, uint8_t mac[6]) {
  if (i < 0 || i >= V4_LINKS) return false;
  bool used;
  portENTER_CRITICAL(&radioMux);
  used = linkCfg[i].used;
  memcpy(mac, linkCfg[i].mac, 6);
  portEXIT_CRITICAL(&radioMux);
  return used;
}
static bool v4LinkNameAuto(int i) {
  if (i <= 0 || i >= V4_LINKS) return false;
  portENTER_CRITICAL(&radioMux);
  bool a = linkCfg[i].used && linkCfg[i].nameAuto;
  portEXIT_CRITICAL(&radioMux);
  return a;
}
static void v4SetAutoName(int i, const char *name) {
  if (i <= 0 || i >= V4_LINKS || !name || !name[0]) return;
  portENTER_CRITICAL(&radioMux);
  if (linkCfg[i].used && linkCfg[i].nameAuto) {
    strncpy(linkCfg[i].name, name, sizeof linkCfg[i].name - 1);
    linkCfg[i].name[sizeof linkCfg[i].name - 1] = 0;
  }
  portEXIT_CRITICAL(&radioMux);
  cfgGen = cfgGen + 1;
}

static bool parseMacStr(const char *s, uint8_t *m) {
  if (!s) return false;
  int n = 0;
  for (int i = 0; i < 6; i++) {
    while (s[n] == ':' || s[n] == '-' || s[n] == ' ') n++;
    if (!isxdigit((unsigned char)s[n]) || !isxdigit((unsigned char)s[n + 1])) return false;
    char b[3] = { s[n], s[n + 1], 0 };
    m[i] = (uint8_t)strtol(b, nullptr, 16);
    n += 2;
  }
  return !(m[0] & 1);
}

static void v4LinkClear(int slot) {
  if (slot < 1 || slot >= V4_LINKS) return;
  portENTER_CRITICAL(&radioMux);
  memset(&linkCfg[slot], 0, sizeof(LinkCfg));
  cfgGen = cfgGen + 1;
  portEXIT_CRITICAL(&radioMux);
  v4SaveLinks();
}

static bool v4LinkSet(int slot, const uint8_t mac[6], const char *name, const char *room, bool lock) {
  if (slot == 0) {
    portENTER_CRITICAL(&radioMux);
    if (name && name[0]) {
      strncpy(linkCfg[0].name, name, sizeof linkCfg[0].name - 1);
      linkCfg[0].name[sizeof linkCfg[0].name - 1] = 0;
      linkCfg[0].nameAuto = 0;
    }
    if (room) {
      strncpy(linkCfg[0].room, room, sizeof linkCfg[0].room - 1);
      linkCfg[0].room[sizeof linkCfg[0].room - 1] = 0;
    }
    cfgGen = cfgGen + 1;
    portEXIT_CRITICAL(&radioMux);
    v4SaveLinks();
    return true;
  }
  if (slot < 1 || slot >= V4_LINKS || !mac || (mac[0] & 1)) return false;
  portENTER_CRITICAL(&radioMux);
  for (int i = 1; i < V4_LINKS; i++) if (i != slot && linkCfg[i].used && macEq(linkCfg[i].mac, mac)) {
    portEXIT_CRITICAL(&radioMux);
    return false;
  }
  memcpy(linkCfg[slot].mac, mac, 6);
  linkCfg[slot].used = 1;
  linkCfg[slot].locked = lock ? 1 : 0;
  if (name && name[0]) {
    strncpy(linkCfg[slot].name, name, sizeof linkCfg[slot].name - 1);
    linkCfg[slot].name[sizeof linkCfg[slot].name - 1] = 0;
    linkCfg[slot].nameAuto = 0;
  } else if (!linkCfg[slot].name[0]) {
    snprintf(linkCfg[slot].name, sizeof linkCfg[slot].name, "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    linkCfg[slot].nameAuto = 1;
  }
  if (room) {
    strncpy(linkCfg[slot].room, room, sizeof linkCfg[slot].room - 1);
    linkCfg[slot].room[sizeof linkCfg[slot].room - 1] = 0;
  }
  cfgGen = cfgGen + 1;
  portEXIT_CRITICAL(&radioMux);
  v4SaveLinks();
  return true;
}

static int v4AutoPick() {
  V4Talk copy[V4_TALK];
  portENTER_CRITICAL(&radioMux);
  memcpy(copy, talkers, sizeof copy);
  portEXIT_CRITICAL(&radioMux);
  int order[V4_TALK];
  int n = 0;
  for (int i = 0; i < V4_TALK; i++) if (copy[i].hits >= 3 && !(copy[i].mac[0] & 0x02) && !macSelf(copy[i].mac) && !macAp(copy[i].mac)) order[n++] = i;
  for (int a = 0; a < n; a++) for (int b = a + 1; b < n; b++) {
    int ia = order[a], ib = order[b];
    int sa = (int)copy[ia].hits * (copy[ia].rssi > -85 ? 2 : 1);
    int sb = (int)copy[ib].hits * (copy[ib].rssi > -85 ? 2 : 1);
    if (sb > sa) { int t = order[a]; order[a] = order[b]; order[b] = t; }
  }
  int filled = 0;
  int ti = 0;
  for (int slot = 1; slot < V4_LINKS && ti < n; slot++) {
    portENTER_CRITICAL(&radioMux);
    bool locked = linkCfg[slot].locked && linkCfg[slot].used;
    portEXIT_CRITICAL(&radioMux);
    if (locked) continue;
    const uint8_t *mac = copy[order[ti++]].mac;
    const char *vend = vendorOf(mac);
    char name[20];
    if (vend && vend[0] && strcmp(vend, "Private (randomized) MAC") != 0) strncpy(name, vend, sizeof name - 1), name[sizeof name - 1] = 0;
    else snprintf(name, sizeof name, "%02X:%02X:%02X", mac[3], mac[4], mac[5]);
    // v4LinkSet forces nameAuto=0 when a name is passed. Mark auto after.
    if (v4LinkSet(slot, mac, name, "", false)) {
      portENTER_CRITICAL(&radioMux);
      linkCfg[slot].nameAuto = 1;
      linkCfg[slot].locked = 0;
      portEXIT_CRITICAL(&radioMux);
      filled++;
    }
  }
  v4SaveLinks();
  cfgGen = cfgGen + 1;
  return filled;
}

static void jsonAppendEsc(String &o, const char *s) {
  for (const unsigned char *p = (const unsigned char *)(s ? s : ""); *p; p++) {
    if (*p == '"' || *p == '\\') { o += '\\'; o += (char)*p; }
    else if (*p >= 0x20) o += (char)*p;
  }
}
static String macColon(const uint8_t *m) {
  char b[18];
  snprintf(b, sizeof b, "%02X:%02X:%02X:%02X:%02X:%02X", m[0], m[1], m[2], m[3], m[4], m[5]);
  return String(b);
}

static String v4LinksJsonArray() {
  String o = "[";
  for (int i = 0; i < V4_LINKS; i++) {
    LinkView v;
    v4View(i, v);
    if (i) o += ",";
    o += "{\"i\":" + String(i) + ",\"mac\":\"" + (v.active ? macColon(v.mac) : String("")) + "\",\"name\":\"";
    jsonAppendEsc(o, v.name);
    o += "\",\"room\":\"";
    jsonAppendEsc(o, v.room);
    o += "\",\"m\":" + String(v.level, 1) + ",\"sc\":" + String(v.score, 2) + ",\"pps\":" + String(v.pps, 0) +
         ",\"rssi\":" + String(v.rssi, 0) + ",\"s\":\"" + String(v.st) + "\",\"src\":\"" +
         String(v.src == 1 ? "csi" : (v.src == 2 ? "rssi" : "none")) + "\",\"on\":" + String(v.active ? "true" : "false") + "}";
  }
  return o + "]";
}

static String v4MotionSuffix() {
  LinkView loud;
  v4Loud(loud);
  String o = ",\"sniff\":" + String(sniffOn ? "true" : "false") + ",\"sniffWant\":" + String(sniffUser ? "true" : "false") +
             ",\"loud\":\"";
  jsonAppendEsc(o, loud.name);
  o += "\",\"loudm\":" + String(loud.level, 1) + ",\"louds\":\"" + String(loud.st) + "\",\"links\":" + v4LinksJsonArray();
  return o;
}

static String v4TalkersJson() {
  V4Talk copy[V4_TALK];
  portENTER_CRITICAL(&radioMux);
  memcpy(copy, talkers, sizeof copy);
  portEXIT_CRITICAL(&radioMux);
  String o = "{\"sniff\":" + String(sniffOn ? "true" : "false") + ",\"talkers\":[";
  bool first = true;
  for (int i = 0; i < V4_TALK; i++) {
    if (!copy[i].hits) continue;
    if (!first) o += ",";
    first = false;
    const char *vend = vendorOf(copy[i].mac);
    o += "{\"mac\":\"" + macColon(copy[i].mac) + "\",\"hits\":" + String(copy[i].hits) + ",\"rssi\":" + String((int)copy[i].rssi) +
         ",\"vendor\":\"";
    jsonAppendEsc(o, vend ? vend : "");
    o += "\"}";
  }
  o += "],\"links\":" + v4LinksJsonArray() + "}";
  return o;
}

static String activityJson() {
  String o = "{\"now\":" + String(millis()) + ",\"minuteStart\":" + String((millis() / 60000) * 60000) + ",\"cur\":{\"avg\":" +
             String(bN ? (int)(bSum / bN) : 0) + ",\"peak\":" + String(bPeak) + ",\"mot\":" + String(bN ? (int)(100 * bMot / bN) : 0) + "},\"b\":[";
  for (int i = 0; i < actCount; i++) {
    if (i) o += ",";
    o += "[" + String(act[i].avg) + "," + String(act[i].peak) + "," + String(act[i].motionPct) + "]";
  }
  return o + "]}";
}
