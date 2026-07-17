// thesada-fw - log_kv_policy.h
// Pure formatting core for Log::kvf/kvfw/kvfe. No Arduino deps, so the
// truncation guarantee (NUL always within cap) is host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>

// Format into out[cap]; vsnprintf NUL-terminates within cap on any input.
// Returns true when the full string fit, false on truncation or error.
inline bool logKvFormatV(char* out, size_t cap, const char* fmt, va_list ap) {
  if (!out || cap == 0) return false;
  int n = vsnprintf(out, cap, fmt, ap);
  return n >= 0 && (size_t)n < cap;
}

inline bool logKvFormat(char* out, size_t cap, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  bool fit = logKvFormatV(out, cap, fmt, ap);
  va_end(ap);
  return fit;
}
