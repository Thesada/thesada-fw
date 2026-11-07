// thesada-fw - cell_ntp_policy.h
// Cellular NTP schedule and server choice. Right after bearer activation the
// modem cannot resolve yet (+CNTP: 62), and a LAN server is unreachable over
// a carrier. Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>

static const uint32_t CELL_NTP_FIRST_MS = 30000UL;
static const uint32_t CELL_NTP_RETRY_MS = 60000UL;
static const uint32_t CELL_NTP_MAX_MS   = 1800000UL;

// Gap before the next server of the same attempt: one host per loop pass.
static const uint32_t CELL_NTP_NEXT_HOST_MS = 2000UL;

// One +CNTP wait holds the AT bus. Kept under the 20 s hold the cellular
// MQTT keepalive is sized for.
static const uint32_t CELL_NTP_WAIT_MAX_MS = 15000UL;

// Longest host the copy and the +CNTP command buffer both take.
static const size_t CELL_NTP_HOST_MAX = 111;

// in: attempts already made since activation. out: wait before the next one.
inline uint32_t cellNtpDelayMs(uint8_t attempts) {
  if (attempts == 0) return CELL_NTP_FIRST_MS;
  uint32_t d = CELL_NTP_RETRY_MS;
  for (uint8_t i = 1; i < attempts && d < CELL_NTP_MAX_MS; i++) d *= 2;
  return d < CELL_NTP_MAX_MS ? d : CELL_NTP_MAX_MS;
}

// in: millis() now, deadline. out: true once due, safe across the 49-day wrap.
inline bool cellNtpDue(uint32_t now, uint32_t at) {
  return (int32_t)(now - at) >= 0;
}

// in: ntp.cell_timeout_s as configured. out: the +CNTP wait, 1 s to the cap.
inline uint32_t cellNtpWaitMs(int32_t seconds) {
  if (seconds < 1) return 1000UL;
  if ((uint32_t)seconds >= CELL_NTP_WAIT_MAX_MS / 1000UL) return CELL_NTP_WAIT_MAX_MS;
  return (uint32_t)seconds * 1000UL;
}

// Servers one attempt may try: ntp.server, ntp.server2, then this default.
static const char* const CELL_NTP_DEFAULT = "pool.ntp.org";
static const uint8_t     CELL_NTP_PER_TRY = 2;

// in: host. out: its length without one trailing root dot.
inline size_t cellNtpHostLen(const char* host) {
  size_t len = 0;
  while (host[len]) len++;
  if (len > 1 && host[len - 1] == '.') len--;
  return len;
}

// in: host. out: false for what a carrier bearer cannot reach or the modem
// command cannot carry: empty, too long, a private, loopback, link-local or
// zero IPv4 literal, or an mDNS .local name. One trailing dot is ignored.
inline bool cellNtpHostUsable(const char* host) {
  if (!host || !*host) return false;
  size_t len = cellNtpHostLen(host);
  if (len > CELL_NTP_HOST_MAX) return false;

  int    o[4] = {0, 0, 0, 0}, n = 0, digits = 0;
  size_t i    = 0;
  for (; i < len; i++) {
    char c = host[i];
    if (c >= '0' && c <= '9') {
      o[n] = o[n] * 10 + (c - '0');
      if (++digits > 3 || o[n] > 255) break;
    } else if (c == '.' && digits > 0 && n < 3) {
      n++;
      digits = 0;
    } else {
      break;
    }
  }
  if (i == len && n == 3 && digits > 0) {
    if (o[0] == 10 || o[0] == 127 || o[0] == 0) return false;
    if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return false;
    if (o[0] == 192 && o[1] == 168) return false;
    if (o[0] == 169 && o[1] == 254) return false;
    return true;
  }

  const char   sfx[] = ".local";
  const size_t sl    = sizeof(sfx) - 1;
  if (len >= sl) {
    for (size_t k = 0; k < sl; k++) {
      char c = host[len - sl + k];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != sfx[k]) return true;
    }
    return false;
  }
  return true;
}

// in: a, b. out: true when both name the same server, ASCII case-insensitive,
// one trailing dot ignored.
inline bool cellNtpSameHost(const char* a, const char* b) {
  if (!a || !b) return false;
  size_t la = cellNtpHostLen(a), lb = cellNtpHostLen(b);
  if (la != lb) return false;
  for (size_t i = 0; i < la; i++) {
    char x = a[i], y = b[i];
    if (x >= 'A' && x <= 'Z') x = (char)(x - 'A' + 'a');
    if (y >= 'A' && y <= 'Z') y = (char)(y - 'A' + 'a');
    if (x != y) return false;
  }
  return true;
}

// in: configured server, server2, out array of CELL_NTP_PER_TRY. out: count of
// usable, distinct servers in order, falling back to CELL_NTP_DEFAULT.
inline uint8_t cellNtpCandidates(const char* server, const char* server2,
                                 const char* out[]) {
  const char* src[3] = {server, server2, CELL_NTP_DEFAULT};
  for (uint8_t i = 0; i < CELL_NTP_PER_TRY; i++) out[i] = nullptr;
  uint8_t n = 0;
  for (uint8_t i = 0; i < 3 && n < CELL_NTP_PER_TRY; i++) {
    if (!cellNtpHostUsable(src[i])) continue;
    bool dup = false;
    for (uint8_t j = 0; j < n; j++) dup = dup || cellNtpSameHost(out[j], src[i]);
    if (!dup) out[n++] = src[i];
  }
  return n;
}
