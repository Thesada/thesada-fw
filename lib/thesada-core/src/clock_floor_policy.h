// thesada-fw - clock_floor_policy.h
// Pure decision logic for the boot clock floor. No NVS/Arduino deps, so it is
// host-unit-testable. The floor guarantees time(nullptr) is always a sane
// lower bound of real time, which lets MQTT TLS validate certificates from
// the very first connect - the pre-NTP setInsecure window is gone (F1).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <time.h>

// Same "clock is real" threshold the rest of MQTTClient uses (Nov 2023).
static const uint32_t CLOCK_FLOOR_SANE_EPOCH = 1700000000UL;
// Re-persist the floor at most daily - bounds NVS wear to one write per day.
static const uint32_t CLOCK_FLOOR_PERSIST_INTERVAL_S = 86400UL;
// __DATE__/__TIME__ are build-machine local time, up to ±14 h from UTC.
// Subtract a day so the derived epoch is a guaranteed lower bound.
static const uint32_t CLOCK_FLOOR_BUILD_MARGIN_S = 86400UL;

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
// Valid for all Gregorian dates; only called with y >= 2023 here.
inline int64_t clockFloorDaysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int      era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153u * (m + (m > 2 ? -3 : 9)) + 2u) / 5u + d - 1u;
  const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

// Parse __DATE__ ("Mmm dd yyyy", day space-padded) + __TIME__ ("hh:mm:ss")
// into an epoch, minus the build margin. Returns 0 on malformed input - the
// caller then falls back to the NVS floor alone.
inline uint32_t clockFloorBuildEpoch(const char* date, const char* time_) {
  if (!date || !time_) return 0;
  static const char* MMM = "JanFebMarAprMayJunJulAugSepOctNovDec";
  unsigned mon = 0;
  for (unsigned i = 0; i < 12; i++) {
    if (date[0] == MMM[i * 3] && date[1] == MMM[i * 3 + 1] && date[2] == MMM[i * 3 + 2]) {
      mon = i + 1;
      break;
    }
  }
  if (mon == 0 || date[3] != ' ') return 0;
  // Day: "Jul  2" or "Jul 12" - one digit space-padded, or two digits.
  unsigned day = 0;
  if (date[4] == ' ') {
    if (date[5] < '1' || date[5] > '9') return 0;
    day = (unsigned)(date[5] - '0');
  } else {
    if (date[4] < '1' || date[4] > '3' || date[5] < '0' || date[5] > '9') return 0;
    day = (unsigned)(date[4] - '0') * 10u + (unsigned)(date[5] - '0');
  }
  if (day > 31 || date[6] != ' ') return 0;
  int year = 0;
  for (int i = 7; i < 11; i++) {
    if (date[i] < '0' || date[i] > '9') return 0;
    year = year * 10 + (date[i] - '0');
  }
  if (year < 2023) return 0;  // pre-threshold build stamp = malformed
  for (int i = 0; i < 8; i++) {
    char c = time_[i];
    if (i == 2 || i == 5) { if (c != ':') return 0; }
    else if (c < '0' || c > '9') return 0;
  }
  const unsigned hh = (unsigned)(time_[0] - '0') * 10u + (unsigned)(time_[1] - '0');
  const unsigned mm = (unsigned)(time_[3] - '0') * 10u + (unsigned)(time_[4] - '0');
  const unsigned ss = (unsigned)(time_[6] - '0') * 10u + (unsigned)(time_[7] - '0');
  if (hh > 23 || mm > 59 || ss > 60) return 0;
  const int64_t epoch =
      clockFloorDaysFromCivil(year, mon, day) * 86400 + hh * 3600 + mm * 60 + ss;
  if (epoch <= (int64_t)CLOCK_FLOOR_BUILD_MARGIN_S) return 0;
  return (uint32_t)(epoch - CLOCK_FLOOR_BUILD_MARGIN_S);
}

// The floor to enforce: the newer of the persisted floor and the build stamp.
inline uint32_t clockFloorTarget(uint32_t nvsFloor, uint32_t buildEpoch) {
  return nvsFloor > buildEpoch ? nvsFloor : buildEpoch;
}

// Apply iff the floor is sane and the clock sits below it. A synced (or
// already-floored) clock is never moved backwards.
inline bool clockFloorShouldApply(time_t now, uint32_t floor) {
  if (floor < CLOCK_FLOOR_SANE_EPOCH) return false;
  return now < (time_t)floor;
}

// Persist iff the clock is sane and the stored floor is at least a day stale.
// The clock only advances at real-time rate, so a persisted value is always a
// valid lower bound of real time - even across boots that never see NTP.
inline bool clockFloorShouldPersist(time_t now, uint32_t stored) {
  if (now < (time_t)CLOCK_FLOOR_SANE_EPOCH) return false;
  return (uint32_t)now >= stored + CLOCK_FLOOR_PERSIST_INTERVAL_S;
}
