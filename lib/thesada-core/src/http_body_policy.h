// thesada-fw - http_body_policy.h
// Per-request POST body buffer for the async HTTP server. No Arduino deps.
// One malloc'd block, so the request destructor frees it with _tempObject.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct HttpBody {
  size_t len;      // bytes received, SIZE_MAX once a chunk was refused
  size_t cap;      // declared total
  bool   denied;   // chunk 0 failed auth and already counted the guess
  bool   locked;   // ...and the client was locked out at that point
  char   data[1];  // cap bytes + NUL
};

// Largest file /api/file writes and the dashboard editor loads (reads stream any
// size). Bodies past 4 KB malloc into PSRAM on every shipped board, after auth.
static const size_t HTTP_FILE_MAX = 65536;

// Body caps per endpoint, bytes.
static const size_t HTTP_BODY_CFG_MAX  = 16384;
static const size_t HTTP_BODY_CMD_MAX  = 2048;
static const size_t HTTP_BODY_FILE_MAX = HTTP_FILE_MAX;

// Bytes all POST bodies may hold at once: one full file write beside one
// config push. Without it, concurrent authed POSTs each malloc their cap.
static const size_t HTTP_BODY_BUDGET = HTTP_BODY_FILE_MAX + HTTP_BODY_CFG_MAX;

// in: bytes held now, bytes wanted, budget. out: true, and held grows, when it fits.
inline bool httpBodyReserve(size_t& held, size_t want, size_t budget) {
  if (want > budget || held > budget - want) return false;
  held += want;
  return true;
}

// in: bytes held now, bytes one finished request reserved. out: held shrinks.
inline void httpBodyRelease(size_t& held, size_t n) {
  held = n > held ? 0 : held - n;
}

// in: declared total, cap. out: a buffer for exactly total bytes, or nullptr
// when total is 0, past the cap, or the alloc fails.
inline HttpBody* httpBodyBegin(size_t total, size_t maxBytes) {
  if (total == 0 || total > maxBytes) return nullptr;
  HttpBody* b = (HttpBody*)malloc(sizeof(HttpBody) + total);
  if (!b) return nullptr;
  b->len = 0;
  b->cap = total;
  b->denied = false;
  b->locked = false;
  b->data[0] = '\0';
  return b;
}

// out: a refused marker for a request whose chunk 0 failed auth (locked: it was
// already locked out), or nullptr when the alloc fails.
inline HttpBody* httpBodyDenied(bool locked = false) {
  HttpBody* b = (HttpBody*)malloc(sizeof(HttpBody));
  if (!b) return nullptr;
  b->len = SIZE_MAX;
  b->cap = 0;
  b->denied = true;
  b->locked = locked;
  b->data[0] = '\0';
  return b;
}

// in: buffer (may be null), chunk, its offset. out: false, and the buffer
// stays refused, when a chunk is out of order or overruns the declared total.
inline bool httpBodyAppend(HttpBody* b, const uint8_t* data, size_t len, size_t index) {
  if (!b || b->len == SIZE_MAX) return false;
  if (index != b->len || len > b->cap - b->len) {
    b->len = SIZE_MAX;
    return false;
  }
  memcpy(b->data + b->len, data, len);
  b->len += len;
  b->data[b->len] = '\0';
  return true;
}

// True once every declared byte arrived and no chunk was refused.
inline bool httpBodyComplete(const HttpBody* b) {
  return b && b->len == b->cap;
}

enum HttpBodyVerdict {
  HTTP_BODY_OK = 0,             // whole body buffered
  HTTP_BODY_EMPTY,              // Content-Length 0, a legitimate empty body
  HTTP_BODY_UNAUTHORIZED,       // 401
  HTTP_BODY_LENGTH_REQUIRED,    // 411, chunked or no Content-Length: nothing was buffered
  HTTP_BODY_TOO_LARGE,          // 413
  HTTP_BODY_INCOMPLETE,         // 400, body did not arrive whole
};

// A chunked body, or one with no Content-Length, has length 0 and is never
// buffered. Checked before the empty case, or it would read as an empty body.
inline HttpBodyVerdict httpBodyVerdict(bool authed, bool chunked, bool lengthDeclared,
                                       size_t contentLength, size_t maxBytes,
                                       const HttpBody* b) {
  if (!authed) return HTTP_BODY_UNAUTHORIZED;
  if (chunked || !lengthDeclared) return HTTP_BODY_LENGTH_REQUIRED;
  if (contentLength == 0) return HTTP_BODY_EMPTY;
  if (contentLength > maxBytes) return HTTP_BODY_TOO_LARGE;
  if (!httpBodyComplete(b)) return HTTP_BODY_INCOMPLETE;
  return HTTP_BODY_OK;
}

// Bodies ESPAsyncWebServer parses itself, never through onBody or the budget:
// a form, a text/plain `name=` body, a multipart upload. Judged at the end of
// the headers, before any body byte. 1 KB holds the setup form fully escaped.
static const size_t HTTP_FORM_MAX = 1024;

enum HttpParsedBodyVerdict {
  HTTP_PARSED_PASS = 0,         // the library parses nothing, or a form within HTTP_FORM_MAX
  HTTP_PARSED_TOO_LARGE,        // 413, a form past HTTP_FORM_MAX
  HTTP_PARSED_UNSUPPORTED,      // 415, multipart on a route that takes no upload
};

// in: media type without parameters, Content-Length, the library's multipart
// flag, whether this route takes an upload. out: the verdict.
inline HttpParsedBodyVerdict httpParsedBodyVerdict(const char* mediaType, size_t contentLength,
                                                   bool multipart, bool uploadRoute) {
  if (multipart) return uploadRoute ? HTTP_PARSED_PASS : HTTP_PARSED_UNSUPPORTED;
  if (!mediaType) return HTTP_PARSED_PASS;
  // Case-folded, so every spelling the library parses is covered.
  auto is = [mediaType](const char* want, bool prefix) {
    size_t i = 0;
    for (; want[i]; i++) {
      char c = mediaType[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != want[i]) return false;
    }
    return prefix || mediaType[i] == '\0';
  };
  const bool form = is("application/x-www-form-urlencoded", true) || is("text/plain", false);
  if (form && contentLength > HTTP_FORM_MAX) return HTTP_PARSED_TOO_LARGE;
  return HTTP_PARSED_PASS;
}
