// Host-native unit tests for mqtt_rollback_policy.h (the rollback predicate).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "mqtt_rollback_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const char* GOOD = "{\"broker\":\"mqtt.thesada.app\",\"port\":8884}";
static const char* BAD  = "{\"broker\":\"bad.invalid\",\"port\":8884}";
static const char* BAD2 = "{\"broker\":\"other.invalid\",\"port\":8884}";

// The config that rebooted-without-connecting is still current -> roll back.
void test_bad_config_that_rebooted_rolls_back(void) {
  TEST_ASSERT_TRUE(mqttRollbackShould(GOOD, true, BAD, BAD));
}

// Offline good broker: counter rose but config never changed -> no rollback.
void test_offline_good_broker_does_not_roll_back(void) {
  TEST_ASSERT_FALSE(mqttRollbackShould(GOOD, true, GOOD, GOOD));
}

// Recovery edit after an unrelated failing streak: current != the recorded
// candidate -> no rollback (give the new config its own chance). The bug.
void test_recovery_edit_after_streak_does_not_roll_back(void) {
  TEST_ASSERT_FALSE(mqttRollbackShould(GOOD, true, BAD, BAD2));
}

// No snapshot yet -> nothing to fall back to.
void test_no_snapshot_does_not_roll_back(void) {
  TEST_ASSERT_FALSE(mqttRollbackShould(GOOD, false, BAD, BAD));
  TEST_ASSERT_FALSE(mqttRollbackShould("", true, BAD, BAD));
}

// No failing candidate recorded (no exhaustion reboot happened) -> no rollback.
void test_no_failing_candidate_does_not_roll_back(void) {
  TEST_ASSERT_FALSE(mqttRollbackShould(GOOD, true, "", BAD));
  TEST_ASSERT_FALSE(mqttRollbackShould(GOOD, true, nullptr, BAD));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bad_config_that_rebooted_rolls_back);
  RUN_TEST(test_offline_good_broker_does_not_roll_back);
  RUN_TEST(test_recovery_edit_after_streak_does_not_roll_back);
  RUN_TEST(test_no_snapshot_does_not_roll_back);
  RUN_TEST(test_no_failing_candidate_does_not_roll_back);
  return UNITY_END();
}
