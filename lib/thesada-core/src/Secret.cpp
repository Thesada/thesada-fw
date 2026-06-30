// thesada-fw - Secret.cpp
// SPDX-License-Identifier: GPL-3.0-only
//
// Raw IDF nvs API, not Arduino Preferences: an absent namespace/key is the
// normal standalone path and Preferences ERROR-logs every miss (un-maskable
// ARDUINO tag); the nvs calls return ESP_ERR_NVS_NOT_FOUND quietly.
#include "Secret.h"
#include "secret_keymap.h"
#include <nvs.h>
#include <string.h>

static const char* SECRET_NS = "thesada-secrets";

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

// wifiKey / nvsKeyFor delegate to the pure secret_keymap unit (host-tested).
void Secret::wifiKey(const char* ssid, char* out, size_t maxLen) {
  secretWifiKey(ssid, out, maxLen);
}

bool Secret::nvsKeyFor(const char* field, char* keyOut, size_t maxLen) {
  return secretNvsKeyFor(field, keyOut, maxLen);
}

bool Secret::set(const char* field, const char* value) {
  char key[16];
  if (!nvsKeyFor(field, key, sizeof(key))) return false;
  if (!value || !*value) return clear(field);   // empty value = clear
  if (strlen(value) >= MAX_LEN) return false;    // must fit every read buffer
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
  esp_err_t e = nvs_open(SECRET_NS, NVS_READWRITE, &h);
  if (e == ESP_ERR_NVS_NOT_FOUND) return true;   // namespace never created = clear
  if (e != ESP_OK) return false;                  // real open failure
  e = nvs_erase_key(h, key);
  if (e == ESP_ERR_NVS_NOT_FOUND) { nvs_close(h); return true; }  // key absent = clear
  if (e == ESP_OK) e = nvs_commit(h);
  nvs_close(h);
  return e == ESP_OK;
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
