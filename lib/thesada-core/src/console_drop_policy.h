// thesada-fw - console_drop_policy.h
// Console line sink that never waits on a stalled USB host: a line is written
// whole when the TX room fits it, else dropped whole and counted. Pure, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Longest framed line (text plus CRLF). Under the 256 B HWCDC TX ring.
static const size_t CONSOLE_FRAME_MAX = 240;

// Wait for TX room once per stall. A reading host drains 256 B in about 4 ms.
static const uint32_t CONSOLE_GRACE_MS = 10;

// Buffers live here, not on the logging task's stack. One state per sink,
// used under the sink's lock.
struct ConsoleDropState {
  uint32_t dropped = 0;
  char frame[CONSOLE_FRAME_MAX] = {};
  char note[48] = {};
};

// Write buf only when the TX room fits it whole. out: true when all n bytes went.
template <class Port>
bool consoleTryWrite(Port& port, const char* buf, size_t n) {
  const int room = port.availableForWrite();
  if (room < 0 || (size_t)room < n) return false;
  return port.write(reinterpret_cast<const uint8_t*>(buf), n) == n;
}

// Port: int availableForWrite(), size_t write(const uint8_t*, size_t),
// uint32_t nowMs(), void sleepMs(uint32_t).
// A stall costs at most CONSOLE_GRACE_MS once, then each line drops at once
// until room returns. The first line after it is "console.dropped n=<count>".
// in: port, drop state, NUL-terminated line. out: true when the line was written.
template <class Port>
bool consoleWriteLine(Port& port, ConsoleDropState& st, const char* line) {
  if (st.dropped > 0) {
    char* note = st.note;
    const int k = snprintf(note, sizeof(st.note), "[WRN][Console] console.dropped n=%lu\r\n",
                           (unsigned long)st.dropped);
    if (k <= 0 || !consoleTryWrite(port, note, (size_t)k)) {
      st.dropped++;
      return false;
    }
    st.dropped = 0;
  }
  char* frame = st.frame;
  size_t n = strnlen(line, CONSOLE_FRAME_MAX - 2);
  memcpy(frame, line, n);
  frame[n++] = '\r';
  frame[n++] = '\n';
  if (consoleTryWrite(port, frame, n)) return true;
  if (st.dropped == 0) {
    const uint32_t t0 = port.nowMs();
    while ((uint32_t)(port.nowMs() - t0) < CONSOLE_GRACE_MS) {
      port.sleepMs(1);
      if (consoleTryWrite(port, frame, n)) return true;
    }
  }
  st.dropped++;
  return false;
}
