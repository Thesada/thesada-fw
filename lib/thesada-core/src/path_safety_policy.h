// thesada-fw - path_safety_policy.h
// Pure decision logic for filesystem paths arriving from an operator surface
// (Shell over serial/WS, HTTP /api/file, MQTT CLI). No Arduino deps, so it is
// host-unit-testable. Shell::pathSafe delegates here, and every dynamic
// filesystem-open site on the allow-list in scripts/check-path-safety.sh is
// safe because of this rule and nothing else.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string.h>

// Absolute, no traversal, no empty segment. "//" is rejected on its own and
// not only as a traversal helper: a collapsed empty segment lets two spellings
// of one path disagree, and the allow-list assumes one spelling per file.
inline bool pathSafePolicy(const char* path) {
  if (!path || !*path) return false;
  if (path[0] != '/') return false;
  if (strstr(path, "..") != nullptr) return false;
  if (strstr(path, "//") != nullptr) return false;
  return true;
}
