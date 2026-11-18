/*
  ESP Radar - WiFi CSI motion/presence radar for the Heltec WiFi LoRa 32 V4
  (ESP32-S3 + SSD1306/SSD1315 128x64 OLED).

  What it does: watches how the WiFi signal from your router "wobbles"
  (Channel State Information, per-subcarrier amplitude) and turns that into a
  0-100 motion/presence intensity. It is NOT a camera and does NOT locate
  individual people - the radar blips are a visualisation of intensity.

  MIT License - see LICENSE.
*/
#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <Preferences.h>
#include <Wire.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SSD1306Wire.h>
#include "esp_wifi.h"
#include "ping/ping_sock.h"
#include "lwip/ip_addr.h"
#include "web_ui.h"

#define FW_VERSION "1.0.0"

// ---------- Heltec WiFi LoRa 32 V4 pins (Heltec pins_arduino.h + Meshtastic variant) ----------
static const int PIN_VEXT = 36;   // Vext power switch, ACTIVE LOW (powers the OLED)
static const int PIN_OLED_SDA = 17;
static const int PIN_OLED_SCL = 18;
static const int PIN_OLED_RST = 21;
static const int PIN_PRG = 0;     // PRG / BOOT button, active low
static const int PIN_LED = 35;    // white LED

// ---------- tuning ----------
static const int   WIN = 48;                 // sliding window (packets)
static const uint32_t CAL_MS = 10000;        // calibration time
static const int   PING_INTERVAL_MS = 20;    // ~50 pings/s to the gateway -> CSI flow
static const char *AP_PASS = "espradar";     // setup AP password (WPA2)

// ---------- globals ----------
SSD1306Wire display(0x3c, PIN_OLED_SDA, PIN_OLED_SCL);
AsyncWebServer server(80);
AsyncEventSource events("/events");
DNSServer dns;
Preferences prefs;

String staSsid, staPass, apName, staIp = "", lastErr = "";
bool setupMode = false;      // no credentials saved
bool staUp = false;
bool mdnsUp = false;
volatile bool reqRecal = false;
volatile bool reqRestart = false;
uint32_t restartAt = 0;
int sensitivity = 5;         // 1..10

// CSI plumbing
struct CsiPkt { int8_t buf[128]; uint16_t len; int8_t rssi; bool fwi; };
static QueueHandle_t csiQ;
static volatile uint8_t apBssid[6];
static volatile bool bssidValid = false;
static esp_ping_handle_t pingH = nullptr;

// detector state (written by csi task, read by others)
static portMUX_TYPE stMux = portMUX_INITIALIZER_UNLOCKED;
struct State {
  float level = 0;        // 0..100 smoothed motion level
  float prox = 0;         // 0..100 rough "closeness" of the disturbance
  bool motion = false;
  bool calibrating = true;
  float calProgress = 0;  // 0..1
  float score = 0, baseline = 0, sigma = 0;
  float rssi = -100;
  float pps = 0;
  uint32_t packets = 0;
} st;

// ------------------------------------------------------------------------------------
// CSI callback (WiFi task context: keep it tiny)
static void csiCb(void *ctx, wifi_csi_info_t *info) {
  if (!info || !info->buf || info->len < 64) return;
  if (!bssidValid) return;
  for (int i = 0; i < 6; i++) if (info->mac[i] != apBssid[i]) return;   // only frames from our AP
  CsiPkt p;
  p.len = info->len > 128 ? 128 : info->len;
  memcpy(p.buf, info->buf, p.len);
  p.rssi = info->rx_ctrl.rssi;
  p.fwi = info->first_word_invalid;
  xQueueSend(csiQ, &p, 0);
}

// LLTF: 64 subcarriers, (imag, real) int8 pairs. index 0..31 = sc 0..31, 32..63 = sc -32..-1
// usable: 2..26 and 38..63 (skip DC, guard band, first word which may be invalid)
static inline bool usableSc(int k) { return (k >= 2 && k <= 26) || (k >= 38 && k <= 63); }
static const int NSC = 51;

static float ring[WIN][NSC];
static float rsum[NSC], rsq[NSC];
static int ringPos = 0, ringFill = 0;
static float rssiRing[WIN]; static float rssiSum = 0, rssiSq = 0;

static void csiTask(void *) {
  CsiPkt p;
  float amp[NSC];
  // calibration accumulators
  uint32_t calStart = 0; double cSum = 0, cSq = 0; uint32_t cN = 0; double cRssi = 0;
  float rssiBase = -60;
  float lvl = 0, prox = 0; uint32_t lastAbove = 0; bool motion = false;
  uint32_t ppsCount = 0, ppsT = millis(); float pps = 0;
  uint32_t lastEval = 0; float scoreEma = 0;
  bool calibrating = true;
  float base = 0, sig = 1;
  for (;;) {
    bool got = xQueueReceive(csiQ, &p, pdMS_TO_TICKS(100)) == pdTRUE;
    uint32_t now = millis();
    if (reqRecal) {
      reqRecal = false; calibrating = true; calStart = 0; cSum = cSq = 0; cN = 0; cRssi = 0;
      ringFill = 0; ringPos = 0; memset(rsum, 0, sizeof rsum); memset(rsq, 0, sizeof rsq);
      rssiSum = rssiSq = 0; lvl = 0; motion = false;
    }
    if (got) {
      ppsCount++;
      // amplitudes, normalised by mean amplitude (cancels AGC / gain jumps)
      int j = 0; float mean = 0;
      for (int k = 0; k < 64 && 2 * k + 1 < p.len; k++) {
        if (!usableSc(k)) continue;
        float im = p.buf[2 * k], re = p.buf[2 * k + 1];
        amp[j] = sqrtf(re * re + im * im); mean += amp[j]; j++;
      }
      if (j == NSC && mean > 1.0f) {
        mean /= NSC;
        float *slot = ring[ringPos];
        bool full = ringFill == WIN;
        for (int i = 0; i < NSC; i++) {
          float a = amp[i] / mean;
          if (full) { rsum[i] -= slot[i]; rsq[i] -= slot[i] * slot[i]; }
          slot[i] = a; rsum[i] += a; rsq[i] += a * a;
        }
        if (full) { rssiSum -= rssiRing[ringPos]; rssiSq -= rssiRing[ringPos] * rssiRing[ringPos]; }
        rssiRing[ringPos] = p.rssi; rssiSum += p.rssi; rssiSq += (float)p.rssi * p.rssi;
        ringPos = (ringPos + 1) % WIN; if (!full) ringFill++;
      }
    }
    if (now - ppsT >= 1000) { pps = ppsCount * 1000.0f / (now - ppsT); ppsCount = 0; ppsT = now; }

    // evaluate at ~10 Hz
    if (now - lastEval < 100) continue;
    lastEval = now;
    float score = 0, rssiMean = -100, rssiStd = 0;
    if (ringFill >= WIN / 2) {
      for (int i = 0; i < NSC; i++) {
        float m = rsum[i] / ringFill; float v = rsq[i] / ringFill - m * m; if (v > 0) score += v;
      }
      score = score / NSC * 1000.0f;   // mean per-subcarrier variance (x1000)
      rssiMean = rssiSum / ringFill;
      float rv = rssiSq / ringFill - rssiMean * rssiMean; rssiStd = rv > 0 ? sqrtf(rv) : 0;
      scoreEma = scoreEma == 0 ? score : scoreEma * 0.6f + score * 0.4f;
    }
    float calProg = 0;
    if (calibrating) {
      if (ringFill >= WIN / 2) {
        if (calStart == 0) calStart = now;
        cSum += scoreEma; cSq += (double)scoreEma * scoreEma; cN++; cRssi += rssiMean;
        calProg = min(1.0f, (now - calStart) / (float)CAL_MS);
        if (now - calStart >= CAL_MS && cN >= 20) {
          base = cSum / cN; double var = cSq / cN - (double)base * base; sig = var > 0 ? sqrt(var) : 0;
          rssiBase = cRssi / cN; calibrating = false; lvl = 0;
          Serial.printf("# calibrated: baseline=%.3f sigma=%.3f rssi=%.1f samples=%lu\n", base, sig, rssiBase, (unsigned long)cN);
        }
      }
    } else if (ringFill >= WIN / 2) {
      float spread = max(sig, max(base * 0.10f, 0.02f));
      float z = (scoreEma - base) / spread;
      float gain = 1.2f + sensitivity * 0.6f;               // sens 5 -> 4.2 per sigma
      float target = constrain(z * gain, 0.0f, 100.0f);
      lvl = lvl * 0.7f + target * 0.3f;
      // rough proximity: strong disturbance + RSSI wobble/drop => "closer"
      float rssiTerm = constrain(rssiStd * 18.0f + (rssiBase - rssiMean) * 4.0f, 0.0f, 100.0f);
      float pt = lvl < 3 ? 0 : constrain(lvl * 0.75f + rssiTerm * 0.25f, 0.0f, 100.0f);
      prox = prox * 0.8f + pt * 0.2f;
      if (lvl >= 25) { motion = true; lastAbove = now; }
      else if (motion && lvl < 12 && now - lastAbove > 2000) motion = false;
      else if (lvl >= 12) lastAbove = now;
      // slow baseline drift tracking while the room is still
      if (!motion && lvl < 8) { base = base * 0.998f + scoreEma * 0.002f; rssiBase = rssiBase * 0.995f + rssiMean * 0.005f; }
    }
    portENTER_CRITICAL(&stMux);
    st.level = calibrating ? 0 : lvl; st.prox = calibrating ? 0 : prox; st.motion = calibrating ? false : motion;
    st.calibrating = calibrating; st.calProgress = calProg; st.score = scoreEma; st.baseline = base; st.sigma = sig;
    st.rssi = rssiMean; st.pps = pps; st.packets++;
    portEXIT_CRITICAL(&stMux);
  }
}

static State snap() { State s; portENTER_CRITICAL(&stMux); s = st; portEXIT_CRITICAL(&stMux); return s; }

// ------------------------------------------------------------------------------------
// traffic generator: ICMP echo to the gateway so the router keeps sending us frames
static void startPing() {
  if (pingH) return;
  IPAddress gw = WiFi.gatewayIP();
  if (gw == IPAddress(0, 0, 0, 0)) return;
  esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
  ip_addr_t target; IP_ADDR4(&target, gw[0], gw[1], gw[2], gw[3]);
  cfg.target_addr = target;
  cfg.count = ESP_PING_COUNT_INFINITE;
  cfg.interval_ms = PING_INTERVAL_MS;
  cfg.timeout_ms = 1000;
  cfg.data_size = 16;
  cfg.task_stack_size = 3072;
  esp_ping_callbacks_t cbs = {};
  if (esp_ping_new_session(&cfg, &cbs, &pingH) == ESP_OK) esp_ping_start(pingH);
  else pingH = nullptr;
}
static void stopPing() { if (pingH) { esp_ping_stop(pingH); esp_ping_delete_session(pingH); pingH = nullptr; } }

static void startCsi() {
  wifi_csi_config_t c = {};
  c.lltf_en = true; c.htltf_en = false; c.stbc_htltf2_en = false; c.ltf_merge_en = true;
  c.channel_filter_en = false; c.manu_scale = false; c.shift = 0;
  esp_wifi_set_csi_config(&c);
  esp_wifi_set_csi_rx_cb(csiCb, nullptr);
  esp_wifi_set_csi(true);
}

// ------------------------------------------------------------------------------------
// motion event log (for the web UI)
struct Ev { uint32_t ms; bool start; uint8_t peak; };
static Ev evlog[20]; static int evCount = 0;
static void logEvent(bool start, uint8_t peak) {
  if (evCount == 20) { memmove(evlog, evlog + 1, sizeof(Ev) * 19); evCount = 19; }
  evlog[evCount++] = { millis(), start, peak };
}

static String jsonEsc(const String &s) {
  String o; for (char c : s) { if (c == '"' || c == '\\') { o += '\\'; o += c; } else if ((uint8_t)c >= 0x20) o += c; } return o;
}

static String stateJson() {
  State s = snap();
  char b[520];
  snprintf(b, sizeof b,
    "{\"m\":%.1f,\"p\":%.1f,\"s\":\"%s\",\"cal\":%s,\"cp\":%.2f,\"rssi\":%.1f,\"pps\":%.0f,"
    "\"score\":%.3f,\"base\":%.3f,\"sig\":%.3f,\"sens\":%d,\"ip\":\"%s\",\"ssid\":\"%s\",\"ap\":\"%s\","
    "\"up\":%lu,\"setup\":%s,\"sta\":%s,\"v\":\"" FW_VERSION "\"}",
    s.level, s.prox, s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"),
    s.calibrating ? "true" : "false", s.calProgress, s.rssi, s.pps, s.score, s.baseline, s.sigma,
    sensitivity, staIp.c_str(), jsonEsc(staSsid).c_str(), apName.c_str(), (unsigned long)(millis() / 1000),
    setupMode ? "true" : "false", staUp ? "true" : "false");
  return String(b);
}

static String eventsJson() {
  String o = "{\"now\":" + String(millis()) + ",\"ev\":[";
  for (int i = 0; i < evCount; i++) {
    if (i) o += ",";
    o += "{\"t\":" + String(evlog[i].ms) + ",\"start\":" + (evlog[i].start ? "true" : "false") + ",\"peak\":" + String(evlog[i].peak) + "}";
  }
  return o + "]}";
}


// ------------------------------------------------------------------------------------
// web server
static void sendHtml(AsyncWebServerRequest *r, const char *h) {
  r->send(200, "text/html", (const uint8_t *)h, strlen(h));
}

static void setupWeb() {
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (setupMode) { r->redirect("/setup"); return; }
    sendHtml(r, RADAR_HTML);
  });
  server.on("/radar", HTTP_GET, [](AsyncWebServerRequest *r) { sendHtml(r, RADAR_HTML); });
  server.on("/setup", HTTP_GET, [](AsyncWebServerRequest *r) { sendHtml(r, SETUP_HTML); });
  server.on("/api/state", HTTP_GET, [](AsyncWebServerRequest *r) { r->send(200, "application/json", stateJson()); });
  server.on("/api/events", HTTP_GET, [](AsyncWebServerRequest *r) { r->send(200, "application/json", eventsJson()); });
  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); r->send(202, "application/json", "{\"scanning\":true,\"nets\":[]}"); return; }
    if (n == WIFI_SCAN_RUNNING) { r->send(202, "application/json", "{\"scanning\":true,\"nets\":[]}"); return; }
    String o = "{\"scanning\":false,\"nets\":[";
    for (int i = 0; i < n && i < 25; i++) {
      if (i) o += ",";
      o += "{\"ssid\":\"" + jsonEsc(WiFi.SSID(i)) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"open\":" + (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    o += "]}";
    WiFi.scanDelete(); WiFi.scanNetworks(true);   // refresh for next call
    r->send(200, "application/json", o);
  });
  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!r->hasParam("ssid", true)) { r->send(400, "text/plain", "missing ssid"); return; }
    String s = r->getParam("ssid", true)->value();
    String p = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    if (s.length() == 0 || s.length() > 32 || p.length() > 64) { r->send(400, "text/plain", "bad ssid/password length"); return; }
    prefs.putString("ssid", s); prefs.putString("pass", p);
    r->send(200, "application/json", "{\"ok\":true}");
    reqRestart = true; restartAt = millis() + 1500;
  });
  server.on("/api/recal", HTTP_POST, [](AsyncWebServerRequest *r) { reqRecal = true; r->send(200, "application/json", "{\"ok\":true}"); });
  server.on("/api/sens", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (r->hasParam("v", true)) { sensitivity = constrain(r->getParam("v", true)->value().toInt(), 1, 10); prefs.putInt("sens", sensitivity); }
    r->send(200, "application/json", "{\"sens\":" + String(sensitivity) + "}");
  });
  server.on("/api/forget", HTTP_POST, [](AsyncWebServerRequest *r) {
    prefs.remove("ssid"); prefs.remove("pass");
    r->send(200, "application/json", "{\"ok\":true}");
    reqRestart = true; restartAt = millis() + 1500;
  });
  // captive-portal style: any unknown URL goes to the setup page (setup mode) or radar
  server.onNotFound([](AsyncWebServerRequest *r) { r->redirect(setupMode ? "http://192.168.4.1/setup" : "/"); });
  events.onConnect([](AsyncEventSourceClient *c) { c->send(stateJson().c_str(), "state", millis(), 1000); });
  server.addHandler(&events);
  server.begin();
}

// ------------------------------------------------------------------------------------
// OLED
static float sweep = 0;
struct Blip { float ang, rad; float life; };
static Blip blips[10];
static float lastSweep = 0;

static void oledInit() {
  pinMode(PIN_VEXT, OUTPUT); digitalWrite(PIN_VEXT, LOW);  // power on OLED (active low)
  delay(50);
  pinMode(PIN_OLED_RST, OUTPUT); digitalWrite(PIN_OLED_RST, LOW); delay(20); digitalWrite(PIN_OLED_RST, HIGH); delay(20);
  display.init();
  display.flipScreenVertically();
  display.setContrast(255);
  display.setFont(ArialMT_Plain_10);
}

static void drawRadar(int cx, int cy, int R, const State &s, float dt) {
  display.drawCircle(cx, cy, R);
  display.drawCircle(cx, cy, R * 2 / 3);
  display.drawCircle(cx, cy, R / 3);
  for (int x = cx - R; x <= cx + R; x += 3) display.setPixel(x, cy);
  for (int y = cy - R; y <= cy + R; y += 3) display.setPixel(cx, y);
  // sweep + phosphor trail
  float speed = 2.6f + s.level * 0.03f;   // rad/s, faster with motion
  lastSweep = sweep; sweep += speed * dt; if (sweep > TWO_PI) sweep -= TWO_PI;
  for (int t = 0; t < 6; t++) {
    float a = sweep - t * 0.09f;
    int len = R - t * 2;
    if (t == 0) display.drawLine(cx, cy, cx + cosf(a) * R, cy + sinf(a) * R);
    else for (int r = 4; r < len; r += 1 + t) display.setPixel(cx + cosf(a) * r, cy + sinf(a) * r);
  }
  // spawn blips as the sweep passes, count/intensity from motion level, radius from proximity
  if (!s.calibrating && s.level > 4) {
    int want = 1 + (int)(s.level / 16);
    for (int i = 0; i < want; i++) {
      if (random(100) > s.level + 10) continue;
      for (auto &b : blips) if (b.life <= 0) {
        float base = 0.92f - s.prox / 100.0f * 0.7f;           // closer => nearer centre
        b.rad = constrain(base + (random(-15, 16) / 100.0f), 0.12f, 0.95f) * R;
        b.ang = sweep - random(0, 30) / 100.0f; b.life = 1.0f; break;
      }
    }
  }
  for (auto &b : blips) {
    if (b.life <= 0) continue;
    int x = cx + cosf(b.ang) * b.rad, y = cy + sinf(b.ang) * b.rad;
    if (b.life > 0.66f) display.fillCircle(x, y, 2);
    else if (b.life > 0.33f) display.drawCircle(x, y, 1);
    else if (((int)(millis() / 90)) & 1) display.setPixel(x, y);
    b.life -= dt * 0.55f;
  }
}

static void drawOled(float dt) {
  State s = snap();
  display.clear();
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.setFont(ArialMT_Plain_10);
  if (setupMode) {
    drawRadar(100, 40, 22, s, dt);
    display.drawString(0, 0, "SETUP MODE");
    display.drawString(0, 14, "Join WiFi:");
    display.drawString(0, 26, apName);
    display.drawString(0, 38, String("pw: ") + AP_PASS);
    display.drawString(0, 52, "192.168.4.1");
    display.display(); return;
  }
  // status line: IP + motion %
  String ip = staUp ? staIp : String("connecting...");
  display.drawString(0, 0, ip);
  display.setTextAlignment(TEXT_ALIGN_RIGHT);
  display.drawString(128, 0, s.calibrating ? String("CAL") : String((int)s.level) + "%");
  display.setTextAlignment(TEXT_ALIGN_LEFT);
  display.drawHorizontalLine(0, 12, 128);
  drawRadar(26, 38, 24, s, dt);
  int px = 58;
  if (!staUp) {
    display.drawString(px, 16, "WiFi...");
    display.drawString(px, 28, lastErr.length() ? lastErr : String("joining"));
    display.drawString(px, 40, "AP:" + apName.substring(10));
    display.drawString(px, 52, "192.168.4.1");
  } else if (s.calibrating) {
    display.drawString(px, 16, "CALIBRATING");
    display.drawString(px, 28, "keep still...");
    display.drawProgressBar(px, 44, 68, 8, (uint8_t)(s.calProgress * 100));
    display.drawString(px, 53, String((int)s.pps) + " pkt/s");
  } else {
    display.setFont(ArialMT_Plain_16);
    display.drawString(px, 14, s.motion ? "MOTION" : "STILL");
    display.setFont(ArialMT_Plain_10);
    display.drawProgressBar(px, 34, 68, 8, (uint8_t)s.level);
    display.drawString(px, 44, "near " + String((int)s.prox) + "%");
    display.drawString(px, 53, s.pps < 5 ? String("low CSI!") : String((int)s.rssi) + "dBm " + String((int)s.pps) + "/s");
  }
  display.display();
}

// ------------------------------------------------------------------------------------
static void startAp() {
  WiFi.softAPConfig(IPAddress(192, 168, 4, 1), IPAddress(192, 168, 4, 1), IPAddress(255, 255, 255, 0));
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
  display.setFont(ArialMT_Plain_10); display.drawString(10, 34, "WiFi CSI motion sensor"); display.drawString(10, 48, "v" FW_VERSION); display.display();

  prefs.begin("espradar", false);
  staSsid = prefs.getString("ssid", ""); staPass = prefs.getString("pass", "");
  sensitivity = prefs.getInt("sens", 5);
  setupMode = staSsid.length() == 0;

  uint8_t mac[6]; WiFi.macAddress(mac);
  char nm[24]; snprintf(nm, sizeof nm, "ESP-Radar-%02X%02X", mac[4], mac[5]); apName = nm;

  csiQ = xQueueCreate(32, sizeof(CsiPkt));
  xTaskCreatePinnedToCore(csiTask, "csi", 6144, nullptr, 3, nullptr, 0);

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP_STA);
  WiFi.setSleep(false);                // no modem sleep: steady CSI
  startAp();
  if (setupMode) {
    dns.start(53, "*", IPAddress(192, 168, 4, 1));
    WiFi.scanNetworks(true);
  } else {
    WiFi.setHostname("esp-radar");
    WiFi.begin(staSsid.c_str(), staPass.c_str());
  }
  startCsi();
  setupWeb();
  delay(600);
  Serial.println("# ESP Radar " FW_VERSION " | AP " + apName + " (pw " + AP_PASS + ") at 192.168.4.1");
  Serial.println("ms,level,state,proximity,rssi,pps,score,baseline,sigma");
}

void loop() {
  static uint32_t lastFrame = millis(), lastSse = 0, lastCsv = 0, lastRetry = 0, staStart = millis();
  static bool prevMotion = false; static float peak = 0;
  uint32_t now = millis();
  if (setupMode) dns.processNextRequest();
  handleButton();
  if (reqRestart && now > restartAt) ESP.restart();

  // STA management
  if (!setupMode) {
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !staUp) {
      staUp = true; staIp = WiFi.localIP().toString(); lastErr = "";
      uint8_t *b = WiFi.BSSID(); for (int i = 0; i < 6; i++) apBssid[i] = b[i]; bssidValid = true;
      if (!mdnsUp && MDNS.begin("esp-radar")) { MDNS.addService("http", "tcp", 80); mdnsUp = true; }
      startPing(); reqRecal = true;
      Serial.printf("# WiFi up: %s ip=%s gw=%s ch=%d -> http://esp-radar.local\n", staSsid.c_str(), staIp.c_str(), WiFi.gatewayIP().toString().c_str(), (int)WiFi.channel());
    } else if (!up && staUp) {
      staUp = false; bssidValid = false; stopPing(); staIp = ""; lastErr = "lost link"; staStart = now;
      Serial.println("# WiFi lost, reconnecting");
    }
    if (!up && now - staStart > 20000 && now - lastRetry > 15000) {
      lastRetry = now; lastErr = "can't join"; WiFi.disconnect(); WiFi.begin(staSsid.c_str(), staPass.c_str());
    }
  }

  State s = snap();
  // motion events + LED
  if (!s.calibrating) {
    if (s.motion) peak = max(peak, s.level);
    if (s.motion != prevMotion) { logEvent(s.motion, (uint8_t)peak); if (!s.motion) peak = 0; prevMotion = s.motion; }
  }
  digitalWrite(PIN_LED, s.motion ? ((now / 120) & 1) : LOW);

  if (now - lastFrame >= 33) { float dt = (now - lastFrame) / 1000.0f; lastFrame = now; drawOled(dt); }
  if (now - lastSse >= 125) { lastSse = now; if (events.count()) events.send(stateJson().c_str(), "state", now); }
  if (now - lastCsv >= 100) {
    lastCsv = now;
    if (staUp) Serial.printf("%lu,%.1f,%s,%.1f,%.1f,%.0f,%.3f,%.3f,%.3f\n", (unsigned long)now, s.level,
      s.calibrating ? "calibrating" : (s.motion ? "motion" : "still"), s.prox, s.rssi, s.pps, s.score, s.baseline, s.sigma);
  }
  delay(2);
}
