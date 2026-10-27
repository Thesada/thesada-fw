// thesada-fw - http_url_policy.h
// URL split and status-line parse for the hand-rolled HTTP clients (OTA GET,
// Telegram and webhook POST). Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Split an http(s) URL. in: url, host cap. out: host, path, port, https.
inline bool httpUrlSplit(const char* url, char* host, size_t hostCap,
                         const char** pathOut, uint16_t* portOut, bool* httpsOut = nullptr) {
  if (!url || !host || hostCap == 0) return false;
  const char* p = url;
  uint16_t port = 443;
  bool https = true;
  if (strncmp(p, "https://", 8) == 0)      { p += 8; port = 443; }
  else if (strncmp(p, "http://",  7) == 0) { p += 7; port = 80; https = false; }
  else return false;
  const char* slash   = strchr(p, '/');
  const char* hostEnd = slash ? slash : p + strlen(p);
  size_t hostLen      = hostEnd - p;
  if (hostLen == 0 || hostLen >= hostCap) return false;
  memcpy(host, p, hostLen);
  host[hostLen] = '\0';
  char* colon = strchr(host, ':');
  if (colon) {
    *colon = '\0';
    char* end = nullptr;
    const long v = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end != '\0' || v < 1 || v > 65535) return false;
    port = (uint16_t)v;
  }
  if (host[0] == '\0') return false;
  *pathOut = slash ? slash : "/";
  *portOut = port;
  if (httpsOut) *httpsOut = https;
  return true;
}

// in: an HTTP status line ("HTTP/1.1 200 OK"). out: the code, or -1 if malformed.
inline int httpStatusCode(const char* line) {
  if (!line || strncmp(line, "HTTP/", 5) != 0) return -1;
  const char* sp = strchr(line, ' ');
  if (!sp) return -1;
  char* end = nullptr;
  const long code = strtol(sp + 1, &end, 10);
  if (end != sp + 4) return -1;
  return (code >= 100 && code <= 599) ? (int)code : -1;
}
