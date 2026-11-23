// thesada-fw - cell_at_policy.h
// Modem AT bus: console lines nested inside a held exchange, the raw line
// reader and the socket URCs it hands back. Pure, host-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// in: console pumps running inside a held exchange, bus holder, calling task.
// out: refuse the bus. The mutex is recursive, so the holder re-takes it freely.
inline bool cellAtNestedRefused(unsigned heldPumps, const void* holder, const void* self) {
  return heldPumps > 0 && holder != nullptr && holder == self;
}

// One modem line, CR dropped. A line past N-1 bytes is dropped whole.
template <size_t N>
struct CellLine {
  char   buf[N] = {};
  size_t len    = 0;
  bool   over   = false;

  // in: one byte. out: true when buf holds a whole, non-empty line.
  bool feed(char c) {
    if (c == '\r') return false;
    if (c == '\n') {
      const bool whole = len > 0 && !over;
      buf[len] = '\0';
      len      = 0;
      over     = false;
      return whole;
    }
    if (len < N - 1) buf[len++] = c;
    else             over = true;
    return false;
  }
};

enum : uint8_t { CELL_URC_NONE = 0, CELL_URC_DATA = 1, CELL_URC_CLOSED = 2 };
struct CellUrc {
  uint8_t kind;
  uint8_t mux;
};

// Parses a decimal at p into *v. out: the byte after it, or nullptr.
inline const char* cellAtInt(const char* p, long* v) {
  while (*p == ' ') ++p;
  if (*p < '0' || *p > '9') return nullptr;
  char* end = nullptr;
  *v = strtol(p, &end, 10);
  return end;
}

// in: one modem line, socket slots. out: the socket event it reports.
// +CADATAIND: <mux> is data waiting, +CASTATE: <mux>,<state> other than 1 a close.
inline CellUrc cellSocketUrc(const char* line, uint8_t slots) {
  const CellUrc none = {CELL_URC_NONE, 0};
  if (!line) return none;
  long        mux = -1, state = -1;
  const char* p   = nullptr;
  uint8_t     kind;
  if (strncmp(line, "+CADATAIND:", 11) == 0) {
    p = cellAtInt(line + 11, &mux);
    if (!p || *p != '\0') return none;
    kind = CELL_URC_DATA;
  } else if (strncmp(line, "+CASTATE:", 9) == 0) {
    p = cellAtInt(line + 9, &mux);
    if (!p || *p != ',' || !cellAtInt(p + 1, &state)) return none;
    if (state == 1) return none;
    kind = CELL_URC_CLOSED;
  } else {
    return none;
  }
  if (mux < 0 || mux >= slots) return none;
  const CellUrc u = {kind, (uint8_t)mux};
  return u;
}

// in: one modem line. out: the +CNTP: result code, 0 when it has none,
// -1 for another line.
inline int cellNtpCode(const char* line) {
  const char* p = line ? strstr(line, "+CNTP:") : nullptr;
  if (!p) return -1;
  long code = 0;
  return cellAtInt(p + 6, &code) ? (int)code : 0;
}

// in: one modem line, out buffer. out: true with the first address of a
// +CDNSGIP: 1,"host","ip" line. An address that does not fit is refused.
inline bool cellDnsgipIp(const char* line, char* out, size_t outLen) {
  const char* p = line ? strstr(line, "+CDNSGIP:") : nullptr;
  if (!p || !out || outLen == 0) return false;
  long status = 0;
  p = cellAtInt(p + 9, &status);
  if (!p || status != 1 || *p != ',') return false;
  const char* q1 = strchr(p, '"');
  const char* q2 = q1 ? strchr(q1 + 1, '"') : nullptr;
  const char* q3 = q2 ? strchr(q2 + 1, '"') : nullptr;
  const char* q4 = q3 ? strchr(q3 + 1, '"') : nullptr;
  if (!q4) return false;
  const size_t n = (size_t)(q4 - q3 - 1);
  if (n == 0 || n >= outLen) return false;
  memcpy(out, q3 + 1, n);
  out[n] = '\0';
  return true;
}
