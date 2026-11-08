// thesada-fw - alert_payload_policy.h
// How much of an alert message fits the MQTT alert slot as the app's alert JSON.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// Bytes one character takes inside a JSON string, as ArduinoJson writes it.
inline size_t alertJsonCharLen(unsigned char c) {
  if (c == '"' || c == '\\' || c == '\b' || c == '\f' || c == '\n' || c == '\r' || c == '\t') return 2;
  if (c < 0x20) return 6;
  return 1;
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
