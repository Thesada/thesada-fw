// Host-native unit tests for web_snapshot_policy.h (config copies for async_tcp).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "web_snapshot_policy.h"

void setUp(void) {}
void tearDown(void) {}

// The snapshot owns its bytes: wiping the source leaves the copy intact,
// which is what a config doc rebuild does to the strings it held.
void test_snapshot_survives_the_source_being_freed(void) {
  char user[] = "operator";
  char pass[] = "s3cret-pass";
  char name[] = "boiler-room";
  WebSnap s;
  webSnapFill(s, user, pass, name);
  memset(user, 'x', sizeof(user) - 1);
  memset(pass, 'x', sizeof(pass) - 1);
  memset(name, 'x', sizeof(name) - 1);
  TEST_ASSERT_TRUE(s.userFits && s.passFits);
  TEST_ASSERT_EQUAL_STRING("operator", s.user);
  TEST_ASSERT_EQUAL_STRING("s3cret-pass", s.passCfg);
  TEST_ASSERT_EQUAL_STRING("boiler-room", s.devName);
}

// A clipped user or password would accept a different credential.
void test_credential_that_does_not_fit_is_flagged_not_clipped(void) {
  char longUser[WEB_SNAP_USER_CAP + 1];
  memset(longUser, 'u', WEB_SNAP_USER_CAP);
  longUser[WEB_SNAP_USER_CAP] = '\0';
  WebSnap s;
  webSnapFill(s, longUser, "pw", "n");
  TEST_ASSERT_FALSE(s.userFits);
  TEST_ASSERT_EQUAL_STRING("", s.user);

  char longPass[WEB_SNAP_PASS_CAP + 1];
  memset(longPass, 'p', WEB_SNAP_PASS_CAP);
  longPass[WEB_SNAP_PASS_CAP] = '\0';
  webSnapFill(s, "admin", longPass, "n");
  TEST_ASSERT_TRUE(s.userFits);
  TEST_ASSERT_FALSE(s.passFits);
  TEST_ASSERT_EQUAL_STRING("", s.passCfg);
}

// --- webCredState ------------------------------------------------------------

static WebSnap snapWith(bool userFits, bool passFits) {
  WebSnap s;
  webSnapFill(s, "admin", "fallback-pw", "n");
  s.userFits = userFits;
  s.passFits = passFits;
  return s;
}

// The NVS secret wins over the fallback, so a too-long fallback that is never
// used must not lock the admin out.
void test_nvs_password_beats_a_too_long_fallback(void) {
  TEST_ASSERT_EQUAL(WEB_CRED_OK, webCredState(snapWith(true, false), "nvs-secret"));
}

// NVS miss: the overflowed fallback reads as "", and that is the real cause.
void test_too_long_fallback_in_use_is_too_long_not_default(void) {
  TEST_ASSERT_EQUAL(WEB_CRED_TOO_LONG, webCredState(snapWith(true, false), ""));
  TEST_ASSERT_EQUAL(WEB_CRED_TOO_LONG, webCredState(snapWith(true, false), nullptr));
}

void test_too_long_user_locks_whatever_the_password(void) {
  TEST_ASSERT_EQUAL(WEB_CRED_TOO_LONG, webCredState(snapWith(false, true), "nvs-secret"));
}

void test_default_and_ok_states(void) {
  TEST_ASSERT_EQUAL(WEB_CRED_DEFAULT, webCredState(snapWith(true, true), "changeme"));
  TEST_ASSERT_EQUAL(WEB_CRED_DEFAULT, webCredState(snapWith(true, true), ""));
  TEST_ASSERT_EQUAL(WEB_CRED_OK, webCredState(snapWith(true, true), "fallback-pw"));
}

void test_longest_fitting_credentials_are_usable(void) {
  char user[WEB_SNAP_USER_CAP];
  memset(user, 'u', sizeof(user) - 1);
  user[sizeof(user) - 1] = '\0';
  char pass[WEB_SNAP_PASS_CAP];
  memset(pass, 'p', sizeof(pass) - 1);
  pass[sizeof(pass) - 1] = '\0';
  WebSnap s;
  webSnapFill(s, user, pass, "n");
  TEST_ASSERT_TRUE(s.userFits && s.passFits);
  TEST_ASSERT_EQUAL_STRING(user, s.user);
  TEST_ASSERT_EQUAL_STRING(pass, s.passCfg);
}

// The device name is display only, so a long one falls back to the default.
void test_long_name_falls_back_to_the_default(void) {
  char longName[WEB_SNAP_NAME_CAP + 8];
  memset(longName, 'n', sizeof(longName) - 1);
  longName[sizeof(longName) - 1] = '\0';
  WebSnap s;
  webSnapFill(s, "admin", "pw", longName);
  TEST_ASSERT_TRUE(s.userFits && s.passFits);
  TEST_ASSERT_EQUAL_STRING(WEB_SNAP_NAME_DEFAULT, s.devName);
}

void test_null_inputs_read_as_empty(void) {
  WebSnap s;
  webSnapFill(s, nullptr, nullptr, nullptr);
  TEST_ASSERT_TRUE(s.userFits && s.passFits);
  TEST_ASSERT_EQUAL_STRING("", s.user);
  TEST_ASSERT_EQUAL_STRING("", s.passCfg);
  TEST_ASSERT_EQUAL_STRING("", s.devName);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_snapshot_survives_the_source_being_freed);
  RUN_TEST(test_credential_that_does_not_fit_is_flagged_not_clipped);
  RUN_TEST(test_nvs_password_beats_a_too_long_fallback);
  RUN_TEST(test_too_long_fallback_in_use_is_too_long_not_default);
  RUN_TEST(test_too_long_user_locks_whatever_the_password);
  RUN_TEST(test_default_and_ok_states);
  RUN_TEST(test_longest_fitting_credentials_are_usable);
  RUN_TEST(test_long_name_falls_back_to_the_default);
  RUN_TEST(test_null_inputs_read_as_empty);
  return UNITY_END();
}
