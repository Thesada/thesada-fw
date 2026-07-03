// Host-native unit tests for clock_floor_policy.h (the boot clock floor).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "clock_floor_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- clockFloorBuildEpoch -------------------------------------------------

// Known-good reference: "Jan  1 2024" 00:00:00 UTC = 1704067200.
void test_build_epoch_known_date(void) {
  uint32_t e = clockFloorBuildEpoch("Jan  1 2024", "00:00:00");
  TEST_ASSERT_EQUAL_UINT32(1704067200UL - CLOCK_FLOOR_BUILD_MARGIN_S, e);
}

// Two-digit day, mid-year, leap year: "Feb 29 2024" 12:34:56 = 1709210096.
void test_build_epoch_leap_day(void) {
  uint32_t e = clockFloorBuildEpoch("Feb 29 2024", "12:34:56");
  TEST_ASSERT_EQUAL_UINT32(1709210096UL - CLOCK_FLOOR_BUILD_MARGIN_S, e);
}

// Space-padded single-digit day (the actual __DATE__ format).
void test_build_epoch_space_padded_day(void) {
  // "Jul  2 2026" 00:00:00 = 1782950400.
  uint32_t e = clockFloorBuildEpoch("Jul  2 2026", "00:00:00");
  TEST_ASSERT_EQUAL_UINT32(1782950400UL - CLOCK_FLOOR_BUILD_MARGIN_S, e);
}

// The result must always clear the sanity threshold for any real build date.
void test_build_epoch_beats_sane_threshold(void) {
  TEST_ASSERT_GREATER_THAN_UINT32(CLOCK_FLOOR_SANE_EPOCH,
                                  clockFloorBuildEpoch("Jan  1 2024", "00:00:00"));
}

// Malformed inputs return 0 (caller falls back to the NVS floor alone).
void test_build_epoch_malformed_returns_zero(void) {
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch(nullptr, "00:00:00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  1 2024", nullptr));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Xyz  1 2024", "00:00:00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  1 20a4", "00:00:00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  1 2024", "24:00:00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  1 2024", "00-00-00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  0 2024", "00:00:00"));
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan 99 2024", "00:00:00"));
  // A pre-2023 build stamp is treated as malformed, not as a floor.
  TEST_ASSERT_EQUAL_UINT32(0, clockFloorBuildEpoch("Jan  1 1980", "00:00:00"));
}

// --- clockFloorTarget -------------------------------------------------------

void test_target_takes_newer_of_nvs_and_build(void) {
  TEST_ASSERT_EQUAL_UINT32(1800000000UL, clockFloorTarget(1800000000UL, 1750000000UL));
  TEST_ASSERT_EQUAL_UINT32(1800000000UL, clockFloorTarget(1750000000UL, 1800000000UL));
  // Fresh device: no NVS floor yet -> build stamp wins.
  TEST_ASSERT_EQUAL_UINT32(1750000000UL, clockFloorTarget(0, 1750000000UL));
  // Malformed build stamp -> NVS floor alone.
  TEST_ASSERT_EQUAL_UINT32(1750000000UL, clockFloorTarget(1750000000UL, 0));
}

// --- clockFloorShouldApply --------------------------------------------------

// Cold boot: clock at epoch, sane floor -> apply. The F1 case.
void test_apply_at_cold_boot(void) {
  TEST_ASSERT_TRUE(clockFloorShouldApply(0, 1750000000UL));
  TEST_ASSERT_TRUE(clockFloorShouldApply(1000, 1750000000UL));
}

// NTP already synced past the floor -> never move the clock backwards.
void test_no_apply_when_clock_ahead(void) {
  TEST_ASSERT_FALSE(clockFloorShouldApply(1750000001UL, 1750000000UL));
  TEST_ASSERT_FALSE(clockFloorShouldApply(1750000000UL, 1750000000UL));
}

// Insane floor (0, or below threshold) -> never apply.
void test_no_apply_with_insane_floor(void) {
  TEST_ASSERT_FALSE(clockFloorShouldApply(0, 0));
  TEST_ASSERT_FALSE(clockFloorShouldApply(0, CLOCK_FLOOR_SANE_EPOCH - 1));
}

// --- clockFloorShouldPersist ------------------------------------------------

// Sane clock, no stored floor -> persist immediately (first boot).
void test_persist_first_time(void) {
  TEST_ASSERT_TRUE(clockFloorShouldPersist(1750000000UL, 0));
}

// Stored floor fresher than a day -> no write (NVS wear bound).
void test_no_persist_within_interval(void) {
  TEST_ASSERT_FALSE(clockFloorShouldPersist(1750000000UL, 1750000000UL));
  TEST_ASSERT_FALSE(clockFloorShouldPersist(
      1750000000UL + CLOCK_FLOOR_PERSIST_INTERVAL_S - 1, 1750000000UL));
}

// Stored floor a day (or more) stale -> persist.
void test_persist_after_interval(void) {
  TEST_ASSERT_TRUE(clockFloorShouldPersist(
      1750000000UL + CLOCK_FLOOR_PERSIST_INTERVAL_S, 1750000000UL));
}

// Unsynced/floored-below-threshold clock never persists garbage.
void test_no_persist_with_insane_clock(void) {
  TEST_ASSERT_FALSE(clockFloorShouldPersist(0, 0));
  TEST_ASSERT_FALSE(clockFloorShouldPersist(CLOCK_FLOOR_SANE_EPOCH - 1, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_build_epoch_known_date);
  RUN_TEST(test_build_epoch_leap_day);
  RUN_TEST(test_build_epoch_space_padded_day);
  RUN_TEST(test_build_epoch_beats_sane_threshold);
  RUN_TEST(test_build_epoch_malformed_returns_zero);
  RUN_TEST(test_target_takes_newer_of_nvs_and_build);
  RUN_TEST(test_apply_at_cold_boot);
  RUN_TEST(test_no_apply_when_clock_ahead);
  RUN_TEST(test_no_apply_with_insane_floor);
  RUN_TEST(test_persist_first_time);
  RUN_TEST(test_no_persist_within_interval);
  RUN_TEST(test_persist_after_interval);
  RUN_TEST(test_no_persist_with_insane_clock);
  return UNITY_END();
}
