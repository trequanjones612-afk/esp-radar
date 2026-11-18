// Tuya / CloudyBay local control. The local key is a secret: it is copied onto the
// stack for a command, then wiped. Callers must not log it or put it in JSON.
// 3.3 / 3.4 / 3.5 follow the public TinyTuya protocol notes. Untested on hardware.
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClient.h>
#include <mbedtls/aes.h>
#include <mbedtls/md.h>
#include <mbedtls/gcm.h>
#include <esp_system.h>
#include <time.h>
#include <string.h>

// Same CRC-32 as zlib / binascii.crc32, which is what the Tuya 3.3 frame uses.
static uint32_t crc32(const uint8_t *data, int n) {
  uint32_t c = 0xffffffffu;
  for (int i = 0; i < n; i++) {
    c ^= data[i];
    for (int b = 0; b < 8; b++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)-(int)(c & 1));
  }
  return ~c;
}

static const uint8_t TUYA_PREFIX[] = {0x00, 0x00, 0x55, 0xaa};
static const uint8_t TUYA_SUFFIX[] = {0x00, 0x00, 0xaa, 0x55};
static const uint8_t TUYA35_PREFIX[] = {0x00, 0x00, 0x66, 0x99};
static const uint8_t TUYA35_SUFFIX[] = {0x00, 0x00, 0x99, 0x66};

static void tuyaKey16(const char *key, uint8_t out[16]) {
  memset(out, 0, 16);
  if (!key) return;
  size_t n = strlen(key);
  if (n > 16) n = 16;
  memcpy(out, key, n);
}

static bool aesEcb(const uint8_t key[16], const uint8_t *in, int len, uint8_t *out, bool enc) {
  if (len <= 0 || (len & 15)) return false;
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  int rc = enc ? mbedtls_aes_setkey_enc(&ctx, key, 128) : mbedtls_aes_setkey_dec(&ctx, key, 128);
  if (rc == 0) {
    for (int i = 0; i < len; i += 16)
      rc |= mbedtls_aes_crypt_ecb(&ctx, enc ? MBEDTLS_AES_ENCRYPT : MBEDTLS_AES_DECRYPT, in + i, out + i);
  }
  mbedtls_aes_free(&ctx);
  return rc == 0;
}

static int pkcs7(const uint8_t *in, int n, uint8_t *out, int cap) {
  int pad = 16 - (n & 15);
  if (pad == 0) pad = 16;
  if (n + pad > cap) return -1;
  memcpy(out, in, n);
  memset(out + n, pad, pad);
  return n + pad;
}

static void hmacSha256(const uint8_t *key, size_t klen, const uint8_t *data, size_t n, uint8_t out[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, key, klen, data, n, out);
}

static void putBe32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v;
}

// 55AA frame. hmac32 true → 32-byte HMAC-SHA256 (3.4) instead of CRC32 (3.3).
static int frame55(uint8_t *out, int cap, uint32_t seq, uint32_t cmd, const uint8_t *payload, int plen, bool hmac32, const uint8_t *macKey, int macLen) {
  int tail = hmac32 ? 32 + 4 : 4 + 4;
  int total = 16 + plen + tail;
  if (total > cap || plen < 0) return -1;
  putBe32(out, 0x000055aa);
  putBe32(out + 4, seq);
  putBe32(out + 8, cmd);
  putBe32(out + 12, (uint32_t)(plen + tail));
  if (plen) memcpy(out + 16, payload, plen);
  if (hmac32) {
    hmacSha256(macKey, macLen, out, 16 + plen, out + 16 + plen);
    memcpy(out + 16 + plen + 32, TUYA_SUFFIX, 4);
  } else {
    uint32_t c = crc32(out, 16 + plen);
    putBe32(out + 16 + plen, c);
    memcpy(out + 16 + plen + 4, TUYA_SUFFIX, 4);
  }
  return total;
}

static int frame35(uint8_t *out, int cap, uint32_t seq, uint32_t cmd, const uint8_t *plain, int plen, const uint8_t key[16]) {
  uint8_t iv[12];
  esp_fill_random(iv, sizeof iv);
  int total = 4 + 2 + 4 + 4 + 4 + 12 + plen + 16 + 4;
  if (total > cap) return -1;
  uint8_t *p = out;
  memcpy(p, TUYA35_PREFIX, 4); p += 4;
  p[0] = 0; p[1] = 0; p += 2;
  putBe32(p, seq); p += 4;
  putBe32(p, cmd); p += 4;
  putBe32(p, (uint32_t)(12 + plen + 16)); p += 4;
  uint8_t aad[14];
  memcpy(aad, out + 4, 14);
  memcpy(p, iv, 12);
  uint8_t *ct = p + 12;
  uint8_t *tag = ct + plen;
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0)
    rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, plen, iv, 12, aad, 14, plain, ct, 16, tag);
  mbedtls_gcm_free(&g);
  if (rc != 0) return -1;
  memcpy(tag + 16, TUYA35_SUFFIX, 4);
  return total;
}

static bool gcmDecrypt(const uint8_t key[16], const uint8_t *iv, const uint8_t *aad, int aadLen,
                       const uint8_t *ct, int ctLen, const uint8_t *tag, uint8_t *out) {
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, key, 128);
  if (rc == 0)
    rc = mbedtls_gcm_auth_decrypt(&g, ctLen, iv, 12, aad, aadLen, tag, 16, ct, out);
  mbedtls_gcm_free(&g);
  return rc == 0;
}

static int tcpReadSome(WiFiClient &c, uint8_t *buf, int cap, uint32_t ms) {
  uint32_t t = millis();
  int n = 0;
  while ((int32_t)(millis() - t) < (int32_t)ms && n < cap) {
    while (c.available() && n < cap) buf[n++] = (uint8_t)c.read();
    if (n >= 20) break;
    delay(8);
  }
  return n;
}

static bool tcpExchange(WiFiClient &c, const uint8_t *msg, int n, uint8_t *rx, int rxCap, int &rxN) {
  if (!c.connected()) return false;
  c.write(msg, n);
  rxN = tcpReadSome(c, rx, rxCap, 350);
  return rxN >= 16;
}

// Encrypt JSON for 3.3: clear "3.3" + 12 zero bytes + AES-ECB(PKCS7(json)).
static int enc33(const uint8_t key[16], const char *json, uint8_t *out, int cap) {
  uint8_t padded[240];
  int n = pkcs7((const uint8_t *)json, (int)strlen(json), padded, sizeof padded);
  if (n < 0 || 15 + n > cap) return -1;
  memcpy(out, "3.3", 3);
  memset(out + 3, 0, 12);
  if (!aesEcb(key, padded, n, out + 15, true)) return -1;
  return 15 + n;
}

// 3.4 post-handshake: header is inside the ciphertext.
static int enc34(const uint8_t key[16], const char *json, uint8_t *out, int cap) {
  uint8_t plain[240];
  int jlen = (int)strlen(json);
  if (15 + jlen > (int)sizeof plain) return -1;
  memcpy(plain, "3.4", 3);
  memset(plain + 3, 0, 12);
  memcpy(plain + 15, json, jlen);
  uint8_t padded[256];
  int n = pkcs7(plain, 15 + jlen, padded, sizeof padded);
  if (n < 0 || n > cap) return -1;
  if (!aesEcb(key, padded, n, out, true)) return -1;
  return n;
}

static bool negotiate34(WiFiClient &c, const uint8_t real[16], uint8_t session[16], uint32_t &seq) {
  uint8_t nonce[16], enc[32], frame[96], rx[320];
  esp_fill_random(nonce, 16);
  if (!aesEcb(real, nonce, 16, enc, true)) return false;
  int n = frame55(frame, sizeof frame, seq++, 0x03, enc, 16, false, nullptr, 0);
  int rn = 0;
  if (n < 0 || !tcpExchange(c, frame, n, rx, sizeof rx, rn)) return false;
  // Find payload after 16-byte header. Device packets include a 4-byte retcode.
  if (rn < 16 + 4 + 48) return false;
  const uint8_t *pay = rx + 16;
  if (rn >= 20 && pay[0] == 0 && pay[1] == 0) pay += 4;   // retcode
  uint8_t dec[64];
  if (!aesEcb(real, pay, 48, dec, false)) return false;
  uint8_t expect[32];
  hmacSha256(real, 16, nonce, 16, expect);
  if (memcmp(expect, dec + 16, 32) != 0) return false;
  uint8_t devNonce[16];
  memcpy(devNonce, dec, 16);
  uint8_t finish[32];
  hmacSha256(real, 16, devNonce, 16, finish);
  uint8_t encF[32];
  if (!aesEcb(real, finish, 32, encF, true)) return false;
  n = frame55(frame, sizeof frame, seq++, 0x05, encF, 32, false, nullptr, 0);
  if (n < 0 || !tcpExchange(c, frame, n, rx, sizeof rx, rn)) return false;
  uint8_t x[16];
  for (int i = 0; i < 16; i++) x[i] = (uint8_t)(nonce[i] ^ devNonce[i]);
  if (!aesEcb(real, x, 16, session, true)) return false;
  return true;
}

static bool negotiate35(WiFiClient &c, const uint8_t real[16], uint8_t session[16], uint32_t &seq) {
  uint8_t nonce[16], frame[160], rx[400];
  esp_fill_random(nonce, 16);
  int n = frame35(frame, sizeof frame, seq++, 0x03, nonce, 16, real);
  int rn = 0;
  if (n < 0 || !tcpExchange(c, frame, n, rx, sizeof rx, rn) || rn < 4 + 2 + 12 + 16 + 48) return false;
  // 6699: prefix4 + reserved2 + seq4 + cmd4 + len4 + iv12 + ct + tag16 + footer4
  if (memcmp(rx, TUYA35_PREFIX, 4) != 0) return false;
  uint8_t iv[12];
  memcpy(iv, rx + 18, 12);
  int ctLen = rn - (18 + 12 + 16 + 4);
  if (ctLen < 48) ctLen = 48;
  uint8_t aad[14];
  memcpy(aad, rx + 4, 14);
  uint8_t dec[80];
  if (!gcmDecrypt(real, iv, aad, 14, rx + 30, 48, rx + 30 + 48, dec)) return false;
  uint8_t expect[32];
  hmacSha256(real, 16, nonce, 16, expect);
  if (memcmp(expect, dec + 16, 32) != 0) return false;
  uint8_t devNonce[16];
  memcpy(devNonce, dec, 16);
  uint8_t finish[32];
  hmacSha256(real, 16, devNonce, 16, finish);
  n = frame35(frame, sizeof frame, seq++, 0x05, finish, 32, real);
  if (n < 0 || !tcpExchange(c, frame, n, rx, sizeof rx, rn)) return false;
  uint8_t x[16];
  for (int i = 0; i < 16; i++) x[i] = (uint8_t)(nonce[i] ^ devNonce[i]);
  // session key = GCM ciphertext (16 bytes) of the XOR block, IV = first 12 of client nonce
  uint8_t ct[16], tag[16];
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  int rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, real, 128);
  if (rc == 0)
    rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, 16, nonce, 12, nullptr, 0, x, ct, 16, tag);
  mbedtls_gcm_free(&g);
  if (rc != 0) return false;
  memcpy(session, ct, 16);
  return true;
}

static void dpsJson(char *js, int cap, const char *id, bool on, int bri, bool alt) {
  unsigned long t = (unsigned long)time(nullptr);
  if (t < 1700000000ul) t = millis() / 1000;
  int b = 10 + (constrain(bri, 1, 100) - 1) * 10;   // 10..1000, the usual bright_value range
  if (b > 1000) b = 1000;
  if (alt)
    snprintf(js, cap, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\",\"dps\":{\"1\":%s,\"2\":%d}}", id, id, t, on ? "true" : "false", b);
  else
    snprintf(js, cap, "{\"devId\":\"%s\",\"uid\":\"%s\",\"t\":\"%lu\",\"dps\":{\"20\":%s,\"22\":%d}}", id, id, t, on ? "true" : "false", b);
}

// Returns true if the device answered a framed packet. key is wiped by the caller.
static bool tuyaControl(const IPAddress &ip, const char *devId, const char *key, int ver, bool on, int bri) {
  if (!devId || !devId[0] || !key || !key[0]) return false;
  if (!strncmp(devId, "tuya-", 5) || !strncmp(devId, "ip-", 3)) return false;
  uint8_t real[16];
  tuyaKey16(key, real);
  WiFiClient c;
  c.setTimeout(2);
  if (!c.connect(ip, 6668, 800)) { memset(real, 0, sizeof real); return false; }
  uint32_t seq = 1;
  uint8_t session[16];
  memcpy(session, real, 16);
  bool ok = true;
  if (ver >= 35) ok = negotiate35(c, real, session, seq);
  else if (ver >= 34) ok = negotiate34(c, real, session, seq);
  if (!ok) { c.stop(); memset(real, 0, sizeof real); memset(session, 0, sizeof session); return false; }
  bool answered = false;
  for (int alt = 0; alt < 2 && !answered; alt++) {
    char js[200];
    dpsJson(js, sizeof js, devId, on, bri, alt == 1);
    uint8_t pay[280], frame[420], rx[200];
    int plen = -1, n = -1;
    if (ver >= 35) {
      uint8_t plain[240];
      int jlen = (int)strlen(js);
      memcpy(plain, "3.5", 3);
      memset(plain + 3, 0, 12);
      memcpy(plain + 15, js, jlen);
      n = frame35(frame, sizeof frame, seq++, 0x07, plain, 15 + jlen, session);
    } else if (ver >= 34) {
      plen = enc34(session, js, pay, sizeof pay);
      if (plen > 0) n = frame55(frame, sizeof frame, seq++, 0x07, pay, plen, true, session, 16);
    } else {
      plen = enc33(real, js, pay, sizeof pay);
      if (plen > 0) n = frame55(frame, sizeof frame, seq++, 0x07, pay, plen, false, nullptr, 0);
    }
    int rn = 0;
    if (n > 0 && tcpExchange(c, frame, n, rx, sizeof rx, rn)) {
      bool framed = (memcmp(rx, TUYA_PREFIX, 4) == 0) || (memcmp(rx, TUYA35_PREFIX, 4) == 0);
      if (framed) answered = true;
    }
  }
  c.stop();
  memset(real, 0, sizeof real);
  memset(session, 0, sizeof session);
  return answered;
}

// 0 = nothing, 1 = TCP 6668 accepted the connection, 2 = it answered with a Tuya frame.
static int tuyaPortProbe(const IPAddress &ip) {
  WiFiClient c;
  c.setTimeout(1);
  if (!c.connect(ip, 6668, 160)) return 0;
  uint8_t frame[32], rx[80];
  int n = frame55(frame, sizeof frame, 1, 0x09, nullptr, 0, false, nullptr, 0);
  int rn = 0;
  bool framed = n > 0 && tcpExchange(c, frame, n, rx, sizeof rx, rn) && rn >= 4 &&
                (memcmp(rx, TUYA_PREFIX, 4) == 0 || memcmp(rx, TUYA35_PREFIX, 4) == 0);
  c.stop();
  return framed ? 2 : 1;
}
