// thesada-fw - Secret.cpp
// SPDX-License-Identifier: GPL-3.0-only
//
// Raw IDF nvs API, not Arduino Preferences: an absent namespace/key is the
// normal standalone path and Preferences ERROR-logs every miss (un-maskable
// ARDUINO tag); the nvs calls return ESP_ERR_NVS_NOT_FOUND quietly.
#include "Secret.h"
#include <nvs.h>
#include <string.h>

static const char* SECRET_NS = "thesada-secrets";

// wifi.password:<ssid> -> wifi_pw_<6 hex>; everything after the prefix is the
// SSID (SSIDs may contain ':' so only the first one delimits).
static const char* WIFI_PW_PREFIX = "wifi.password:";

const char* Secret::resolve(const char* nvsKey, const char* fallback,
                            char* out, size_t maxLen) {
  if (!out || maxLen == 0) return fallback ? fallback : "";
  out[0] = '\0';
  if (nvsKey) {
    nvs_handle_t h;
    if (nvs_open(SECRET_NS, NVS_READONLY, &h) == ESP_OK) {
      size_t len = maxLen;
      esp_err_t e = nvs_get_str(h, nvsKey, out, &len);
      nvs_close(h);
      if (e == ESP_OK && out[0]) return out;  // NVS wins
    }
  }
  // NVS miss: copy the config.json plaintext (or empty) into out so the caller
  // always gets the effective value in a stable buffer.
  strncpy(out, fallback ? fallback : "", maxLen - 1);
  out[maxLen - 1] = '\0';
  return out;
}

void Secret::wifiKey(const char* ssid, char* out, size_t maxLen) {
  uint32_t h = 2166136261u;              // FNV-1a 32-bit
  for (const char* p = ssid; p && *p; ++p) {
    h ^= (uint8_t)*p;
    h *= 16777619u;
  }
  snprintf(out, maxLen, "wifi_pw_%06x", (unsigned)(h & 0xFFFFFFu));
}

bool Secret::nvsKeyFor(const char* field, char* keyOut, size_t maxLen) {
  if (!field || !keyOut) return false;
  struct { const char* field; const char* key; } map[] = {
    { "mqtt.password",      "mqtt_password"  },
    { "telegram.bot_token", "telegram_token" },
    { "web.password",       "web_password"   },
    { "wifi.ap_password",   "ap_password"    },
  };
  for (auto& m : map) {
    if (strcmp(field, m.field) == 0) {
      snprintf(keyOut, maxLen, "%s", m.key);
      return true;
    }
  }
  size_t plen = strlen(WIFI_PW_PREFIX);
  if (strncmp(field, WIFI_PW_PREFIX, plen) == 0 && field[plen]) {
    wifiKey(field + plen, keyOut, maxLen);
    return true;
  }
  return false;
}

bool Secret::set(const char* field, const char* value) {
  char key[16];
  if (!nvsKeyFor(field, key, sizeof(key))) return false;
  if (!value || !*value) return clear(field);  // empty value = clear
  nvs_handle_t h;
  if (nvs_open(SECRET_NS, NVS_READWRITE, &h) != ESP_OK) return false;
  esp_err_t e = nvs_set_str(h, key, value);
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
}

bool Secret::clear(const char* field) {
  char key[16];
  if (!nvsKeyFor(field, key, sizeof(key))) return false;
  nvs_handle_t h;
  if (nvs_open(SECRET_NS, NVS_READWRITE, &h) != ESP_OK) return true;  // ns absent = clear
  esp_err_t e = nvs_erase_key(h, key);
  if (e == ESP_OK) nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK || e == ESP_ERR_NVS_NOT_FOUND;  // absent key = already clear
}

bool Secret::has(const char* field) {
  char key[16];
  if (!nvsKeyFor(field, key, sizeof(key))) return false;
  nvs_handle_t h;
  if (nvs_open(SECRET_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t len = 0;
  esp_err_t e = nvs_get_str(h, key, nullptr, &len);  // query length only
  nvs_close(h);
  return e == ESP_OK && len > 1;  // len counts the null terminator
}
