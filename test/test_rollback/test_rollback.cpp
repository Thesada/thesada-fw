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

// --- mqttDefragRebootDue -----------------------------------------------------

void test_defrag_reboot_on_tls_fail_low_heap(void) {
  TEST_ASSERT_TRUE(mqttDefragRebootDue(3, true, -2, 39999, false));
  TEST_ASSERT_TRUE(mqttDefragRebootDue(7, true, -2, 20000, false));
}

// A DNS outage reports rc -2 without a handshake: never a reboot.
void test_defrag_reboot_not_on_dns_failure(void) {
  TEST_ASSERT_FALSE(mqttDefragRebootDue(3, false, -2, 20000, false));
  TEST_ASSERT_FALSE(mqttDefragRebootDue(99, false, -2, 0, false));
}

void test_defrag_reboot_other_gates(void) {
  TEST_ASSERT_FALSE(mqttDefragRebootDue(2, true, -2, 20000, false));
  TEST_ASSERT_FALSE(mqttDefragRebootDue(3, true, 5, 20000, false));
  TEST_ASSERT_FALSE(mqttDefragRebootDue(3, true, -4, 20000, false));
  TEST_ASSERT_FALSE(mqttDefragRebootDue(3, true, -2, 40000, false));
  TEST_ASSERT_FALSE(mqttDefragRebootDue(3, true, -2, 20000, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_bad_config_that_rebooted_rolls_back);
  RUN_TEST(test_offline_good_broker_does_not_roll_back);
  RUN_TEST(test_recovery_edit_after_streak_does_not_roll_back);
  RUN_TEST(test_no_snapshot_does_not_roll_back);
  RUN_TEST(test_no_failing_candidate_does_not_roll_back);
  RUN_TEST(test_defrag_reboot_on_tls_fail_low_heap);
  RUN_TEST(test_defrag_reboot_not_on_dns_failure);
  RUN_TEST(test_defrag_reboot_other_gates);
  return UNITY_END();
}
