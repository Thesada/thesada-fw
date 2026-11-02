// Host-native unit tests for temp_guard_policy.h (DS18B20 plausibility guard).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <cmath>
#include "temp_guard_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const float DELTA = 40.0f;

static TempGuard baseline(float c) {
  TempGuard g;
  TEST_ASSERT_TRUE(tempGuardJudge(g, c, DELTA) == TempVerdict::Accepted);
  return g;
}

void test_first_reading_is_accepted_whatever_its_value(void) {
  TempGuard g;
  TEST_ASSERT_TRUE(tempGuardJudge(g, 95.5f, DELTA) == TempVerdict::Accepted);
  TEST_ASSERT_EQUAL_FLOAT(95.5f, g.accepted);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NO_READING, g.held);
}

// |delta| == max_delta_c is plausible; only a larger step is held back.
void test_step_of_exactly_max_delta_is_accepted(void) {
  TempGuard g = baseline(10.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 50.0f, DELTA) == TempVerdict::Accepted);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 10.0f, DELTA) == TempVerdict::Accepted);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 50.5f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_EQUAL_FLOAT(10.0f, g.accepted);
}

// Issue #12: 5 C, probe drops out during fire-up, returns at 70 C. The first
// 70 is held, the second confirms it, and the probe reads again.
void test_real_jump_after_dropout_rebaselines_on_second_sample(void) {
  TempGuard g = baseline(5.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, TEMP_NO_READING, DELTA) == TempVerdict::NoReading);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 70.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_EQUAL_FLOAT(5.0f, g.accepted);
  TEST_ASSERT_EQUAL_FLOAT(70.0f, g.held);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 70.5f, DELTA) == TempVerdict::Rebaselined);
  TEST_ASSERT_EQUAL_FLOAT(70.5f, g.accepted);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NO_READING, g.held);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 71.0f, DELTA) == TempVerdict::Accepted);
}

// A probe that flaps (fail, read, fail, read) still rebaselines: a failed
// read between the two samples keeps the held one.
void test_held_reading_survives_a_failed_read(void) {
  TempGuard g = baseline(5.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 70.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_TRUE(tempGuardJudge(g, TEMP_NO_READING, DELTA) == TempVerdict::NoReading);
  TEST_ASSERT_EQUAL_FLOAT(70.0f, g.held);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 72.0f, DELTA) == TempVerdict::Rebaselined);
  TEST_ASSERT_EQUAL_FLOAT(72.0f, g.accepted);
}

// One garbage reading is dropped and does not linger: after a good reading
// clears it, a second lone outlier is held again, not accepted.
void test_lone_outlier_is_dropped_and_forgotten(void) {
  TempGuard g = baseline(20.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 100.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 20.5f, DELTA) == TempVerdict::Accepted);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NO_READING, g.held);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 100.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_EQUAL_FLOAT(20.5f, g.accepted);
}

// Outliers that disagree with each other never pass; the newest is held.
void test_disagreeing_outliers_stay_held(void) {
  TempGuard g = baseline(20.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 100.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_TRUE(tempGuardJudge(g, -40.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_EQUAL_FLOAT(-40.0f, g.held);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 100.0f, DELTA) == TempVerdict::Implausible);
  TEST_ASSERT_EQUAL_FLOAT(20.0f, g.accepted);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 101.0f, DELTA) == TempVerdict::Rebaselined);
  TEST_ASSERT_EQUAL_FLOAT(101.0f, g.accepted);
}

// Library faults (-127, and the -250..-254 codes) and NaN never move state.
void test_failed_reads_leave_state_untouched(void) {
  TempGuard g = baseline(20.0f);
  TEST_ASSERT_TRUE(tempGuardJudge(g, 90.0f, DELTA) == TempVerdict::Implausible);
  const float bad[] = {TEMP_NO_READING, -254.0f, -251.0f, NAN};
  for (float c : bad) {
    TEST_ASSERT_TRUE(tempGuardJudge(g, c, DELTA) == TempVerdict::NoReading);
    TEST_ASSERT_EQUAL_FLOAT(20.0f, g.accepted);
    TEST_ASSERT_EQUAL_FLOAT(90.0f, g.held);
  }
  TempGuard fresh;
  TEST_ASSERT_TRUE(tempGuardJudge(fresh, NAN, DELTA) == TempVerdict::NoReading);
  TEST_ASSERT_EQUAL_FLOAT(TEMP_NO_READING, fresh.accepted);
}

// max_delta_c 0 (or negative) turns the guard off: every reading passes.
void test_zero_or_negative_max_delta_disables_guard(void) {
  const float off[] = {0.0f, -1.0f};
  for (float d : off) {
    TempGuard g;
    TEST_ASSERT_TRUE(tempGuardJudge(g, 5.0f, d) == TempVerdict::Accepted);
    TEST_ASSERT_TRUE(tempGuardJudge(g, 95.0f, d) == TempVerdict::Accepted);
    TEST_ASSERT_EQUAL_FLOAT(95.0f, g.accepted);
    TEST_ASSERT_TRUE(tempGuardJudge(g, TEMP_NO_READING, d) == TempVerdict::NoReading);
  }
}

// A steady climb in sub-delta steps is never held back.
void test_gradual_climb_is_always_accepted(void) {
  TempGuard g = baseline(-20.0f);
  for (int i = 1; i <= 3; i++) {
    float c = -20.0f + 39.5f * (float)i;
    TEST_ASSERT_TRUE(tempGuardJudge(g, c, DELTA) == TempVerdict::Accepted);
    TEST_ASSERT_EQUAL_FLOAT(c, g.accepted);
  }
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_reading_is_accepted_whatever_its_value);
  RUN_TEST(test_step_of_exactly_max_delta_is_accepted);
  RUN_TEST(test_real_jump_after_dropout_rebaselines_on_second_sample);
  RUN_TEST(test_held_reading_survives_a_failed_read);
  RUN_TEST(test_lone_outlier_is_dropped_and_forgotten);
  RUN_TEST(test_disagreeing_outliers_stay_held);
  RUN_TEST(test_failed_reads_leave_state_untouched);
  RUN_TEST(test_zero_or_negative_max_delta_disables_guard);
  RUN_TEST(test_gradual_climb_is_always_accepted);
  return UNITY_END();
}
