// Host-native unit tests for web_auth_policy.h (default-creds admin veto).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "web_auth_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- webAuthPassIsDefault ---------------------------------------------------

void test_shipped_default_is_default(void) {
  TEST_ASSERT_TRUE(webAuthPassIsDefault("changeme"));
}

// Explicit empty and missing passwords must count as default - an empty
// web.password in config.json must not open the admin surface.
void test_empty_and_null_are_default(void) {
  TEST_ASSERT_TRUE(webAuthPassIsDefault(""));
  TEST_ASSERT_TRUE(webAuthPassIsDefault(nullptr));
}

void test_real_password_is_not_default(void) {
  TEST_ASSERT_FALSE(webAuthPassIsDefault("s3cret-Pw"));
  // Near-misses stay non-default: the veto is exact, not a prefix rule.
  TEST_ASSERT_FALSE(webAuthPassIsDefault("changeme2"));
  TEST_ASSERT_FALSE(webAuthPassIsDefault("Changeme"));
}

// --- webAuthAllowed -----------------------------------------------------------

// The F2 case: default password serves nothing, even with correct Basic auth.
void test_default_pass_vetoes_basic(void) {
  TEST_ASSERT_FALSE(webAuthAllowed(true, false, true));
}

// A Bearer token minted before a password reset must not outlive the reset.
void test_default_pass_vetoes_bearer(void) {
  TEST_ASSERT_FALSE(webAuthAllowed(true, true, false));
  TEST_ASSERT_FALSE(webAuthAllowed(true, true, true));
}

void test_real_pass_admits_either_scheme(void) {
  TEST_ASSERT_TRUE(webAuthAllowed(false, true, false));   // Bearer
  TEST_ASSERT_TRUE(webAuthAllowed(false, false, true));   // Basic
  TEST_ASSERT_TRUE(webAuthAllowed(false, true, true));
}

void test_real_pass_rejects_bad_creds(void) {
  TEST_ASSERT_FALSE(webAuthAllowed(false, false, false));
}

// --- webAuthMethodChangesState ------------------------------------------------

void test_read_methods_do_not_change_state(void) {
  TEST_ASSERT_FALSE(webAuthMethodChangesState("GET"));
  TEST_ASSERT_FALSE(webAuthMethodChangesState("HEAD"));
  TEST_ASSERT_FALSE(webAuthMethodChangesState("OPTIONS"));
}

void test_write_methods_change_state(void) {
  TEST_ASSERT_TRUE(webAuthMethodChangesState("POST"));
  TEST_ASSERT_TRUE(webAuthMethodChangesState("DELETE"));
  TEST_ASSERT_TRUE(webAuthMethodChangesState("PUT"));
  TEST_ASSERT_TRUE(webAuthMethodChangesState("PATCH"));
}

// An unknown or missing method is treated as unsafe, never waved through.
void test_unknown_method_counts_as_state_changing(void) {
  TEST_ASSERT_TRUE(webAuthMethodChangesState(nullptr));
  TEST_ASSERT_TRUE(webAuthMethodChangesState(""));
  TEST_ASSERT_TRUE(webAuthMethodChangesState("get"));
}

// --- webAuthBasicAllowed ------------------------------------------------------

// The CSRF case: a cross-site auto-submit form replays cached Basic creds.
void test_cross_site_write_refuses_basic(void) {
  TEST_ASSERT_FALSE(webAuthBasicAllowed(true, "cross-site"));
}

// A cross-site read changes nothing and the response is not readable
// cross-origin, so Basic still applies there.
void test_cross_site_read_keeps_basic(void) {
  TEST_ASSERT_TRUE(webAuthBasicAllowed(false, "cross-site"));
}

// GET /api/ws/token mints a WS grant and /ws/serial reaches the shell, so the
// caller declares the side effect and the rule applies despite the safe method.
void test_side_effect_read_is_refused_when_declared(void) {
  TEST_ASSERT_FALSE(webAuthMethodChangesState("GET"));      // method alone admits
  TEST_ASSERT_FALSE(webAuthBasicAllowed(true, "cross-site"));  // declaration refuses
}

// The device's own UI is same-origin; a bookmark or typed URL sends none.
void test_non_cross_site_values_keep_basic(void) {
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, "same-origin"));
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, "same-site"));
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, "none"));
}

// curl and pre-Sec-Fetch browsers send no header and must keep working.
void test_absent_header_keeps_basic(void) {
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, nullptr));
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, ""));
}

// Only the exact token refuses; a near-miss or case variant does not, so the
// rule never guesses at a value the browser did not send.
void test_near_miss_site_values_keep_basic(void) {
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, "cross-site-ish"));
  TEST_ASSERT_TRUE(webAuthBasicAllowed(true, "Cross-Site"));
}

// --- webAuthCountsAsGuess -----------------------------------------------------

void test_wrong_credential_is_a_guess(void) {
  TEST_ASSERT_TRUE(webAuthCountsAsGuess(false, true, true));
}

// The cross-site refusal never reached the password check, so it says nothing
// about whether the caller knows it - and a foreign page must not lock anyone out.
void test_policy_refusal_is_not_a_guess(void) {
  TEST_ASSERT_FALSE(webAuthCountsAsGuess(false, false, true));
}

// Nothing was attempted, so there is nothing to throttle.
void test_anonymous_request_is_not_a_guess(void) {
  TEST_ASSERT_FALSE(webAuthCountsAsGuess(false, true, false));
}

void test_success_is_never_a_guess(void) {
  TEST_ASSERT_FALSE(webAuthCountsAsGuess(true, true, true));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_shipped_default_is_default);
  RUN_TEST(test_empty_and_null_are_default);
  RUN_TEST(test_real_password_is_not_default);
  RUN_TEST(test_default_pass_vetoes_basic);
  RUN_TEST(test_default_pass_vetoes_bearer);
  RUN_TEST(test_real_pass_admits_either_scheme);
  RUN_TEST(test_real_pass_rejects_bad_creds);
  RUN_TEST(test_read_methods_do_not_change_state);
  RUN_TEST(test_write_methods_change_state);
  RUN_TEST(test_unknown_method_counts_as_state_changing);
  RUN_TEST(test_cross_site_write_refuses_basic);
  RUN_TEST(test_cross_site_read_keeps_basic);
  RUN_TEST(test_side_effect_read_is_refused_when_declared);
  RUN_TEST(test_non_cross_site_values_keep_basic);
  RUN_TEST(test_absent_header_keeps_basic);
  RUN_TEST(test_near_miss_site_values_keep_basic);
  RUN_TEST(test_wrong_credential_is_a_guess);
  RUN_TEST(test_policy_refusal_is_not_a_guess);
  RUN_TEST(test_anonymous_request_is_not_a_guess);
  RUN_TEST(test_success_is_never_a_guess);
  return UNITY_END();
}
