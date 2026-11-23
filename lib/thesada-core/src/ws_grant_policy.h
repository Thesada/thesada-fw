// thesada-fw - ws_grant_policy.h
// Single-use WebSocket grants: a random token per /api/ws/token call, bound to
// the issuing IP, consumed by the /ws/serial upgrade. No Arduino deps.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "ttl_policy.h"

static constexpr uint8_t  WS_GRANT_SLOTS  = 4;
static constexpr uint32_t WS_GRANT_TTL_MS = 30000UL;
static constexpr size_t   WS_TOKEN_HEX    = 32;   // 16 random bytes
static constexpr size_t   WS_GRANT_IP_CAP = 46;   // fits an IPv6 text address

struct WsGrant {
  char     token[WS_TOKEN_HEX + 1];
  char     ip[WS_GRANT_IP_CAP];
  uint64_t expiry;   // 0 = empty or consumed (ttl_policy.h)
};

// Compares every byte, so the time taken does not leak a matching prefix.
inline bool constTimeEq(const char* a, const char* b, size_t len) {
  uint8_t diff = 0;
  for (size_t i = 0; i < len; i++) diff |= (uint8_t)(a[i] ^ b[i]);
  return diff == 0;
}

// Hex-encode n random bytes into out (2n chars plus NUL).
inline void wsTokenHex(const uint8_t* bytes, size_t n, char* out) {
  static const char kHexDigits[] = "0123456789abcdef";
  for (size_t i = 0; i < n; i++) {
    out[2 * i]     = kHexDigits[bytes[i] >> 4];
    out[2 * i + 1] = kHexDigits[bytes[i] & 0x0F];
  }
  out[2 * n] = '\0';
}

// Store a grant in a free slot. A live grant is never evicted.
// in: table, uptime ms, 32-hex token, issuing IP. out: slot index, or -1 when full or the input does not fit.
inline int wsGrantIssue(WsGrant* t, uint64_t now, const char* token, const char* ip) {
  if (!token || !ip || strlen(token) != WS_TOKEN_HEX || strlen(ip) >= WS_GRANT_IP_CAP) return -1;
  for (uint8_t i = 0; i < WS_GRANT_SLOTS; i++) {
    if (ttlActive(now, t[i].expiry)) continue;
    memcpy(t[i].token, token, WS_TOKEN_HEX + 1);
    memcpy(t[i].ip, ip, strlen(ip) + 1);
    t[i].expiry = now + WS_GRANT_TTL_MS;
    return i;
  }
  return -1;
}

// Spend the grant matching token and IP. Every slot is compared, so the slot
// loop does not leak which one matched. out: true once, for a live match.
inline bool wsGrantConsume(WsGrant* t, uint64_t now, const char* token, const char* ip) {
  if (!token || !ip || strlen(token) != WS_TOKEN_HEX) return false;
  int hit = -1;
  for (uint8_t i = 0; i < WS_GRANT_SLOTS; i++) {
    const bool live  = ttlActive(now, t[i].expiry);
    const bool tokOk = constTimeEq(t[i].token, token, WS_TOKEN_HEX);
    const bool ipOk  = strcmp(t[i].ip, ip) == 0;
    if (live && tokOk && ipOk && hit < 0) hit = i;
  }
  if (hit < 0) return false;
  t[hit].expiry = 0;
  return true;
}
