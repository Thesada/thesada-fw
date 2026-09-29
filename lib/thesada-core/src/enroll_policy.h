// thesada-fw - enroll_policy.h
// Pure request building and step logic for first-boot enrollment over HTTPS.
// Host-unit-testable. See docs/invariants.md for the claim-code contract.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "device_identity_policy.h"
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ENROLL_CLAIM_CODE_LEN   8
#define ENROLL_URL_CAP          128
#define ENROLL_CHALLENGE_HEX_LEN 64
#define ENROLL_SIG_HEX_LEN      128
#define ENROLL_PATH             "/api/v1/devices/enroll"
#define ENROLL_HOST_CAP         96

// The server caps each (device_id, pubkey) at 30 requests/h, so no loop may
// run faster than one request per 120 s.
#define ENROLL_POLL_MS          150000u
#define ENROLL_BACKOFF_MIN_MS   150000u
#define ENROLL_BACKOFF_MAX_MS   1800000u

enum class EnrollStep : uint8_t { Announce, Verify, Poll, AwaitMtls, Ack, Done };

struct EnrollAction {
  EnrollStep next;
  uint32_t   delayMs;
  bool       progressed;  // caller resets its failure count when true
};

// Length of s when every char is lowercase hex. in: s. out: length, or -1
// when s is null or holds anything else.
inline long enrollHexLen(const char* s) {
  if (!s) return -1;
  long n = 0;
  for (; s[n]; n++) {
    char c = s[n];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
  }
  return n;
}

// Exactly len lowercase hex chars. Uppercase is refused: the server compares
// hex strings, so a case change is a different key. in: s, len. out: valid.
inline bool enrollHexValid(const char* s, size_t len) {
  return enrollHexLen(s) == (long)len;
}

// The flash-time claim code: exactly 8 ASCII digits.
// in: code. out: true when it has that shape.
inline bool enrollClaimCodeValid(const char* code) {
  if (!code) return false;
  size_t n = 0;
  for (; code[n]; n++) {
    if (code[n] < '0' || code[n] > '9') return false;
  }
  return n == ENROLL_CLAIM_CODE_LEN;
}

// Base URL of the app: https only, a host, no query or fragment, no controls.
// in: url. out: true when enrollEndpoint can build on it.
inline bool enrollUrlUsable(const char* url) {
  static const char kScheme[] = "https://";
  if (!url || strncmp(url, kScheme, sizeof(kScheme) - 1) != 0) return false;
  size_t n = strlen(url);
  if (n >= ENROLL_URL_CAP) return false;
  const char* host = url + sizeof(kScheme) - 1;
  if (*host == '\0' || *host == '/' || *host == ':') return false;
  for (size_t i = 0; i < n; i++) {
    unsigned char c = (unsigned char)url[i];
    if (c <= 0x20 || c >= 0x7f || c == '?' || c == '#' || c == '@') return false;
  }
  return true;
}

// Bytes of base with trailing slashes removed. in: base. out: length.
inline size_t enrollBaseLen(const char* base) {
  if (!base) return 0;
  size_t n = strlen(base);
  while (n > 0 && base[n - 1] == '/') n--;
  return n;
}

// Joins base + ENROLL_PATH + suffix ("", "/verify", "/cert", "/ack"), dropping
// trailing slashes on base. in: base, suffix, out, cap. out: true if it fitted.
inline bool enrollEndpoint(const char* base, const char* suffix,
                           char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!suffix || !enrollUrlUsable(base)) return false;
  size_t n = enrollBaseLen(base);
  int w = snprintf(out, cap, "%.*s%s%s", (int)n, base, ENROLL_PATH, suffix);
  if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return false; }
  return true;
}

// Claim-form URL for the setup page. Trailing slashes on base are dropped.
// in: out, cap, base, device id, 8-digit code. out: true if it fitted.
inline bool enrollClaimLink(char* out, size_t cap, const char* base,
                            const char* deviceId, const char* code) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!enrollUrlUsable(base) || !identityDeviceIdValid(deviceId) ||
      !enrollClaimCodeValid(code)) return false;
  size_t n = enrollBaseLen(base);
  int w = snprintf(out, cap, "%.*s/devices/claim?device_id=%s&code=%s",
                   (int)n, base, deviceId, code);
  if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return false; }
  return true;
}

// JSON body: device_id, pubkey, and claim_token or signature. Inputs are pre-checked.
// in: out, cap, deviceId, pubkeyHex, key, value. out: true if built.
inline bool enrollBody(char* out, size_t cap, const char* deviceId,
                       const char* pubkeyHex, const char* key, const char* value) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!identityDeviceIdValid(deviceId)) return false;
  if (!enrollHexValid(pubkeyHex, IDENTITY_PUBKEY_LEN * 2)) return false;
  if (!key || !value) return false;
  bool ok = (strcmp(key, "claim_token") == 0 && enrollClaimCodeValid(value)) ||
            (strcmp(key, "signature") == 0 && enrollHexValid(value, ENROLL_SIG_HEX_LEN));
  if (!ok) return false;
  int w = snprintf(out, cap, "{\"device_id\":\"%s\",\"pubkey\":\"%s\",\"%s\":\"%s\"}",
                   deviceId, pubkeyHex, key, value);
  if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return false; }
  return true;
}

// Broker host from the cert reply: letters, digits, dots and dashes, and short
// enough for the MQTT host buffer. in: host. out: true when storable.
inline bool enrollHostUsable(const char* host) {
  if (!host || !*host || *host == '.' || *host == '-') return false;
  size_t n = 0;
  for (; host[n]; n++) {
    char c = host[n];
    bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '-';
    if (!ok) return false;
  }
  return n < ENROLL_HOST_CAP;
}

// Topic prefix: no wildcards, controls, or edge slashes, with room for /cmd/config.
// in: prefix, prefixCap. out: true when storable.
inline bool enrollPrefixUsable(const char* prefix, size_t prefixCap) {
  static const char kLongest[] = "/cmd/config";
  if (!prefix || !*prefix || *prefix == '/') return false;
  size_t n = 0;
  for (; prefix[n]; n++) {
    unsigned char c = (unsigned char)prefix[n];
    if (c <= 0x20 || c >= 0x7f || c == '+' || c == '#') return false;
  }
  if (prefix[n - 1] == '/') return false;
  return n + sizeof(kLongest) - 1 < prefixCap;
}

// The cert reply is only stored when it is for this device and every broker
// field is usable. in: our id, reply fields, prefixCap. out: true to store.
inline bool enrollReplyUsable(const char* ourId, const char* replyId, const char* host,
                              long port, const char* prefix, size_t prefixCap) {
  if (!ourId || !replyId || strcmp(ourId, replyId) != 0) return false;
  if (port < 1 || port > 65535) return false;
  return enrollHostUsable(host) && enrollPrefixUsable(prefix, prefixCap);
}

// --- Revocation status check -------------------------------------------------
// A revoke gives the device no refusal it can see (PubSubClient ignores SUBACK
// codes), so a unit holding a cert asks the app.

#define ENROLL_STATUS_SUFFIX     "/status"
#define ENROLL_STATUS_DOMAIN     "thesada-enroll-status"
#define ENROLL_SERIAL_HEX_CAP    41            // 20-byte X.509 serial, plus NUL
#define ENROLL_STATUS_FIRST_MS   300000u       // after boot, once WiFi settled
#define ENROLL_STATUS_EVERY_MS   21600000u     // 6 h between answered checks
#define ENROLL_STATUS_MIN_EPOCH  1767225600LL  // 2026-01-01: earlier = clock not set

// The serial as the app stores it: lowercase hex, no leading zeros. The DER
// bytes carry a 00 pad and zero-padded bytes that the app's %x drops.
// in: raw hex, out, cap. out: true when in was hex and the result fitted.
inline bool enrollSerialCanonical(const char* in, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!in || enrollHexLen(in) <= 0) return false;
  while (in[0] == '0' && in[1] != '\0') in++;
  size_t n = strlen(in);
  if (n >= cap) return false;
  memcpy(out, in, n + 1);
  return true;
}

// Canonical serial shape: lowercase hex, no leading zero unless it is "0".
// in: serial. out: true when canonical and within ENROLL_SERIAL_HEX_CAP.
inline bool enrollSerialValid(const char* s) {
  long n = enrollHexLen(s);
  if (!s || n <= 0 || n >= ENROLL_SERIAL_HEX_CAP) return false;
  return !(s[0] == '0' && n > 1);
}

// The signed statement: domain, device id, serial, unix seconds, newline
// separated. The app rebuilds it from the body, so the two must agree byte
// for byte. in: out, cap, device id, canonical serial, ts. out: true if built.
inline bool enrollStatusMessage(char* out, size_t cap, const char* deviceId,
                                const char* serial, long long ts) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!identityDeviceIdValid(deviceId) || !enrollSerialValid(serial)) return false;
  if (ts < ENROLL_STATUS_MIN_EPOCH) return false;
  int w = snprintf(out, cap, ENROLL_STATUS_DOMAIN "\n%s\n%s\n%lld", deviceId, serial, ts);
  if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return false; }
  return true;
}

// JSON body for POST /status. in: out, cap, device id, pubkey, serial, ts,
// signature over enrollStatusMessage. out: true if built.
inline bool enrollStatusBody(char* out, size_t cap, const char* deviceId,
                             const char* pubkeyHex, const char* serial, long long ts,
                             const char* sigHex) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  if (!identityDeviceIdValid(deviceId) || !enrollSerialValid(serial)) return false;
  if (!enrollHexValid(pubkeyHex, IDENTITY_PUBKEY_LEN * 2)) return false;
  if (!enrollHexValid(sigHex, ENROLL_SIG_HEX_LEN) || ts < ENROLL_STATUS_MIN_EPOCH) return false;
  int w = snprintf(out, cap,
                   "{\"device_id\":\"%s\",\"pubkey\":\"%s\",\"serial\":\"%s\","
                   "\"ts\":%lld,\"signature\":\"%s\"}",
                   deviceId, pubkeyHex, serial, ts, sigHex);
  if (w < 0 || (size_t)w >= cap) { out[0] = '\0'; return false; }
  return true;
}

// Wipe only on an explicit "revoked" for this exact serial. Anything else,
// including an outage, keeps the cert: a broken app must never wipe a fleet.
// in: HTTP status, reply status, reply serial, our serial. out: true to wipe.
inline bool enrollStatusRevoked(int httpStatus, const char* status,
                                const char* replySerial, const char* ourSerial) {
  if (httpStatus != 200 || !status || !replySerial || !ourSerial) return false;
  if (!enrollSerialValid(ourSerial)) return false;
  return strcmp(status, "revoked") == 0 && strcmp(replySerial, ourSerial) == 0;
}

// Exponential from ENROLL_BACKOFF_MIN_MS, capped, plus up to 25% jitter so
// units behind one NAT do not retry in lockstep. in: failures, rnd. out: ms.
inline uint32_t enrollBackoffMs(uint8_t failures, uint32_t rnd) {
  uint32_t d = ENROLL_BACKOFF_MIN_MS;
  for (uint8_t i = 0; i < failures && d < ENROLL_BACKOFF_MAX_MS; i++) d *= 2;
  if (d > ENROLL_BACKOFF_MAX_MS) d = ENROLL_BACKOFF_MAX_MS;
  return d + rnd % (d / 4 + 1);
}

// Delay to the next status check after an answer, with up to 12.5% jitter;
// an unanswered check uses enrollBackoffMs. in: rnd. out: ms.
inline uint32_t enrollStatusCadenceMs(uint32_t rnd) {
  return ENROLL_STATUS_EVERY_MS + rnd % (ENROLL_STATUS_EVERY_MS / 8 + 1);
}

// Where a boot starts. No cert means enroll from scratch; a cert whose ack
// never landed waits for mTLS, then acks. in: hasCert, ackPending. out: step.
inline EnrollStep enrollStartStep(bool hasCert, bool ackPending) {
  if (!hasCert) return EnrollStep::Announce;
  return ackPending ? EnrollStep::AwaitMtls : EnrollStep::Done;
}

// Next step after an HTTP answer. status < 0 is transport failure. Refusals are 403.
// in: step, status, failures, rnd. out: next step, delay, progressed.
inline EnrollAction enrollOnResponse(EnrollStep step, int status,
                                     uint8_t failures, uint32_t rnd) {
  uint32_t back = enrollBackoffMs(failures, rnd);
  switch (step) {
    case EnrollStep::Announce:
      if (status == 200) return {EnrollStep::Verify, 0, true};
      return {EnrollStep::Announce, back, false};
    case EnrollStep::Verify:
      // Any attempt burns the challenge, so every failure re-announces.
      if (status == 200) return {EnrollStep::Poll, 0, true};
      return {EnrollStep::Announce, back, false};
    case EnrollStep::Poll:
      if (status == 204) return {EnrollStep::Poll, ENROLL_POLL_MS, true};
      if (status == 200) return {EnrollStep::AwaitMtls, 0, true};
      // Pruned after 24 h idle, or reset by an admin: start over.
      if (status == 403) return {EnrollStep::Announce, back, false};
      return {EnrollStep::Poll, back, false};
    case EnrollStep::Ack:
      if (status == 200) return {EnrollStep::Done, 0, true};
      return {EnrollStep::Ack, back, false};
    case EnrollStep::AwaitMtls:
    case EnrollStep::Done:
      break;
  }
  return {step, 0, false};
}
