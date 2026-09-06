// Host-native unit tests for shell_mode_policy.h (headless command surface).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "shell_mode_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- shellModeParse -----------------------------------------------------------

void test_each_mode_name_parses(void) {
  TEST_ASSERT_EQUAL(SHELL_MODE_FULL,        shellModeParse("full"));
  TEST_ASSERT_EQUAL(SHELL_MODE_SERIAL_ONLY, shellModeParse("serial-only"));
  TEST_ASSERT_EQUAL(SHELL_MODE_MQTT_ONLY,   shellModeParse("mqtt-only"));
  TEST_ASSERT_EQUAL(SHELL_MODE_OFF,         shellModeParse("off"));
}

// An old config has no key at all, and must behave exactly as it did before
// the mode existed - a fleet mid-upgrade must not go quiet.
void test_absent_key_is_full(void) {
  TEST_ASSERT_EQUAL(SHELL_MODE_FULL, shellModeParse(nullptr));
  TEST_ASSERT_EQUAL(SHELL_MODE_FULL, shellModeParse(""));
}

// A value that was typed but not understood is a hardening request we cannot
// honour precisely, so it closes rather than silently serving everything.
void test_unrecognised_value_closes_everything(void) {
  TEST_ASSERT_EQUAL(SHELL_MODE_OFF, shellModeParse("of"));
  TEST_ASSERT_EQUAL(SHELL_MODE_OFF, shellModeParse("Full"));
  TEST_ASSERT_EQUAL(SHELL_MODE_OFF, shellModeParse("serial_only"));
  TEST_ASSERT_EQUAL(SHELL_MODE_OFF, shellModeParse("true"));
}

// --- transport gates ----------------------------------------------------------

void test_full_allows_both(void) {
  TEST_ASSERT_TRUE(shellModeSerialAllowed(SHELL_MODE_FULL));
  TEST_ASSERT_TRUE(shellModeMqttAllowed(SHELL_MODE_FULL));
}

void test_serial_only_drops_mqtt(void) {
  TEST_ASSERT_TRUE(shellModeSerialAllowed(SHELL_MODE_SERIAL_ONLY));
  TEST_ASSERT_FALSE(shellModeMqttAllowed(SHELL_MODE_SERIAL_ONLY));
}

void test_mqtt_only_drops_serial(void) {
  TEST_ASSERT_FALSE(shellModeSerialAllowed(SHELL_MODE_MQTT_ONLY));
  TEST_ASSERT_TRUE(shellModeMqttAllowed(SHELL_MODE_MQTT_ONLY));
}

void test_off_drops_both(void) {
  TEST_ASSERT_FALSE(shellModeSerialAllowed(SHELL_MODE_OFF));
  TEST_ASSERT_FALSE(shellModeMqttAllowed(SHELL_MODE_OFF));
}

// --- shellModeUnrecognised ----------------------------------------------------

// Only a typo is worth warning about: a real "off" and an absent key are both
// deliberate, and neither should nag on every boot.
void test_only_a_typo_is_unrecognised(void) {
  TEST_ASSERT_TRUE(shellModeUnrecognised("of"));
  TEST_ASSERT_TRUE(shellModeUnrecognised("Full"));
  TEST_ASSERT_FALSE(shellModeUnrecognised("off"));
  TEST_ASSERT_FALSE(shellModeUnrecognised("full"));
  TEST_ASSERT_FALSE(shellModeUnrecognised(nullptr));
  TEST_ASSERT_FALSE(shellModeUnrecognised(""));
}

// --- shellModeHttpAllowed -----------------------------------------------------

// POST /api/cmd and /ws/serial reach the same Shell. A device asked to narrow
// its command surface must not keep the widest transport open.
void test_only_full_allows_the_http_surface(void) {
  TEST_ASSERT_TRUE(shellModeHttpAllowed(SHELL_MODE_FULL));
  TEST_ASSERT_FALSE(shellModeHttpAllowed(SHELL_MODE_SERIAL_ONLY));
  TEST_ASSERT_FALSE(shellModeHttpAllowed(SHELL_MODE_MQTT_ONLY));
  TEST_ASSERT_FALSE(shellModeHttpAllowed(SHELL_MODE_OFF));
}

// off means off on every transport - the acceptance criterion for headless.
void test_off_closes_every_transport(void) {
  TEST_ASSERT_FALSE(shellModeSerialAllowed(SHELL_MODE_OFF));
  TEST_ASSERT_FALSE(shellModeMqttAllowed(SHELL_MODE_OFF));
  TEST_ASSERT_FALSE(shellModeHttpAllowed(SHELL_MODE_OFF));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_each_mode_name_parses);
  RUN_TEST(test_absent_key_is_full);
  RUN_TEST(test_unrecognised_value_closes_everything);
  RUN_TEST(test_full_allows_both);
  RUN_TEST(test_serial_only_drops_mqtt);
  RUN_TEST(test_mqtt_only_drops_serial);
  RUN_TEST(test_off_drops_both);
  RUN_TEST(test_only_a_typo_is_unrecognised);
  RUN_TEST(test_only_full_allows_the_http_surface);
  RUN_TEST(test_off_closes_every_transport);
  return UNITY_END();
}
