// thesada-fw - mqtt_topic_policy.h
// Pure topic and broker-host copies: what does not fit is refused, never clipped.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// MQTTClient's broker host buffer, NUL included. Enrollment stores no longer host.
#define MQTT_BROKER_HOST_CAP 96

// in: host, cap. out: true when host and its NUL fit cap bytes.
inline bool mqttHostFits(const char* host, size_t cap) {
  return host && strnlen(host, cap) < cap;
}

// Copy host into out[cap] only when all of it fits. out: false when it does
// not, and out keeps what it held, so a reinit keeps the previous broker.
inline bool mqttHostCopy(char* out, size_t cap, const char* host) {
  if (!out || !mqttHostFits(host, cap)) return false;
  memcpy(out, host, strlen(host) + 1);
  return true;
}

// Format a topic into out[cap]. out: true when it fitted, else out holds ""
// so a caller that skips the check has no clipped topic to publish.
inline bool mqttTopicVfmt(char* out, size_t cap, const char* fmt, va_list ap) {
  if (!out || cap == 0) return false;
  if (!fmt) { out[0] = '\0'; return false; }
  // Forwarded va_list, va_start'd by the caller. The linux analyser cannot see that.
  // NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized)
  int n = vsnprintf(out, cap, fmt, ap);
  if (n <= 0 || (size_t)n >= cap) { out[0] = '\0'; return false; }
  return true;
}

inline bool mqttTopicFmt(char* out, size_t cap, const char* fmt, ...)  // NOLINT(cert-dcl50-cpp): printf wrapper
    __attribute__((format(printf, 3, 4)));

inline bool mqttTopicFmt(char* out, size_t cap, const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp): printf wrapper
  va_list ap;
  va_start(ap, fmt);
  bool fit = mqttTopicVfmt(out, cap, fmt, ap);
  va_end(ap);
  return fit;
}

// in: topic, cap. out: true for a non-empty topic that fits cap with its NUL,
// so a copy into a cap-byte slot is checked first rather than clipped.
inline bool mqttTopicFits(const char* topic, size_t cap) {
  return topic && *topic && strnlen(topic, cap) < cap;
}

// Released 26.11.1 built HA discovery uids in 48 bytes, so a long slug clipped.
#define HA_LEGACY_UID_CAP 48

// in: uid as built now. out: true when 26.11.1 clipped it, legacy then holds
// the uid that release published under, else legacy is "".
inline bool haLegacyUid(char* legacy, size_t cap, const char* uid) {
  if (!legacy || cap == 0) return false;
  legacy[0] = '\0';
  if (!uid || strnlen(uid, cap) < cap) return false;
  memcpy(legacy, uid, cap - 1);
  legacy[cap - 1] = '\0';
  return true;
}
