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

// Body caps per endpoint, bytes.
static const size_t HTTP_BODY_CFG_MAX  = 16384;
static const size_t HTTP_BODY_CMD_MAX  = 2048;
static const size_t HTTP_BODY_FILE_MAX = 16384;

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
