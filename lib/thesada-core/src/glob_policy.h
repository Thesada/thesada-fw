// thesada-fw - glob_policy.h
// Wildcard matching and the safety rules for wildcard fs.rm. Pure, so the
// match and the protected-file veto are host-testable without a filesystem.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

inline bool globHasWildcard(const char* s) {
  return s && (strchr(s, '*') || strchr(s, '?'));
}

// Iterative matcher with one backtrack point, so a greedy star gives ground
// rather than swallowing the tail. No recursion: this runs on a 4 KB stack.
inline bool globMatch(const char* pat, const char* name) {
  if (!pat || !name) return false;
  const char* star = nullptr;
  const char* mark = nullptr;
  while (*name) {
    if (*pat == '?' || *pat == *name) { pat++; name++; continue; }
    if (*pat == '*') { star = pat++; mark = name; continue; }
    if (star) { pat = star + 1; name = ++mark; continue; }
    return false;
  }
  while (*pat == '*') pat++;
  return *pat == '\0';
}

// Split "/sd/logs/*.csv" into "/sd/logs" + "*.csv". The wildcard may live in
// the last segment only; anywhere else would match across directories, which
// this does not support and must not silently treat as a literal.
inline bool globSplit(const char* path, char* dirOut, size_t dirCap,
                      char* patOut, size_t patCap) {
  if (!path || !dirOut || !patOut || dirCap == 0 || patCap == 0) return false;
  const char* slash = strrchr(path, '/');
  if (!slash) return false;

  const size_t patLen = strlen(slash + 1);
  if (patLen == 0 || patLen >= patCap) return false;

  // Everything before the last slash must be literal.
  for (const char* p = path; p < slash; p++) {
    if (*p == '*' || *p == '?') return false;
  }

  size_t dirLen = (size_t)(slash - path);
  if (dirLen == 0) dirLen = 1;               // pattern sits at the root
  if (dirLen >= dirCap) return false;

  memcpy(dirOut, path, dirLen);
  dirOut[dirLen] = '\0';
  memcpy(patOut, slash + 1, patLen + 1);
  return true;
}

// The two files the firmware needs to boot and to verify TLS. A wildcard rm
// skips them whatever the pattern says; an exact fs.rm still removes them.
inline bool globRmProtected(const char* dir, const char* name) {
  if (!dir || !name) return false;
  if (strcmp(dir, "/") != 0) return false;
  return strcmp(name, "config.json") == 0 || strcmp(name, "ca.crt") == 0;
}
