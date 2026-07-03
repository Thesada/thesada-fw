// thesada-fw - meshtastic_radio.h
// Pure Meshtastic radio-parameter derivation: modem preset table, region
// table, djb2 channel-name hash -> frequency-slot math, and PSK expansion
// (1-byte index / base64 material -> AES key). No Arduino/IDF deps, so it is
// host-unit-testable; LoRaModule feeds the results into the SX1262.
// Preset/region values and the slot formula mirror Meshtastic firmware
// RadioInterface.cpp; LongFast/US -> 906.875 MHz is bench-verified.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "meshtastic_frame.h"  // DEFAULT_KEY

namespace mesh {

// PHY constants shared by every preset.
constexpr uint8_t  SYNC     = 0x2b;
constexpr uint16_t PREAMBLE = 16;

// Modem presets. name doubles as the default channel name (Meshtastic uses
// the preset's slot-name string when no channel name is configured), so the
// strings must match Meshtastic's getChannelName() exactly - "LongMod", not
// "LongModerate". cr is the RadioLib denominator (4/5 -> 5, 4/8 -> 8).
struct Preset {
  const char* name;
  float       bwKhz;
  uint8_t     sf;
  uint8_t     cr;
};
constexpr Preset PRESETS[] = {
  {"ShortTurbo", 500.0f,  7, 5},
  {"ShortFast",  250.0f,  7, 5},
  {"ShortSlow",  250.0f,  8, 5},
  {"MediumFast", 250.0f,  9, 5},
  {"MediumSlow", 250.0f, 10, 5},
  {"LongTurbo",  500.0f, 11, 8},
  {"LongFast",   250.0f, 11, 5},
  {"LongMod",    125.0f, 11, 8},
};
constexpr size_t PRESET_COUNT = sizeof(PRESETS) / sizeof(PRESETS[0]);

// Frequency ranges (MHz). Small on purpose - config-extendable later.
struct Region {
  const char* name;
  float       freqStart;
  float       freqEnd;
  float       spacing;
};
constexpr Region REGIONS[] = {
  {"US",     902.0f, 928.0f,  0.0f},
  {"EU_868", 869.4f, 869.65f, 0.0f},
  {"ANZ",    915.0f, 928.0f,  0.0f},
};
constexpr size_t REGION_COUNT = sizeof(REGIONS) / sizeof(REGIONS[0]);

inline const Preset* presetFind(const char* name) {
  if (!name) return nullptr;
  for (const auto& p : PRESETS)
    if (strcmp(name, p.name) == 0) return &p;
  return nullptr;
}

inline const Region* regionFind(const char* name) {
  if (!name) return nullptr;
  for (const auto& r : REGIONS)
    if (strcmp(name, r.name) == 0) return &r;
  return nullptr;
}

// Meshtastic's channel-name hash (djb2).
inline uint32_t djb2(const char* s) {
  uint32_t h = 5381;
  for (const char* p = s; p && *p; ++p) h = h * 33 + (uint8_t)*p;
  return h;
}

// How many BW-wide slots fit the region. 0 = preset does not fit (caller
// must fail closed, not pick a fallback frequency).
inline uint16_t numChannels(const Region& r, float bwKhz) {
  float step = r.spacing + bwKhz / 1000.0f;
  if (step <= 0.0f) return 0;
  return (uint16_t)((r.freqEnd - r.freqStart) / step);
}

// Channel name -> center frequency. slotOut gets the 0-based slot (the
// Meshtastic UI shows slot+1). Returns 0.0 when the preset does not fit
// the region.
inline float slotFreqMhz(const Region& r, float bwKhz, const char* channelName,
                         uint16_t& slotOut) {
  uint16_t n = numChannels(r, bwKhz);
  slotOut = 0;
  if (n == 0) return 0.0f;
  slotOut = (uint16_t)(djb2(channelName) % n);
  float step = r.spacing + bwKhz / 1000.0f;
  return r.freqStart + (bwKhz / 2000.0f) + slotOut * step;
}

// Minimal base64 decode (standard alphabet, '=' padding, no whitespace).
// Returns output length; SIZE_MAX on bad input or overflow.
inline size_t b64Decode(const char* s, uint8_t* out, size_t cap) {
  if (!s) return (size_t)-1;
  uint32_t acc = 0;
  int bits = 0;
  size_t n = 0;
  bool done = false;
  for (const char* p = s; *p; ++p) {
    char c = *p;
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else if (c == '=') { done = true; continue; }
    else return (size_t)-1;
    if (done) return (size_t)-1;  // data after padding
    acc = (acc << 6) | (uint32_t)v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return (size_t)-1;
      out[n++] = (uint8_t)(acc >> bits);
    }
  }
  return n;
}

// PSK material -> AES key, per Meshtastic Channels.cpp:
//   len 0            -> default channel key (AES128)
//   len 1, value 0   -> crypto off (keyLen 0)
//   len 1, value 1   -> default key
//   len 1, value 2+  -> default key with the last byte bumped by (value-1)
//   len 16 / 32      -> used as-is (AES128 / AES256)
// Anything else is invalid. Returns false on invalid input.
inline bool pskExpand(const uint8_t* psk, size_t pskLen, uint8_t keyOut[32],
                      size_t& keyLenOut) {
  keyLenOut = 0;
  if (pskLen == 1 && psk[0] == 0) return true;  // explicit no-crypto
  if (pskLen == 0 || pskLen == 1) {
    memcpy(keyOut, DEFAULT_KEY, sizeof(DEFAULT_KEY));
    if (pskLen == 1) keyOut[sizeof(DEFAULT_KEY) - 1] += psk[0] - 1;
    keyLenOut = sizeof(DEFAULT_KEY);
    return true;
  }
  if (pskLen == 16 || pskLen == 32) {
    memcpy(keyOut, psk, pskLen);
    keyLenOut = pskLen;
    return true;
  }
  return false;
}

}  // namespace mesh
