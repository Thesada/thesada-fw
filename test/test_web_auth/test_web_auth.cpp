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

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_shipped_default_is_default);
  RUN_TEST(test_empty_and_null_are_default);
  RUN_TEST(test_real_password_is_not_default);
  RUN_TEST(test_default_pass_vetoes_basic);
  RUN_TEST(test_default_pass_vetoes_bearer);
  RUN_TEST(test_real_pass_admits_either_scheme);
  RUN_TEST(test_real_pass_rejects_bad_creds);
  return UNITY_END();
}
