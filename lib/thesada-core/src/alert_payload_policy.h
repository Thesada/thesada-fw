// thesada-fw - alert_payload_policy.h
// How much of an alert message fits the MQTT alert slot as the app's alert JSON.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// Bytes one character takes inside a JSON string, as ArduinoJson 7 writes it.
// It writes other control bytes raw, so alertScrubControls() runs first.
inline size_t alertJsonCharLen(unsigned char c) {
  if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') return 2;
  return 1;
}

// Replaces the control bytes ArduinoJson would write raw (invalid JSON) with a space.
inline void alertScrubControls(char* s) {
  for (; s && *s; s++) {
    const unsigned char c = (unsigned char)*s;
    if (c < 0x20 && alertJsonCharLen(c) == 1) *s = ' ';
  }
}

// Length of s cut to at most max bytes without splitting a UTF-8 character.
inline size_t alertUtf8Cut(const char* s, size_t max) {
  if (!s) return 0;
  size_t n = strlen(s);
  if (n <= max) return n;
  n = max;
  while (n > 0 && ((unsigned char)s[n] & 0xC0) == 0x80) n--;
  return n;
}

// Escaped length of s[0..n).
inline size_t alertJsonStrLen(const char* s, size_t n) {
  size_t len = 0;
  for (size_t i = 0; i < n; i++) len += alertJsonCharLen((unsigned char)s[i]);
  return len;
}

// in: severity, code, message, slot size including the NUL. out: how many message bytes
// fit {"severity":..,"code":..,"message":..} below cap, cut on a UTF-8 character boundary.
inline size_t alertMessageFit(const char* severity, const char* code, const char* message,
                              size_t cap) {
  if (!severity || !code || !message || cap == 0) return 0;
  const size_t frame = strlen("{\"severity\":\"\",\"code\":\"\",\"message\":\"\"}") +
                       alertJsonStrLen(severity, strlen(severity)) +
                       alertJsonStrLen(code, strlen(code));
  if (frame + 1 > cap) return 0;
  size_t room = cap - 1 - frame;
  size_t n = 0;
  const size_t mlen = strlen(message);
  while (n < mlen) {
    const size_t c = alertJsonCharLen((unsigned char)message[n]);
    if (c > room) break;
    room -= c;
    n++;
  }
  while (n > 0 && n < mlen && ((unsigned char)message[n] & 0xC0) == 0x80) n--;
  return n;
}
