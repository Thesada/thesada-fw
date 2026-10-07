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
  RUN_TEST(test_abandoned_image_matches_version_date_and_time);
  return UNITY_END();
}
