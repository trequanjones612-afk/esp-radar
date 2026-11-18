// Buffers that are not touched from the WiFi ISR live in PSRAM.
// DMA, the CSI queue, and the promiscuous callback's small MAC tables stay internal.
#pragma once
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <stdarg.h>

static uint32_t heapBoot = 0, heapScanMin = 0, heapPageMin = 0;

static void heapNote(uint32_t &slot) {
  uint32_t f = ESP.getFreeHeap();
  if (!slot || f < slot) slot = f;
}

static void *psAlloc(size_t n, const char *what) {
  void *p = nullptr;
  const char *where = "PSRAM";
  if (psramFound()) p = heap_caps_calloc(1, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!p) {
    p = heap_caps_calloc(1, n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    where = "INTERNAL";
  }
  Serial.printf("# mem %s %u bytes %s\n", what ? what : "buf", (unsigned)n, p ? where : "FAILED");
  return p;
}

struct JBuf {
  char *p = nullptr;
  int n = 0;
  int cap = 0;
  void begin() {
    n = 0;
    if (p && cap) p[0] = 0;
  }
  void add(const char *s) {
    if (!p || !s) return;
    while (*s && n + 1 < cap) p[n++] = *s++;
    p[n] = 0;
  }
  void addf(const char *fmt, ...) {
    if (!p || n + 1 >= cap) return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(p + n, (size_t)(cap - n), fmt, ap);
    va_end(ap);
    if (w < 0) return;
    if (w >= cap - n) n = cap - 1;
    else n += w;
  }
  void esc(const char *s) {
    if (!s) return;
    for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
      if (n + 2 >= cap) break;
      if (*c == '"' || *c == '\\') p[n++] = '\\';
      if (*c >= 0x20) p[n++] = (char)*c;
    }
    if (p) p[n] = 0;
  }
  const char *c_str() const { return p ? p : ""; }
};

static JBuf JB;
static SemaphoreHandle_t jbMux = nullptr;

static void jbInit() {
  if (!JB.p) JB.p = (char *)psAlloc(20 * 1024, "json");
  if (JB.p) JB.cap = 20 * 1024;
  if (!jbMux) jbMux = xSemaphoreCreateMutex();
}
static bool jbTake() {
  if (!JB.p) return false;
  if (!jbMux) return true;
  return xSemaphoreTake(jbMux, pdMS_TO_TICKS(250)) == pdTRUE;
}
static void jbGive() {
  if (jbMux) xSemaphoreGive(jbMux);
}

// Survives esp_restart so the next boot can say why. Not valid after power-on.
RTC_NOINIT_ATTR static uint32_t rstMagic;
RTC_NOINIT_ATTR static char rstWhy[48];
static const uint32_t RST_MAGIC = 0x31523445;
static volatile bool reqRestart = false;
static uint32_t restartAt = 0;
static void armRestart(const char *why) {
  rstMagic = RST_MAGIC;
  strncpy(rstWhy, why ? why : "unspecified", sizeof rstWhy - 1);
  rstWhy[sizeof rstWhy - 1] = 0;
  Serial.printf("# restart: %s\n", rstWhy);
  reqRestart = true;
  restartAt = millis() + 1200;
}
