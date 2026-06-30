// thesada-fw - secret_keymap.h
// Pure logical-field -> NVS-key mapping for device secrets. No Arduino/IDF
// deps, so it is host-unit-testable; Secret wraps the NVS I/O around it.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

// NVS key for a wifi network password: wifi_pw_<6 hex of FNV-1a(ssid)>, so the
// key fits the 15-char NVS limit regardless of SSID length.
inline void secretWifiKey(const char* ssid, char* out, size_t maxLen) {
  uint32_t h = 2166136261u;
  for (const char* p = ssid; p && *p; ++p) { h ^= (uint8_t)*p; h *= 16777619u; }
  snprintf(out, maxLen, "wifi_pw_%06x", (unsigned)(h & 0xFFFFFFu));
}

// Logical field -> 15-char-safe NVS key. Scalars plus "wifi.password:<ssid>".
// out: true if field is known (key written to keyOut), false otherwise.
inline bool secretNvsKeyFor(const char* field, char* keyOut, size_t maxLen) {
  if (!field || !keyOut) return false;
  static const struct { const char* field; const char* key; } kMap[] = {
    { "mqtt.password",      "mqtt_password"  },
    { "telegram.bot_token", "telegram_token" },
    { "web.password",       "web_password"   },
    { "wifi.ap_password",   "ap_password"    },
  };
  for (const auto& m : kMap) {
    if (strcmp(field, m.field) == 0) { snprintf(keyOut, maxLen, "%s", m.key); return true; }
  }
  static const char* kWifiPrefix = "wifi.password:";
  size_t plen = strlen(kWifiPrefix);
  if (strncmp(field, kWifiPrefix, plen) == 0 && field[plen]) {
    secretWifiKey(field + plen, keyOut, maxLen);
    return true;
  }
  return false;
}
