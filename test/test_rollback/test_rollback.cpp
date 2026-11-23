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

// --- mqttSnapshotProven ------------------------------------------------------

void test_snapshot_when_session_dialed_the_config_broker(void) {
  TEST_ASSERT_TRUE(mqttSnapshotProven("mqtt.thesada.app", 8884, "mqtt.thesada.app", 8884));
}

// thesada-fw#78: a refused host keeps the session on the previous broker. That
// connect must not commit the refused config as last-good, or no rollback is left.
void test_no_snapshot_when_host_was_refused(void) {
  TEST_ASSERT_FALSE(mqttSnapshotProven("a-host-too-long-for-the-buffer.invalid", 8884,
                                       "mqtt.thesada.app", 8884));
}

// mqtt.port written but not yet applied: the session still dials the old port.
void test_no_snapshot_when_port_not_applied(void) {
  TEST_ASSERT_FALSE(mqttSnapshotProven("mqtt.thesada.app", 8883, "mqtt.thesada.app", 8884));
}

void test_no_snapshot_without_a_live_broker(void) {
  TEST_ASSERT_FALSE(mqttSnapshotProven("", 8884, "", 8884));
  TEST_ASSERT_FALSE(mqttSnapshotProven(nullptr, 8884, "mqtt.thesada.app", 8884));
  TEST_ASSERT_FALSE(mqttSnapshotProven("mqtt.thesada.app", 8884, nullptr, 8884));
}

// A fresh unit or a standalone config with no broker never dials "", so it
// never spends the reboot budget on a config already logged as missing.
void test_no_broker_and_no_snapshot_does_not_dial(void) {
  TEST_ASSERT_FALSE(mqttShouldDial("", false));
  TEST_ASSERT_FALSE(mqttShouldDial(nullptr, false));
}

// An emptied broker on a unit with a last-good snapshot still dials: those
// failures record the candidate and the boot rollback restores the snapshot.
void test_no_broker_with_snapshot_still_dials_for_the_rollback(void) {
  TEST_ASSERT_TRUE(mqttShouldDial("", true));
  static const char* EMPTIED = "{\"broker\":\"\",\"port\":8884}";
  TEST_ASSERT_TRUE(mqttRollbackShould(GOOD, true, EMPTIED, EMPTIED));
}

// CR #129: the marker landed but the snapshot write did not. Nothing to restore.
void test_last_good_needs_the_snapshot_not_only_the_marker(void) {
  TEST_ASSERT_TRUE(mqttLastGoodUsable(GOOD, true));
  TEST_ASSERT_FALSE(mqttLastGoodUsable(GOOD, false));
  TEST_ASSERT_FALSE(mqttLastGoodUsable("", true));
  TEST_ASSERT_FALSE(mqttLastGoodUsable(nullptr, true));
  TEST_ASSERT_FALSE(mqttLastGoodUsable("{\"broker\":\"mqtt.thes", true));
  TEST_ASSERT_FALSE(mqttLastGoodUsable("{\"broker\":\"\",\"port\":8884}", true));
  TEST_ASSERT_FALSE(mqttLastGoodUsable("[]", true));
}

void test_a_set_broker_dials(void) {
  TEST_ASSERT_TRUE(mqttShouldDial("broker.example", false));
  TEST_ASSERT_TRUE(mqttShouldDial("broker.example", true));
}

// A rollback whose persist failed dials last-good this session, while the
// live config is still the failing one. An operator edit since wins.
void test_rollback_session_dials_last_good(void) {
  JsonDocument lg;
  TEST_ASSERT_TRUE(mqttRollbackSessionLastGood(true, BAD, BAD, GOOD, lg));
  TEST_ASSERT_EQUAL_STRING("mqtt.thesada.app", lg["broker"] | "");
  TEST_ASSERT_EQUAL_UINT(8884, lg["port"] | 0);
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(false, BAD, BAD, GOOD, lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, BAD, BAD2, GOOD, lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, "", BAD, GOOD, lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, nullptr, BAD, GOOD, lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, BAD, nullptr, GOOD, lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, BAD, BAD, "{\"broker\":", lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, BAD, BAD, "{\"port\":8884}", lg));
  TEST_ASSERT_FALSE(mqttRollbackSessionLastGood(true, BAD, BAD, nullptr, lg));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_no_broker_and_no_snapshot_does_not_dial);
  RUN_TEST(test_no_broker_with_snapshot_still_dials_for_the_rollback);
  RUN_TEST(test_last_good_needs_the_snapshot_not_only_the_marker);
  RUN_TEST(test_a_set_broker_dials);
  RUN_TEST(test_snapshot_when_session_dialed_the_config_broker);
  RUN_TEST(test_no_snapshot_when_host_was_refused);
  RUN_TEST(test_no_snapshot_when_port_not_applied);
  RUN_TEST(test_no_snapshot_without_a_live_broker);
  RUN_TEST(test_bad_config_that_rebooted_rolls_back);
  RUN_TEST(test_offline_good_broker_does_not_roll_back);
  RUN_TEST(test_recovery_edit_after_streak_does_not_roll_back);
  RUN_TEST(test_no_snapshot_does_not_roll_back);
  RUN_TEST(test_no_failing_candidate_does_not_roll_back);
  RUN_TEST(test_defrag_reboot_on_tls_fail_low_heap);
  RUN_TEST(test_defrag_reboot_not_on_dns_failure);
  RUN_TEST(test_defrag_reboot_other_gates);
  RUN_TEST(test_rollback_session_dials_last_good);
  return UNITY_END();
}
