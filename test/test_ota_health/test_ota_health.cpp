// Host-native unit tests for ota_health_policy.h.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ota_health_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_a_confirmed_image_is_left_alone(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMark(false, true, OTA_HEALTH_MIN_MS, 0));
  TEST_ASSERT_FALSE(otaHealthShouldMark(false, false, 0, 0));
}

void test_mqtt_confirms_immediately(void) {
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, true, 0, 0));
}

void test_uptime_confirms_without_mqtt(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMark(true, false, OTA_HEALTH_MIN_MS - 1, 0));
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, false, OTA_HEALTH_MIN_MS, 0));
}

void test_power_on_and_deepsleep_clear_the_streak(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_CLEAR, otaHealthResetClass(OTA_RST_POWERON));
  TEST_ASSERT_EQUAL(OTA_RESET_CLEAR, otaHealthResetClass(OTA_RST_DEEPSLEEP));
  TEST_ASSERT_EQUAL_UINT32(0, otaHealthNextCrashStreak(2, OTA_RESET_CLEAR));
}

void test_software_reset_counts_but_one_ota_reboot_can_confirm(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthResetClass(OTA_RST_SW));
  uint32_t streak = otaHealthNextCrashStreak(0, OTA_RESET_CRASH);
  TEST_ASSERT_EQUAL_UINT32(1, streak);
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, true, 0, streak));
  TEST_ASSERT_FALSE(otaHealthShouldRollback(streak));
  streak = otaHealthNextCrashStreak(streak, OTA_RESET_CRASH);
  streak = otaHealthNextCrashStreak(streak, OTA_RESET_CRASH);
  TEST_ASSERT_TRUE(otaHealthShouldRollback(streak));
}

void test_panic_and_watchdogs_increment(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthResetClass(OTA_RST_PANIC));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthResetClass(OTA_RST_INT_WDT));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthResetClass(OTA_RST_TASK_WDT));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthResetClass(OTA_RST_WDT));
  TEST_ASSERT_EQUAL_UINT32(3, otaHealthNextCrashStreak(2, OTA_RESET_CRASH));
}

void test_brownout_and_unknown_hold_the_streak(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthResetClass(OTA_RST_BROWNOUT));
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthResetClass(OTA_RST_UNKNOWN));
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthResetClass(OTA_RST_EXT));
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthResetClass(OTA_RST_SDIO));
  TEST_ASSERT_EQUAL_UINT32(2, otaHealthNextCrashStreak(2, OTA_RESET_HOLD));
}

void test_crash_limit_refuses_confirm_and_rolls_back(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMark(true, true, OTA_HEALTH_MIN_MS, OTA_HEALTH_CRASH_LIMIT));
  TEST_ASSERT_TRUE(otaHealthShouldRollback(OTA_HEALTH_CRASH_LIMIT));
  TEST_ASSERT_FALSE(otaHealthShouldRollback(OTA_HEALTH_CRASH_LIMIT - 1));
}

void test_mqtt_still_confirms_under_the_limit(void) {
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, true, 0, OTA_HEALTH_CRASH_LIMIT - 1));
}

void test_valid_image_clears_its_streak_after_settling(void) {
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS - 1, 2));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS, 2));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS, OTA_HEALTH_CRASH_LIMIT));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS, 0));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(true, OTA_HEALTH_SETTLE_MS, 2));
}

void test_failed_clear_retries_an_hour_later(void) {
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS, 2, 1));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 2u * OTA_HEALTH_SETTLE_MS - 1, 2, 1));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 2u * OTA_HEALTH_SETTLE_MS, 2, 1));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 2u * OTA_HEALTH_SETTLE_MS, 2, 2));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 3u * OTA_HEALTH_SETTLE_MS, 2, 2));
}

// 1194 h is past the 49.7-day wrap of a 32-bit millis().
void test_clear_retry_survives_the_millis_wrap(void) {
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 1194ULL * OTA_HEALTH_SETTLE_MS, 2, 1193));
  // Negative control: one more failed clear waits for the next hour.
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 1194ULL * OTA_HEALTH_SETTLE_MS, 2, 1194));
}

void test_usb_image_is_marked_valid_after_the_settle_hour(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, OTA_HEALTH_SETTLE_MS - 1, 0));
  TEST_ASSERT_TRUE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, OTA_HEALTH_SETTLE_MS, 0));
  // An unread streak may hide a crash loop.
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, false, OTA_HEALTH_SETTLE_MS, 0));
  // Negative control: only an undefined image is marked here.
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_VALID, true, OTA_HEALTH_SETTLE_MS, 0));
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_PENDING_VERIFY, true, OTA_HEALTH_SETTLE_MS, 0));
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(0x0u, true, OTA_HEALTH_SETTLE_MS, 0));  // new
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(0x3u, true, OTA_HEALTH_SETTLE_MS, 0));  // invalid
}

void test_failed_usb_mark_retries_an_hour_later(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 2u * OTA_HEALTH_SETTLE_MS - 1, 1));
  TEST_ASSERT_TRUE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 2u * OTA_HEALTH_SETTLE_MS, 1));
  TEST_ASSERT_TRUE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 1194ULL * OTA_HEALTH_SETTLE_MS, 1193));
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 1194ULL * OTA_HEALTH_SETTLE_MS, 1194));
}

void test_restarts_weeks_apart_never_reach_the_limit(void) {
  uint32_t streak = 0;
  for (int i = 0; i < 10; i++) {
    streak = otaHealthNextCrashStreak(streak, otaHealthResetClass(OTA_RST_SW));
    TEST_ASSERT_FALSE(otaHealthShouldRollback(streak));
    if (otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS, streak)) streak = 0;
  }
}

void test_a_fast_crash_loop_still_reaches_the_limit(void) {
  uint32_t streak = 0;
  for (uint32_t i = 0; i < OTA_HEALTH_CRASH_LIMIT; i++) {
    TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, OTA_HEALTH_SETTLE_MS - 1, streak));
    streak = otaHealthNextCrashStreak(streak, otaHealthResetClass(OTA_RST_PANIC));
  }
  TEST_ASSERT_TRUE(otaHealthShouldRollback(streak));
}

// Boots of a confirmed image after `n` restarts of one kind, from `streak`.
static uint32_t afterRestarts(uint32_t streak, int n, OtaPlanned kind, bool confirmed,
                              bool proven) {
  for (int i = 0; i < n; i++) {
    streak = otaHealthNextCrashStreak(
        streak, otaHealthBootClass(OTA_RST_SW, kind, confirmed, proven));
  }
  return streak;
}

void test_operator_restart_on_a_confirmed_image_holds(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_OPERATOR, true, false));
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_OPERATOR, true, true));
  TEST_ASSERT_EQUAL_UINT32(2, afterRestarts(2, 10, OTA_PLANNED_OPERATOR, true, false));
  // Negative control: the same restarts with no marker reach the limit.
  TEST_ASSERT_TRUE(otaHealthShouldRollback(afterRestarts(2, 1, OTA_PLANNED_NONE, true, false)));
}

void test_proven_image_holds_broker_outage_reboots(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_BROKER, true, true));
  uint32_t streak = afterRestarts(0, 10, OTA_PLANNED_BROKER, true, true);
  TEST_ASSERT_EQUAL_UINT32(0, streak);
  TEST_ASSERT_FALSE(otaHealthShouldRollback(streak));
  // Negative control: the same image not yet proven counts them.
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_BROKER, true, false));
}

void test_unproven_confirmed_image_escapes_on_broker_reboots(void) {
  // OTA reboot into the pending image, then the five-minute confirm.
  uint32_t streak = afterRestarts(0, 1, OTA_PLANNED_OPERATOR, false, false);
  TEST_ASSERT_EQUAL_UINT32(1, streak);
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, false, OTA_HEALTH_MIN_MS, streak));
  streak = 0;
  // A budget reboot every ~30 min never reaches the hour that clears the streak.
  for (uint32_t i = 0; i < OTA_HEALTH_CRASH_LIMIT; i++) {
    TEST_ASSERT_FALSE(otaHealthShouldRollback(streak));
    TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 30u * 60u * 1000u, streak));
    streak = afterRestarts(streak, 1, OTA_PLANNED_BROKER, true, false);
  }
  TEST_ASSERT_TRUE(otaHealthShouldRollback(streak));
  // Negative control: proven on the broker, the same reboots never escape.
  TEST_ASSERT_FALSE(otaHealthShouldRollback(
      afterRestarts(0, OTA_HEALTH_CRASH_LIMIT, OTA_PLANNED_BROKER, true, true)));
}

void test_new_ota_image_starts_unproven(void) {
  uint8_t shaA[32], shaB[32];
  for (int i = 0; i < 32; i++) {
    shaA[i] = (uint8_t)(i + 1);
    shaB[i] = (uint8_t)(i + 1);
  }
  shaB[31] ^= 0x80u;
  uint8_t stored[OTA_IMAGE_ID_LEN], idSame[OTA_IMAGE_ID_LEN], idNew[OTA_IMAGE_ID_LEN],
      idOtherSlot[OTA_IMAGE_ID_LEN];
  TEST_ASSERT_TRUE(otaHealthImageId(0x10000u, shaA, stored));
  TEST_ASSERT_TRUE(otaHealthImageId(0x10000u, shaA, idSame));
  TEST_ASSERT_TRUE(otaHealthImageId(0x10000u, shaB, idNew));
  TEST_ASSERT_TRUE(otaHealthImageId(0x310000u, shaA, idOtherSlot));
  // Negative control: the image the record was written for is proven.
  TEST_ASSERT_TRUE(otaHealthImageProven(stored, OTA_IMAGE_ID_LEN, idSame));
  TEST_ASSERT_FALSE(otaHealthImageProven(stored, OTA_IMAGE_ID_LEN, idNew));
  TEST_ASSERT_FALSE(otaHealthImageProven(stored, OTA_IMAGE_ID_LEN, idOtherSlot));
  TEST_ASSERT_FALSE(otaHealthImageProven(stored, 0, idSame));
  TEST_ASSERT_FALSE(otaHealthImageProven(stored, OTA_IMAGE_ID_LEN - 1, idSame));
  TEST_ASSERT_FALSE(otaHealthImageProven(nullptr, OTA_IMAGE_ID_LEN, idSame));
  uint8_t zero[32] = {0};
  TEST_ASSERT_FALSE(otaHealthImageId(0x10000u, zero, idNew));
}

void test_usb_flashed_image_holds_operator_restarts(void) {
  TEST_ASSERT_TRUE(otaHealthStateConfirmed(OTA_IMG_VALID));
  TEST_ASSERT_TRUE(otaHealthStateConfirmed(OTA_IMG_UNDEFINED));
  TEST_ASSERT_FALSE(otaHealthStateConfirmed(OTA_IMG_PENDING_VERIFY));
  TEST_ASSERT_FALSE(otaHealthStateConfirmed(0x0u));  // new
  TEST_ASSERT_FALSE(otaHealthStateConfirmed(0x3u));  // invalid
  TEST_ASSERT_FALSE(otaHealthStateConfirmed(0x4u));  // aborted
  bool usb = otaHealthStateConfirmed(OTA_IMG_UNDEFINED);
  TEST_ASSERT_EQUAL_UINT32(0, afterRestarts(0, 5, OTA_PLANNED_OPERATOR, usb, false));
  TEST_ASSERT_EQUAL_UINT32(OTA_HEALTH_CRASH_LIMIT,
                           afterRestarts(0, 3, OTA_PLANNED_NONE, usb, false));
}

void test_pending_image_counts_every_restart(void) {
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_OPERATOR, false, true));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_BROKER, false, true));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_NONE, false, true));
  TEST_ASSERT_TRUE(otaHealthShouldRollback(
      afterRestarts(0, OTA_HEALTH_CRASH_LIMIT, OTA_PLANNED_OPERATOR, false, true)));
  // Negative control: the same restarts on a confirmed image hold.
  TEST_ASSERT_FALSE(otaHealthShouldRollback(
      afterRestarts(0, OTA_HEALTH_CRASH_LIMIT, OTA_PLANNED_OPERATOR, true, true)));
}

void test_panic_and_watchdogs_count_even_when_planned(void) {
  const int crashes[] = {OTA_RST_PANIC, OTA_RST_INT_WDT, OTA_RST_TASK_WDT, OTA_RST_WDT};
  for (int reason : crashes) {
    TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(reason, OTA_PLANNED_OPERATOR, true, true));
    TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(reason, OTA_PLANNED_BROKER, true, true));
  }
  // Negative control: a software reset with the same marker holds.
  TEST_ASSERT_EQUAL(OTA_RESET_HOLD, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_OPERATOR, true, true));
}

void test_stale_marker_after_power_on_is_ignored(void) {
  uint32_t marker = OTA_PLANNED_RESTART_MAGIC;
  OtaPlanned planned = otaHealthTakePlanned(&marker);
  // Negative control: the magic itself reads as planned.
  TEST_ASSERT_EQUAL(OTA_PLANNED_OPERATOR, planned);
  TEST_ASSERT_EQUAL(OTA_RESET_CLEAR, otaHealthBootClass(OTA_RST_POWERON, planned, true, true));
  TEST_ASSERT_EQUAL_UINT32(0, marker);
  TEST_ASSERT_EQUAL(OTA_PLANNED_NONE, otaHealthTakePlanned(&marker));
  TEST_ASSERT_EQUAL(OTA_RESET_CRASH, otaHealthBootClass(OTA_RST_SW, OTA_PLANNED_NONE, true, true));
  marker = OTA_PLANNED_BROKER_MAGIC;
  TEST_ASSERT_EQUAL(OTA_PLANNED_BROKER, otaHealthTakePlanned(&marker));
  TEST_ASSERT_EQUAL_UINT32(0, marker);
  marker = OTA_PLANNED_RESTART_MAGIC ^ 1u;
  TEST_ASSERT_EQUAL(OTA_PLANNED_NONE, otaHealthTakePlanned(&marker));
  TEST_ASSERT_EQUAL_UINT32(0, marker);
}

// Two builds on espressif32 6.13.0: the descriptor strings come from the
// prebuilt IDF and match, only the ELF hash differs.
struct TestBuild {
  const char* version;
  const char* date;
  const char* time;
  uint8_t sha[32];
};

static TestBuild testBuild(uint8_t seed) {
  TestBuild b = {"esp-idf: v4.4.7 38eeba213a", "Mar  5 2024", "12:12:53", {0}};
  for (int i = 0; i < 32; i++) b.sha[i] = (uint8_t)(seed + i);
  return b;
}

static const uint32_t SLOT_0 = 0x10000u;
static const uint32_t SLOT_1 = 0x610000u;

// The record a switch arrival writes for the slot it came from.
static void leftRecord(uint32_t slot, const TestBuild& b, uint8_t* rec) {
  TEST_ASSERT_TRUE(otaHealthImageId(slot, b.sha, rec));
}

void test_builds_with_identical_descriptors_are_told_apart(void) {
  TestBuild a = testBuild(1), b = testBuild(2);
  TEST_ASSERT_EQUAL_STRING(a.version, b.version);
  TEST_ASSERT_EQUAL_STRING(a.date, b.date);
  TEST_ASSERT_EQUAL_STRING(a.time, b.time);
  uint8_t rec[OTA_IMAGE_ID_LEN];
  leftRecord(SLOT_0, a, rec);
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, b.sha));
  // Negative control: the build the record was written for matches.
  TEST_ASSERT_TRUE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, a.sha));
}

// Two slots and the left record, driven the way OTAUpdate.cpp drives them.
struct Unit {
  uint32_t addr[2];
  TestBuild img[2];
  int run;
  uint8_t rec[OTA_IMAGE_ID_LEN];
  size_t recLen;
};

static Unit unitOf(uint8_t seed0, uint8_t seed1) {
  Unit u = {{SLOT_0, SLOT_1}, {testBuild(seed0), testBuild(seed1)}, 0, {0}, 0};
  return u;
}

// recordBootStreak's part: drop a stale record, then record an arrival.
static void bootUnit(Unit& u, uint8_t rescue, bool pending, bool sameStrings, bool dropStale) {
  if (dropStale && otaHealthLeftIsStale(u.rec, u.recLen, u.addr[u.run])) u.recLen = 0;
  if (!otaHealthIsArrival(rescue, pending) || !otaHealthRecordArrival(rescue, sameStrings)) return;
  leftRecord(u.addr[1 - u.run], u.img[1 - u.run], u.rec);
  u.recLen = OTA_IMAGE_ID_LEN;
}

static bool otherIsTarget(const Unit& u) {
  int o = 1 - u.run;
  return !otaHealthIsAbandoned(u.rec, u.recLen, u.addr[o], u.img[o].sha);
}

static void guardSwitch(Unit& u) {
  TEST_ASSERT_TRUE(otherIsTarget(u));
  u.run = 1 - u.run;
  bootUnit(u, OTA_RESCUE_ID, true, true, true);
}

// An OTA writes the other slot and boots it pending, after a planned restart.
static void install(Unit& u, uint8_t seed, bool dropStale, uint8_t storedRescue = 0) {
  u.run = 1 - u.run;
  u.img[u.run] = testBuild(seed);
  bootUnit(u, otaHealthRescue(storedRescue, OTA_PLANNED_RESTART_MAGIC), true, true, dropStale);
}

void test_ping_pong_stops_after_one_switch(void) {
  Unit u = unitOf(0xA0, 0xB0);
  guardSwitch(u);
  TEST_ASSERT_FALSE(otherIsTarget(u));
  // Power cycles on B do not touch slot 0, so the record holds.
  for (int i = 0; i < 3; i++) bootUnit(u, 0, false, true, true);
  TEST_ASSERT_FALSE(otherIsTarget(u));
}

void test_a_reinstalled_build_is_a_target_again(void) {
  Unit u = unitOf(0xA0, 0xB0);
  guardSwitch(u);
  install(u, 0xA0, true);
  TEST_ASSERT_EQUAL_UINT32(0, u.recLen);
  install(u, 0xD0, true);
  TEST_ASSERT_TRUE(otherIsTarget(u));
  // Negative control: a record that outlives the reinstall refuses A.
  Unit v = unitOf(0xA0, 0xB0);
  guardSwitch(v);
  install(v, 0xA0, false);
  install(v, 0xD0, false);
  TEST_ASSERT_FALSE(otherIsTarget(v));
}

void test_a_boot_of_the_recorded_slot_drops_the_record(void) {
  uint8_t rec[OTA_IMAGE_ID_LEN];
  leftRecord(SLOT_0, testBuild(1), rec);
  TEST_ASSERT_TRUE(otaHealthLeftIsStale(rec, sizeof(rec), SLOT_0));
  // Negative control: the slot a switch went to keeps it.
  TEST_ASSERT_FALSE(otaHealthLeftIsStale(rec, sizeof(rec), SLOT_1));
  TEST_ASSERT_FALSE(otaHealthLeftIsStale(rec, sizeof(rec) - 1, SLOT_0));
  TEST_ASSERT_FALSE(otaHealthLeftIsStale(nullptr, OTA_IMAGE_ID_LEN, SLOT_0));
}

void test_only_a_switch_arrival_writes_the_record(void) {
  TEST_ASSERT_TRUE(otaHealthIsArrival(OTA_RESCUE_ID, true));
  // A failed switch's leftover on the valid image, and no flag at all.
  TEST_ASSERT_FALSE(otaHealthIsArrival(OTA_RESCUE_ID, false));
  TEST_ASSERT_FALSE(otaHealthIsArrival(0, true));
  Unit u = unitOf(0xA0, 0xB0);
  bootUnit(u, OTA_RESCUE_ID, false, true, true);
  TEST_ASSERT_TRUE(otherIsTarget(u));
  // Negative control: the same flag on a pending boot records slot 1.
  bootUnit(u, OTA_RESCUE_ID, true, true, true);
  TEST_ASSERT_FALSE(otherIsTarget(u));
}

void test_a_planned_restart_voids_the_rescue_flag(void) {
  TEST_ASSERT_EQUAL_UINT8(0, otaHealthRescue(OTA_RESCUE_ID, OTA_PLANNED_RESTART_MAGIC));
  TEST_ASSERT_EQUAL_UINT8(0, otaHealthRescue(1, OTA_PLANNED_BROKER_MAGIC));
  // Negative control: a switch restarts without a marker.
  TEST_ASSERT_EQUAL_UINT8(OTA_RESCUE_ID, otaHealthRescue(OTA_RESCUE_ID, 0));
  TEST_ASSERT_EQUAL_UINT8(1, otaHealthRescue(1, OTA_PLANNED_RESTART_MAGIC ^ 1u));
  // A flag a failed switch left set does not make the next install an arrival.
  Unit u = unitOf(0xA0, 0xB0);
  install(u, 0xD0, true, OTA_RESCUE_ID);
  TEST_ASSERT_EQUAL_UINT32(0, u.recLen);
  TEST_ASSERT_TRUE(otherIsTarget(u));
}

void test_an_older_guard_arrival_is_recorded_across_platforms_only(void) {
  // Same strings: the older guard's own record already refuses this image.
  TEST_ASSERT_FALSE(otaHealthRecordArrival(1, true));
  TEST_ASSERT_TRUE(otaHealthRecordArrival(1, false));
  // Negative control: this guard's own flag records either way.
  TEST_ASSERT_TRUE(otaHealthRecordArrival(OTA_RESCUE_ID, true));
}

void test_same_build_in_the_same_slot_matches(void) {
  TestBuild a = testBuild(7);
  uint8_t rec[OTA_IMAGE_ID_LEN];
  leftRecord(SLOT_1, a, rec);
  TEST_ASSERT_TRUE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_1, a.sha));
  // The record names one slot and one build, like the proven record.
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, a.sha));
  uint8_t flipped[OTA_IMAGE_ID_LEN];
  memcpy(flipped, rec, sizeof(rec));
  flipped[OTA_IMAGE_ID_LEN - 1] ^= 0x01u;
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(flipped, sizeof(flipped), SLOT_1, a.sha));
}

void test_old_format_record_does_not_block_a_switch(void) {
  TestBuild a = testBuild(3);
  // A unit with only left_ver/left_date/left_time reads no left_id.
  uint8_t none[OTA_IMAGE_ID_LEN] = {0};
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(none, 0, SLOT_1, a.sha));
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(nullptr, 0, SLOT_1, a.sha));
  // The old strings fed in as a record are still not a match.
  const char* legacy = "esp-idf: v4.4.7 38eeba213a";
  TEST_ASSERT_FALSE(otaHealthIsAbandoned((const uint8_t*)legacy, strlen(legacy) + 1, SLOT_1,
                                         a.sha));
  uint8_t rec[OTA_IMAGE_ID_LEN];
  leftRecord(SLOT_1, a, rec);
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(rec, OTA_IMAGE_ID_LEN - 1, SLOT_1, a.sha));
  // Negative control: the new record for the same image matches.
  TEST_ASSERT_TRUE(otaHealthIsAbandoned(rec, OTA_IMAGE_ID_LEN, SLOT_1, a.sha));
}

void test_a_zero_hash_candidate_is_never_the_left_image(void) {
  uint8_t zero[32] = {0};
  uint8_t rec[OTA_IMAGE_ID_LEN];
  TEST_ASSERT_FALSE(otaHealthImageId(SLOT_0, zero, rec));
  leftRecord(SLOT_0, testBuild(1), rec);
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, zero));
  TEST_ASSERT_FALSE(otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, nullptr));
}

void test_one_record_refuses_exactly_one_candidate(void) {
  uint8_t rec[OTA_IMAGE_ID_LEN];
  leftRecord(SLOT_0, testBuild(5), rec);
  int refused = 0;
  for (uint8_t seed = 0; seed < 16; seed++) {
    TestBuild b = testBuild(seed);
    refused += otaHealthIsAbandoned(rec, sizeof(rec), SLOT_0, b.sha) ? 1 : 0;
    refused += otaHealthIsAbandoned(rec, sizeof(rec), SLOT_1, b.sha) ? 1 : 0;
  }
  TEST_ASSERT_EQUAL_INT(1, refused);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_confirmed_image_is_left_alone);
  RUN_TEST(test_mqtt_confirms_immediately);
  RUN_TEST(test_uptime_confirms_without_mqtt);
  RUN_TEST(test_power_on_and_deepsleep_clear_the_streak);
  RUN_TEST(test_software_reset_counts_but_one_ota_reboot_can_confirm);
  RUN_TEST(test_panic_and_watchdogs_increment);
  RUN_TEST(test_brownout_and_unknown_hold_the_streak);
  RUN_TEST(test_crash_limit_refuses_confirm_and_rolls_back);
  RUN_TEST(test_mqtt_still_confirms_under_the_limit);
  RUN_TEST(test_valid_image_clears_its_streak_after_settling);
  RUN_TEST(test_failed_clear_retries_an_hour_later);
  RUN_TEST(test_clear_retry_survives_the_millis_wrap);
  RUN_TEST(test_usb_image_is_marked_valid_after_the_settle_hour);
  RUN_TEST(test_failed_usb_mark_retries_an_hour_later);
  RUN_TEST(test_restarts_weeks_apart_never_reach_the_limit);
  RUN_TEST(test_a_fast_crash_loop_still_reaches_the_limit);
  RUN_TEST(test_operator_restart_on_a_confirmed_image_holds);
  RUN_TEST(test_proven_image_holds_broker_outage_reboots);
  RUN_TEST(test_unproven_confirmed_image_escapes_on_broker_reboots);
  RUN_TEST(test_new_ota_image_starts_unproven);
  RUN_TEST(test_usb_flashed_image_holds_operator_restarts);
  RUN_TEST(test_pending_image_counts_every_restart);
  RUN_TEST(test_panic_and_watchdogs_count_even_when_planned);
  RUN_TEST(test_stale_marker_after_power_on_is_ignored);
  RUN_TEST(test_builds_with_identical_descriptors_are_told_apart);
  RUN_TEST(test_ping_pong_stops_after_one_switch);
  RUN_TEST(test_a_reinstalled_build_is_a_target_again);
  RUN_TEST(test_a_boot_of_the_recorded_slot_drops_the_record);
  RUN_TEST(test_only_a_switch_arrival_writes_the_record);
  RUN_TEST(test_a_planned_restart_voids_the_rescue_flag);
  RUN_TEST(test_an_older_guard_arrival_is_recorded_across_platforms_only);
  RUN_TEST(test_same_build_in_the_same_slot_matches);
  RUN_TEST(test_old_format_record_does_not_block_a_switch);
  RUN_TEST(test_a_zero_hash_candidate_is_never_the_left_image);
  RUN_TEST(test_one_record_refuses_exactly_one_candidate);
  return UNITY_END();
}
