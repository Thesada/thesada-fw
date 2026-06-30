// thesada-fw - Secret.h
// Per-device secrets in the "thesada-secrets" NVS namespace. See the
// device-secrets invariant for the resolution-order contract.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <Arduino.h>

class Secret {
public:
  // Single max length for any secret. set() rejects longer values and every
  // read-site buffer is sized to this, so a stored value always fits resolve()
  // - an undersized read buffer makes NVS look empty and silently falls back to
  // config.json, breaking NVS-wins. Covers WPA2 PSK (63), tokens, passwords.
  static constexpr size_t MAX_LEN = 160;

  // Effective value of nvsKey: NVS -> fallback -> "". Always written into out
  // (out must outlive the returned pointer); returns out.
  static const char* resolve(const char* nvsKey, const char* fallback,
                             char* out, size_t maxLen);

  // Logical field -> 15-char-safe NVS key. Scalars (mqtt.password,
  // telegram.bot_token, web.password, wifi.ap_password) and "wifi.password:<ssid>".
  // out: true if known. in: field, keyOut buffer.
  static bool nvsKeyFor(const char* field, char* keyOut, size_t maxLen);

  // NVS key for a wifi network password - short SSID hash, fits the 15-char cap.
  static void wifiKey(const char* ssid, char* out, size_t maxLen);

  // Provisioning over the namespace; field is a logical name. set("") == clear.
  static bool set(const char* field, const char* value);
  static bool clear(const char* field);
  static bool has(const char* field);  // NVS holds a non-empty value
};
