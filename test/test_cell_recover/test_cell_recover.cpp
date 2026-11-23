// Host-native unit tests for cell_recover_policy.h (modem socket-failure
// recovery step and the task that runs it).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "cell_recover_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- step per streak ------------------------------------------------------------

// Three failures in a row recycle the bearer once, six power-cycle the modem.
void test_streak_earns_recycle_at_three_and_reset_at_six(void) {
  const uint8_t want[] = {CELL_RECOVER_NONE, CELL_RECOVER_NONE, CELL_RECOVER_NONE,
                          CELL_RECOVER_RECYCLE, CELL_RECOVER_NONE, CELL_RECOVER_NONE,
                          CELL_RECOVER_RESET};
  for (size_t s = 0; s < sizeof(want); ++s) {
    TEST_ASSERT_EQUAL_UINT8(want[s], cellRecoverStep((uint8_t)s));
  }
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RESET, cellRecoverStep(255));
}

// --- queued steps -----------------------------------------------------------------

void test_pick_runs_a_reset_over_a_recycle(void) {
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_NONE, cellRecoverPick(CELL_RECOVER_NONE));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RECYCLE, cellRecoverPick(CELL_RECOVER_RECYCLE));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RESET, cellRecoverPick(CELL_RECOVER_RESET));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RESET,
                          cellRecoverPick(CELL_RECOVER_RECYCLE | CELL_RECOVER_RESET));
}

// --- which task ---------------------------------------------------------------------

// #69: the net.http task ran the CNACT recycle and the hard reset while the
// loop task waited on the AT bus without feeding its watchdog.
void test_recovery_runs_only_on_the_driver_task(void) {
  int loopTask = 0, fetchTask = 0;
  TEST_ASSERT_TRUE(cellRecoverHere(&loopTask, &loopTask));
  TEST_ASSERT_FALSE(cellRecoverHere(&fetchTask, &loopTask));
}

void test_recovery_runs_inline_before_the_driver_is_known(void) {
  int loopTask = 0;
  TEST_ASSERT_TRUE(cellRecoverHere(&loopTask, nullptr));
}

// --- per slot ----------------------------------------------------------------------

// fw #73: an HTTPS success on slot 1 cleared the open streak of a failing MQTT slot 0.
void test_success_on_one_slot_keeps_the_other_streak(void) {
  CellFailStreaks<2> s;
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_NONE, s.fail(0));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_NONE, s.fail(0));
  s.ok(1);
  TEST_ASSERT_EQUAL_UINT8(2, s.streak(0));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RECYCLE, s.fail(0));
  TEST_ASSERT_EQUAL_UINT8(0, s.streak(1));
}

void test_success_clears_its_own_slot(void) {
  CellFailStreaks<2> s;
  s.fail(1);
  s.fail(1);
  s.ok(1);
  TEST_ASSERT_EQUAL_UINT8(0, s.streak(1));
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_NONE, s.fail(1));
}

// The power cycle a slot earns resets the modem under every slot.
void test_reset_clears_every_slot(void) {
  CellFailStreaks<2> s;
  s.fail(1);
  for (int i = 0; i < 5; i++) s.fail(0);
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_RESET, s.fail(0));
  TEST_ASSERT_EQUAL_UINT8(0, s.streak(0));
  TEST_ASSERT_EQUAL_UINT8(0, s.streak(1));
}

void test_out_of_range_slot_is_ignored(void) {
  CellFailStreaks<2> s;
  TEST_ASSERT_EQUAL_UINT8(CELL_RECOVER_NONE, s.fail(7));
  s.ok(7);
  TEST_ASSERT_EQUAL_UINT8(0, s.streak(7));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_streak_earns_recycle_at_three_and_reset_at_six);
  RUN_TEST(test_pick_runs_a_reset_over_a_recycle);
  RUN_TEST(test_recovery_runs_only_on_the_driver_task);
  RUN_TEST(test_recovery_runs_inline_before_the_driver_is_known);
  RUN_TEST(test_success_on_one_slot_keeps_the_other_streak);
  RUN_TEST(test_success_clears_its_own_slot);
  RUN_TEST(test_reset_clears_every_slot);
  RUN_TEST(test_out_of_range_slot_is_ignored);
  return UNITY_END();
}
