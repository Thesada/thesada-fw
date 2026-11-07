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
  char   data[1];  // cap bytes + NUL
};

// Largest file /api/file writes and the dashboard editor loads (reads stream any
// size). Bodies past 4 KB malloc into PSRAM on every shipped board, after auth.
static const size_t HTTP_FILE_MAX = 65536;

// Body caps per endpoint, bytes.
static const size_t HTTP_BODY_CFG_MAX  = 16384;
static const size_t HTTP_BODY_CMD_MAX  = 2048;
static const size_t HTTP_BODY_FILE_MAX = HTTP_FILE_MAX;

// in: declared total, cap. out: a buffer for exactly total bytes, or nullptr
// when total is 0, past the cap, or the alloc fails.
inline HttpBody* httpBodyBegin(size_t total, size_t maxBytes) {
  if (total == 0 || total > maxBytes) return nullptr;
  HttpBody* b = (HttpBody*)malloc(sizeof(HttpBody) + total);
  if (!b) return nullptr;
  b->len = 0;
  b->cap = total;
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
  HTTP_BODY_LENGTH_REQUIRED,    // 411, chunked: nothing was buffered
  HTTP_BODY_TOO_LARGE,          // 413
  HTTP_BODY_INCOMPLETE,         // 400, body did not arrive whole
};

// A chunked body has Content-Length 0 and onBody total 0, so it is never
// buffered. Checked before the empty case, or it would read as an empty body.
inline HttpBodyVerdict httpBodyVerdict(bool authed, bool chunked, size_t contentLength,
                                       size_t maxBytes, const HttpBody* b) {
  if (!authed) return HTTP_BODY_UNAUTHORIZED;
  if (chunked) return HTTP_BODY_LENGTH_REQUIRED;
  if (contentLength == 0) return HTTP_BODY_EMPTY;
  if (contentLength > maxBytes) return HTTP_BODY_TOO_LARGE;
  if (!httpBodyComplete(b)) return HTTP_BODY_INCOMPLETE;
  return HTTP_BODY_OK;
}
