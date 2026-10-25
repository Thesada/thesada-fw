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

// Crash resets that abandon the running image. Power-on and deep sleep clear,
// brownout holds, and one OTA reboot stays under the limit.
#define OTA_HEALTH_CRASH_LIMIT 3u

// A valid image this long up clears its streak, so restarts weeks apart do
// not add up. A loop that crashes sooner than this still reaches the limit.
#define OTA_HEALTH_SETTLE_MS (60u * 60u * 1000u)

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

// Numeric values of esp_ota_img_states_t, for the same reason.
#define OTA_IMG_PENDING_VERIFY 0x1u
#define OTA_IMG_VALID          0x2u
#define OTA_IMG_UNDEFINED      0xFFFFFFFFu

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

// RTC words a planned restart leaves for the next boot: an operator restart,
// or a reboot from the MQTT budget. Any other value reads as unplanned.
#define OTA_PLANNED_RESTART_MAGIC 0x504C4E44u
#define OTA_PLANNED_BROKER_MAGIC  0x4D515454u

enum OtaPlanned : uint8_t {
  OTA_PLANNED_NONE     = 0,
  OTA_PLANNED_OPERATOR = 1,
  OTA_PLANNED_BROKER   = 2,
};

// Read the planned-restart marker and clear it, so it covers one boot only.
// in: marker word. out: which restart left it, or none.
inline OtaPlanned otaHealthTakePlanned(uint32_t* marker) {
  OtaPlanned planned = OTA_PLANNED_NONE;
  if (*marker == OTA_PLANNED_RESTART_MAGIC) planned = OTA_PLANNED_OPERATOR;
  else if (*marker == OTA_PLANNED_BROKER_MAGIC) planned = OTA_PLANNED_BROKER;
  *marker = 0;
  return planned;
}

// A USB flash leaves its slot UNDEFINED, which the bootloader never rolls
// back, so it counts as confirmed. in: otadata state. out: confirmed.
inline bool otaHealthStateConfirmed(uint32_t state) {
  return state == OTA_IMG_VALID || state == OTA_IMG_UNDEFINED;
}

// Operator restarts, and MQTT budget reboots of a proven image, hold on a
// confirmed image. in: reason, marker, confirmed, proven. out: reset class.
inline OtaResetClass otaHealthBootClass(int reason, OtaPlanned planned, bool confirmed,
                                        bool proven) {
  if (reason == OTA_RST_SW && confirmed &&
      (planned == OTA_PLANNED_OPERATOR || (planned == OTA_PLANNED_BROKER && proven))) {
    return OTA_RESET_HOLD;
  }
  return otaHealthResetClass(reason);
}

// Proven-on-broker record: the slot address, little-endian, then the app ELF
// sha256. A new OTA image differs in one or both.
#define OTA_IMAGE_ID_LEN 36u

// Build the running image's id. An all-zero hash would match every build.
// in: slot address, 32-byte ELF hash, id out. out: false when the hash is empty.
inline bool otaHealthImageId(uint32_t slotAddr, const uint8_t* elfSha, uint8_t* id) {
  uint8_t any = 0;
  for (size_t i = 0; i < 32; i++) any |= elfSha[i];
  if (any == 0) return false;
  for (size_t i = 0; i < 4; i++) id[i] = (uint8_t)(slotAddr >> (8 * i));
  memcpy(id + 4, elfSha, 32);
  return true;
}

// True only when the stored record names this exact image.
// in: stored record and its length, running id. out: proven on the broker.
inline bool otaHealthImageProven(const uint8_t* stored, size_t storedLen, const uint8_t* id) {
  if (!stored || !id || storedLen != OTA_IMAGE_ID_LEN) return false;
  return memcmp(stored, id, OTA_IMAGE_ID_LEN) == 0;
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

// Clear a valid image's streak once it has run OTA_HEALTH_SETTLE_MS, and again
// that long after each failed clear. Uptime is 64-bit: millis() wraps at 49.7 days.
// in: pending, uptime ms, streak, failed clears this boot. out: true to write 0.
inline bool otaHealthShouldClearValid(bool pending, uint64_t uptimeMs, uint32_t crashStreak,
                                      uint32_t failedClears = 0) {
  if (pending || crashStreak == 0) return false;
  return uptimeMs / OTA_HEALTH_SETTLE_MS > failedClears;
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
