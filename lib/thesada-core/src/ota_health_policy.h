// thesada-fw - ota_health_policy.h
// When a pending OTA image may be marked valid. Host-unit-testable.
// See docs/invariants.md.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include <string.h>

// A boot that never reaches this, and never connects, is rolled back by the
// bootloader on the next reset. Five minutes covers a broker that is down.
#define OTA_HEALTH_MIN_MS (5u * 60u * 1000u)

// Panic, watchdog, and software resets. At the limit the running image is
// abandoned. Power-on and deep sleep clear the streak. One software reset
// stays under the limit, so the OTA reboot can still confirm. Brownout holds.
#define OTA_HEALTH_CRASH_LIMIT 3u

// Numeric values of esp_reset_reason_t. Kept here so the host test does not
// include the IDF header.
#define OTA_RST_UNKNOWN   0
#define OTA_RST_POWERON   1
#define OTA_RST_EXT       2
#define OTA_RST_SW        3
#define OTA_RST_PANIC     4
#define OTA_RST_INT_WDT   5
#define OTA_RST_TASK_WDT  6
#define OTA_RST_WDT       7
#define OTA_RST_DEEPSLEEP 8
#define OTA_RST_BROWNOUT  9
#define OTA_RST_SDIO      10

enum OtaResetClass : uint8_t {
  OTA_RESET_CLEAR = 0,
  OTA_RESET_HOLD  = 1,
  OTA_RESET_CRASH = 2,
};

// Classify one reset. Power-on and deep sleep clear. Software reset counts
// with panic and the watchdogs, so a restart loop reaches the limit.
// in: esp_reset_reason value. out: clear, hold, or crash.
inline OtaResetClass otaHealthResetClass(int reason) {
  switch (reason) {
    case OTA_RST_POWERON:
    case OTA_RST_DEEPSLEEP:
      return OTA_RESET_CLEAR;
    case OTA_RST_SW:
    case OTA_RST_PANIC:
    case OTA_RST_INT_WDT:
    case OTA_RST_TASK_WDT:
    case OTA_RST_WDT:
      return OTA_RESET_CRASH;
    default:
      return OTA_RESET_HOLD;
  }
}

// Next streak after this boot. in: previous, class. out: new streak.
inline uint32_t otaHealthNextCrashStreak(uint32_t previous, OtaResetClass kind) {
  if (kind == OTA_RESET_CLEAR) return 0;
  if (kind == OTA_RESET_CRASH) return previous + 1;
  return previous;
}

// Mark a pending image after MQTT connects or OTA_HEALTH_MIN_MS up.
// A streak at the limit never confirms. in: pending, mqtt up, uptime ms,
// crash streak. out: true to confirm.
inline bool otaHealthShouldMark(bool pending, bool mqttUp, uint32_t uptimeMs,
                                uint32_t crashStreak) {
  if (!pending) return false;
  if (crashStreak >= OTA_HEALTH_CRASH_LIMIT) return false;
  if (mqttUp) return true;
  return uptimeMs >= OTA_HEALTH_MIN_MS;
}

// Abandon the running image once the crash streak hits the limit.
// in: crash streak. out: true to boot the other image.
inline bool otaHealthShouldRollback(uint32_t crashStreak) {
  return crashStreak >= OTA_HEALTH_CRASH_LIMIT;
}

// The slot we last left, matched on the image's own version, date, and time.
// An empty bad version means nothing has been left. A new build differs in
// date or time, so it can be selected.
// in: candidate and recorded identity. out: true if this is the image we left.
inline bool otaHealthIsAbandoned(const char* version, const char* date, const char* time,
                                 const char* badVersion, const char* badDate,
                                 const char* badTime) {
  if (!badVersion || badVersion[0] == '\0') return false;
  if (!version || !date || !time || !badDate || !badTime) return false;
  return strcmp(version, badVersion) == 0 && strcmp(date, badDate) == 0 &&
         strcmp(time, badTime) == 0;
}
