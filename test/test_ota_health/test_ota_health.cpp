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
  const uint64_t h = OTA_HEALTH_SETTLE_MS;
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, h, 2, h));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 2 * h - 1, 2, h));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 2 * h, 2, h));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 2 * h, 2, 2 * h));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 3 * h, 2, 2 * h));
}

// A first failure late in uptime waits a full hour from that failure.
void test_late_failed_clear_waits_an_hour_from_the_failure(void) {
  const uint64_t failedAt = 2ULL * OTA_HEALTH_SETTLE_MS;
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, failedAt + 1, 2, failedAt));
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, failedAt + OTA_HEALTH_SETTLE_MS - 1, 2, failedAt));
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, failedAt + OTA_HEALTH_SETTLE_MS, 2, failedAt));
}

// 1194 h is past the 49.7-day wrap of a 32-bit millis().
void test_clear_retry_survives_the_millis_wrap(void) {
  const uint64_t h = OTA_HEALTH_SETTLE_MS;
  TEST_ASSERT_TRUE(otaHealthShouldClearValid(false, 1194 * h, 2, 1193 * h));
  // Negative control: a failure later in that hour waits a full hour from it.
  TEST_ASSERT_FALSE(otaHealthShouldClearValid(false, 1194 * h, 2, 1193 * h + 1));
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
  const uint64_t h = OTA_HEALTH_SETTLE_MS;
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 2 * h - 1, h));
  TEST_ASSERT_TRUE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 2 * h, h));
  TEST_ASSERT_TRUE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 1194 * h, 1193 * h));
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, 1194 * h, 1193 * h + 1));
}

void test_late_failed_usb_mark_waits_an_hour_from_the_failure(void) {
  const uint64_t failedAt = 2ULL * OTA_HEALTH_SETTLE_MS + 5000;
  TEST_ASSERT_FALSE(otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, failedAt + 1, failedAt));
  TEST_ASSERT_FALSE(
      otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, failedAt + OTA_HEALTH_SETTLE_MS - 1, failedAt));
  TEST_ASSERT_TRUE(
      otaHealthShouldMarkUsbValid(OTA_IMG_UNDEFINED, true, failedAt + OTA_HEALTH_SETTLE_MS, failedAt));
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

void test_abandoned_image_matches_version_date_and_time(void) {
  TEST_ASSERT_FALSE(otaHealthIsAbandoned("1", "Oct  6 2026", "09:00:00", "", "", ""));
  TEST_ASSERT_TRUE(otaHealthIsAbandoned("1", "Oct  6 2026", "09:00:00",
                                        "1", "Oct  6 2026", "09:00:00"));
  TEST_ASSERT_FALSE(otaHealthIsAbandoned("1", "Oct  6 2026", "09:00:00",
                                         "1", "Oct  6 2026", "10:00:00"));
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
  RUN_TEST(test_late_failed_clear_waits_an_hour_from_the_failure);
  RUN_TEST(test_clear_retry_survives_the_millis_wrap);
  RUN_TEST(test_usb_image_is_marked_valid_after_the_settle_hour);
  RUN_TEST(test_failed_usb_mark_retries_an_hour_later);
  RUN_TEST(test_late_failed_usb_mark_waits_an_hour_from_the_failure);
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
  RUN_TEST(test_abandoned_image_matches_version_date_and_time);
  return UNITY_END();
}
