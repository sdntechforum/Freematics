#include "config.h"
#include "sms_command.h"

#if ENABLE_SMS_COMMANDS

#include <ctype.h>
#include <string.h>
#include <stdio.h>

#include <Arduino.h>
#include "esp_system.h"
#include "mbedtls/md.h"

#if STORAGE == STORAGE_SPIFFS
#include "telestore.h"
extern SPIFFSLogger logger;
#elif STORAGE == STORAGE_SD
#include "telestore.h"
extern SDLogger logger;
#endif

#ifndef SMS_POLL_INTERVAL_MS
#define SMS_POLL_INTERVAL_MS 120000
#endif


#define SMS_MAGIC "FM1"
#define SMS_CMD_REBOOT "REBOOT"
#define SMS_MAC_HEX 16
#define SMS_CTR_MAX_DIGITS 10

static bool constant_time_eq16(const uint8_t *a, const uint8_t *b)
{
  uint8_t d = 0;
  for (int i = 0; i < 8; i++) d |= (uint8_t)(a[i] ^ b[i]);
  return d == 0;
}

static bool hex16_valid(const char *s)
{
  for (int i = 0; i < SMS_MAC_HEX; i++) {
    if (!isxdigit((unsigned char)s[i])) return false;
  }
  return s[SMS_MAC_HEX] == 0;
}

static uint8_t hexval(unsigned char c)
{
  if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
  if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
  return 0xff;
}

static bool parse_mac_hex(const char *s, uint8_t out[8])
{
  if (!hex16_valid(s)) return false;
  for (int i = 0; i < 8; i++) {
    uint8_t hi = hexval((unsigned char)s[2 * i]);
    uint8_t lo = hexval((unsigned char)s[2 * i + 1]);
    if (hi > 15 || lo > 15) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static bool hmac_sha256_first8(const uint8_t *key, size_t key_len, const char *msg, uint8_t out8[8])
{
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  uint8_t full[32];
  if (!info) return false;
  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, info, 1) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }
  if (mbedtls_md_hmac_starts(&ctx, key, key_len) != 0 ||
      mbedtls_md_hmac_update(&ctx, (const unsigned char *)msg, strlen(msg)) != 0 ||
      mbedtls_md_hmac_finish(&ctx, full) != 0) {
    mbedtls_md_free(&ctx);
    return false;
  }
  mbedtls_md_free(&ctx);
  memcpy(out8, full, 8);
  return true;
}

static bool sender_allowed(nvs_handle_t nvs_h, const char *from)
{
  char trust[32];
  size_t len = sizeof(trust);
  if (nvs_get_str(nvs_h, SMS_NVS_FROM, trust, &len) != ESP_OK || trust[0] == 0) {
    return true;
  }
  return strcmp(from, trust) == 0;
}

void smsCommandPoll(CellSIMCOM *modem, int cellularRegistered, const char *devid, nvs_handle_t nvs_h)
{
  static uint32_t lastPoll = 0;
  static uint32_t lockoutUntil = 0;

  if (!modem || !devid || !nvs_h) return;
  if (!cellularRegistered) {
    lastPoll = 0;
    return;
  }
  uint32_t now = millis();
  if (now < lockoutUntil) return;
  if (lastPoll != 0 && (now - lastPoll) < (uint32_t)SMS_POLL_INTERVAL_MS) return;
  lastPoll = now;

  uint8_t key[32];
  size_t keylen = sizeof(key);
  if (nvs_get_blob(nvs_h, SMS_NVS_KEY_HMAC, key, &keylen) != ESP_OK || keylen != sizeof(key)) {
    return;
  }

  uint8_t en = 0;
  if (nvs_get_u8(nvs_h, SMS_NVS_EN, &en) != ESP_OK || en == 0) {
    return;
  }

  char body[200];
  char sender[48];
  int idx = -1;
  if (!modem->smsReadOldestUnread(body, sizeof(body), sender, sizeof(sender), &idx)) {
    return;
  }

  const char *s = body;
  while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;

  char magic[8], verb[16], ctrs[SMS_CTR_MAX_DIGITS + 2], macstr[SMS_MAC_HEX + 2];
  if (sscanf(s, "%7s %15s %10s %16s", magic, verb, ctrs, macstr) != 4) {
    Serial.println("[SMS] ignored (parse)");
    modem->smsDeleteByIndex(idx);
    return;
  }

  if (strcmp(magic, SMS_MAGIC) != 0 || strcmp(verb, SMS_CMD_REBOOT) != 0) {
    Serial.println("[SMS] ignored (format)");
    modem->smsDeleteByIndex(idx);
    return;
  }

  char *endp = NULL;
  unsigned long ctr_ul = strtoul(ctrs, &endp, 10);
  if (!endp || *endp != 0 || ctr_ul == 0 || ctr_ul > 0xffffffffUL) {
    Serial.println("[SMS] ignored (counter)");
    modem->smsDeleteByIndex(idx);
    return;
  }
  uint32_t ctr = (uint32_t)ctr_ul;

  uint8_t want[8];
  if (!parse_mac_hex(macstr, want)) {
    Serial.println("[SMS] ignored (mac fmt)");
    modem->smsDeleteByIndex(idx);
    return;
  }

  if (!sender_allowed(nvs_h, sender)) {
    Serial.println("[SMS] denied (sender)");
    modem->smsDeleteByIndex(idx);
    return;
  }

  uint32_t last_ctr = 0;
  esp_err_t ce = nvs_get_u32(nvs_h, SMS_NVS_CTR, &last_ctr);
  if (ce != ESP_OK) last_ctr = 0;
  if (ctr <= last_ctr) {
    Serial.println("[SMS] ignored (replay)");
    modem->smsDeleteByIndex(idx);
    return;
  }

  char signmsg[80];
  int sn = snprintf(signmsg, sizeof(signmsg), "%s|%s|%lu|%s", SMS_MAGIC, SMS_CMD_REBOOT, (unsigned long)ctr, devid);
  if (sn <= 0 || sn >= (int)sizeof(signmsg)) {
    modem->smsDeleteByIndex(idx);
    return;
  }

  uint8_t mac8[8];
  if (!hmac_sha256_first8(key, sizeof(key), signmsg, mac8) || !constant_time_eq16(mac8, want)) {
    Serial.println("[SMS] auth fail (lockout 1h)");
    lockoutUntil = now + 3600000UL;
    modem->smsDeleteByIndex(idx);
    return;
  }

  if (nvs_set_u32(nvs_h, SMS_NVS_CTR, ctr) != ESP_OK || nvs_commit(nvs_h) != ESP_OK) {
    Serial.println("[SMS] nvs fail");
    return;
  }

  modem->smsDeleteByIndex(idx);

  Serial.println("[SMS] verified REBOOT");
#if STORAGE == STORAGE_SPIFFS || STORAGE == STORAGE_SD
  logger.end();
#endif
  delay(300);
  esp_restart();
}

#endif /* ENABLE_SMS_COMMANDS */
