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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_streak_earns_recycle_at_three_and_reset_at_six);
  RUN_TEST(test_pick_runs_a_reset_over_a_recycle);
  RUN_TEST(test_recovery_runs_only_on_the_driver_task);
  RUN_TEST(test_recovery_runs_inline_before_the_driver_is_known);
  return UNITY_END();
}
