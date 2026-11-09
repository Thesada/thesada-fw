// thesada-fw - config_scalar_policy.h
// How shell config.set types a bare value: bool, 64-bit integer, float or string.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <errno.h>
#include <stdlib.h>
#include <string.h>

enum ConfigScalarKind { CFG_SCALAR_BOOL, CFG_SCALAR_INT, CFG_SCALAR_FLOAT, CFG_SCALAR_STRING };

// An integer past 64 bits or a float past its range stays a string, never a clamped number.
inline ConfigScalarKind configScalarKind(const char* v, bool* b, long long* i, float* f) {
  if (!v || !*v) return CFG_SCALAR_STRING;
  if (strcmp(v, "true") == 0)  { *b = true;  return CFG_SCALAR_BOOL; }
  if (strcmp(v, "false") == 0) { *b = false; return CFG_SCALAR_BOOL; }
  char* end;
  errno = 0;
  const long long lv = strtoll(v, &end, 10);
  if (*end == '\0') {
    if (errno == ERANGE) return CFG_SCALAR_STRING;
    *i = lv;
    return CFG_SCALAR_INT;
  }
  errno = 0;
  const float fv = strtof(v, &end);
  if (*end == '\0' && errno != ERANGE) { *f = fv; return CFG_SCALAR_FLOAT; }
  return CFG_SCALAR_STRING;
}
