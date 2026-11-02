// thesada-fw - sd_log_policy.h
// SD log slot map built from one root listing. SDModule resumes the highest
// /logNNN.csv and opens the lowest free slot; probing all 999 names with
// exists() took ~29 s on a 694-file card and tripped the 30 s TWDT.
// Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>

static const int SD_LOG_MAX_INDEX = 999;

struct SdLogSlots {
  uint8_t used[(SD_LOG_MAX_INDEX + 1 + 7) / 8];  // bit i: log<i>.csv exists
  uint8_t file[(SD_LOG_MAX_INDEX + 1 + 7) / 8];  // bit i: and is a regular file
};

// 1..999 for a "logNNN.csv" basename, any case (FAT matches names that way),
// else 0. A leading path is stripped.
inline int sdLogIndex(const char* name) {
  if (!name) return 0;
  const char* slash = strrchr(name, '/');
  const char* b = slash ? slash + 1 : name;
  if (strlen(b) != 10) return 0;
  static const char pre[] = "log";
  static const char suf[] = ".csv";
  for (int i = 0; i < 3; i++)
    if ((b[i] | 0x20) != pre[i]) return 0;
  for (int i = 0; i < 4; i++)
    if ((b[6 + i] | (i ? 0x20 : 0)) != suf[i]) return 0;
  int n = 0;
  for (int i = 3; i < 6; i++) {
    if (b[i] < '0' || b[i] > '9') return 0;
    n = n * 10 + (b[i] - '0');
  }
  return n;
}

inline void sdLogSlotsClear(SdLogSlots& s) {
  memset(s.used, 0, sizeof(s.used));
  memset(s.file, 0, sizeof(s.file));
}

inline bool sdLogSlotsHas(const SdLogSlots& s, int i) {
  return i >= 1 && i <= SD_LOG_MAX_INDEX && (s.used[i >> 3] & (1u << (i & 7)));
}

// A directory with a log name still takes its slot, but is never resumed.
inline void sdLogSlotsAdd(SdLogSlots& s, const char* name, bool isDir = false) {
  int i = sdLogIndex(name);
  if (!i) return;
  s.used[i >> 3] |= (uint8_t)(1u << (i & 7));
  if (!isDir) s.file[i >> 3] |= (uint8_t)(1u << (i & 7));
}

// Highest index held by a regular file, 0 when there is none to resume.
inline int sdLogSlotsHighest(const SdLogSlots& s) {
  for (int i = SD_LOG_MAX_INDEX; i >= 1; i--)
    if (s.file[i >> 3] & (1u << (i & 7))) return i;
  return 0;
}

// Lowest free index >= from (clamped to 1), 0 when none is left. No wrap.
inline int sdLogSlotsNextFree(const SdLogSlots& s, int from) {
  for (int i = from < 1 ? 1 : from; i <= SD_LOG_MAX_INDEX; i++)
    if (!sdLogSlotsHas(s, i)) return i;
  return 0;
}
