// thesada-fw - device_identity_policy.h
// Pure derivation and encoding for first-boot device identity.
// Host-unit-testable. See docs/invariants.md for where identity is stored.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define IDENTITY_ID_PREFIX     "thesada-"
#define IDENTITY_MAC_LEN       6
#define IDENTITY_ID_HEX_LEN    (IDENTITY_MAC_LEN * 2)
// "thesada-" + 12 hex + NUL.
#define IDENTITY_ID_CAP        (sizeof(IDENTITY_ID_PREFIX) + IDENTITY_ID_HEX_LEN)
#define IDENTITY_PUBKEY_LEN    32
#define IDENTITY_PUBKEY_HEX_CAP (IDENTITY_PUBKEY_LEN * 2 + 1)

// Lowercase hex, NUL-terminated. Truncation is a failure, not a short write:
// a clipped key or id is a different identity, never a usable one.
// in: buf, len, out, cap. out: true only if the whole encoding fitted.
inline bool identityHexEncode(const uint8_t* buf, size_t len,
                              char* out, size_t cap) {
  if (!buf || !out || cap == 0) return false;
  if (len > (cap - 1) / 2) { out[0] = '\0'; return false; }
  static const char kHex[] = "0123456789abcdef";
  size_t i = 0;
  for (; i < len; i++) {
    out[i * 2]     = kHex[(buf[i] >> 4) & 0x0F];
    out[i * 2 + 1] = kHex[buf[i] & 0x0F];
  }
  out[len * 2] = '\0';
  return true;
}

// Device id from the base MAC: the full six bytes, not a suffix. Espressif
// assigns sequentially, so a short suffix collides across OUIs.
// in: mac (6 bytes), out, cap. out: true only if the whole id fitted.
inline bool identityDeviceIdFromMac(const uint8_t* mac, char* out, size_t cap) {
  if (!mac || !out || cap < IDENTITY_ID_CAP) {
    if (out && cap) out[0] = '\0';
    return false;
  }
  size_t plen = strlen(IDENTITY_ID_PREFIX);
  memcpy(out, IDENTITY_ID_PREFIX, plen);
  return identityHexEncode(mac, IDENTITY_MAC_LEN, out + plen, cap - plen);
}

// Does id have the shape this firmware generates? Used to decide whether a
// stored id is reusable or must be regenerated.
inline bool identityDeviceIdValid(const char* id) {
  if (!id) return false;
  size_t plen = strlen(IDENTITY_ID_PREFIX);
  if (strncmp(id, IDENTITY_ID_PREFIX, plen) != 0) return false;
  const char* h = id + plen;
  size_t n = 0;
  for (; h[n]; n++) {
    char c = h[n];
    bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    if (!hex) return false;
  }
  return n == IDENTITY_ID_HEX_LEN;
}

// An all-zero key is what an erased or half-written NVS read looks like, and
// it must never be treated as a generated keypair.
inline bool identityKeyMaterialPresent(const uint8_t* key, size_t len) {
  if (!key || len == 0) return false;
  for (size_t i = 0; i < len; i++) {
    if (key[i] != 0) return true;
  }
  return false;
}
