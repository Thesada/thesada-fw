// Host-native unit tests for ap_policy.h (fallback access point).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ap_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- apPasswordUsable -------------------------------------------------------

void test_accepts_a_real_passphrase(void) {
  TEST_ASSERT_TRUE(apPasswordUsable("9f2a7c41bb03"));
  TEST_ASSERT_TRUE(apPasswordUsable("12345678"));  // exactly the WPA2 minimum
}

// The trap this unit exists for: the shipped placeholder is exactly 8
// characters, so a length-only gate lets it through and the AP comes up
// "protected" by a password that is in the repository.
void test_rejects_the_shipped_placeholder(void) {
  TEST_ASSERT_EQUAL_UINT(8, (unsigned)strlen(AP_PASS_PLACEHOLDER));
  TEST_ASSERT_FALSE(apPasswordUsable(AP_PASS_PLACEHOLDER));
  TEST_ASSERT_FALSE(apPasswordUsable("changeme"));
}

void test_rejects_absent_or_short(void) {
  TEST_ASSERT_FALSE(apPasswordUsable(nullptr));
  TEST_ASSERT_FALSE(apPasswordUsable(""));
  TEST_ASSERT_FALSE(apPasswordUsable("short"));
  TEST_ASSERT_FALSE(apPasswordUsable("1234567"));  // one under the minimum
}

// Near-misses are fine: only the exact placeholder is refused.
void test_accepts_placeholder_lookalikes(void) {
  TEST_ASSERT_TRUE(apPasswordUsable("changeme1"));
  TEST_ASSERT_TRUE(apPasswordUsable("Changeme"));
}

// --- apMayStart -------------------------------------------------------------

// Fail closed. An open AP is not a degraded recovery path, it is an
// unauthenticated console that writes WiFi credentials.
void test_ap_refuses_to_start_without_a_usable_passphrase(void) {
  TEST_ASSERT_FALSE(apMayStart(nullptr));
  TEST_ASSERT_FALSE(apMayStart(""));
  TEST_ASSERT_FALSE(apMayStart("changeme"));
  TEST_ASSERT_FALSE(apMayStart("short"));
  TEST_ASSERT_TRUE(apMayStart("9f2a7c41bb03"));
}

// --- apSsidFor --------------------------------------------------------------

void test_ssid_prefers_device_id(void) {
  char ssid[32];
  TEST_ASSERT_TRUE(apSsidFor(ssid, sizeof(ssid), "thesada-dcb4d91acd28", "thesada-owb"));
  TEST_ASSERT_EQUAL_STRING("thesada-dcb4d91acd28-setup", ssid);
}

// The operator label ships as a fixed string, so every unit would broadcast
// the same SSID and a per-device join QR would be ambiguous.
void test_ssid_falls_back_only_when_no_device_id(void) {
  char ssid[32];
  TEST_ASSERT_TRUE(apSsidFor(ssid, sizeof(ssid), "", "thesada-owb"));
  TEST_ASSERT_EQUAL_STRING("thesada-owb-setup", ssid);
  TEST_ASSERT_TRUE(apSsidFor(ssid, sizeof(ssid), nullptr, "thesada-owb"));
  TEST_ASSERT_EQUAL_STRING("thesada-owb-setup", ssid);
}

// Truncation must fail rather than emit a short SSID: two units whose ids
// share a prefix would otherwise broadcast the same name.
void test_ssid_rejects_truncation(void) {
  char small[10];
  TEST_ASSERT_FALSE(apSsidFor(small, sizeof(small), "thesada-dcb4d91acd28", "x"));
  TEST_ASSERT_EQUAL_STRING("", small);
  char exact[27];  // 20 + 6 + NUL
  TEST_ASSERT_TRUE(apSsidFor(exact, sizeof(exact), "thesada-dcb4d91acd28", "x"));
}

void test_ssid_degenerate_inputs(void) {
  char ssid[32];
  TEST_ASSERT_FALSE(apSsidFor(nullptr, sizeof(ssid), "a", "b"));
  TEST_ASSERT_FALSE(apSsidFor(ssid, 0, "a", "b"));
  TEST_ASSERT_FALSE(apSsidFor(ssid, sizeof(ssid), "", ""));
  TEST_ASSERT_FALSE(apSsidFor(ssid, sizeof(ssid), nullptr, nullptr));
}

// identity.info reports the state word, never the value.
void test_password_state_words(void) {
  TEST_ASSERT_EQUAL_STRING("absent",    apPasswordState(nullptr));
  TEST_ASSERT_EQUAL_STRING("absent",    apPasswordState(""));
  TEST_ASSERT_EQUAL_STRING("default",   apPasswordState(AP_PASS_PLACEHOLDER));
  TEST_ASSERT_EQUAL_STRING("too-short", apPasswordState("short7c"));
  TEST_ASSERT_EQUAL_STRING("set",       apPasswordState("a-real-passphrase"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_password_state_words);
  RUN_TEST(test_accepts_a_real_passphrase);
  RUN_TEST(test_rejects_the_shipped_placeholder);
  RUN_TEST(test_rejects_absent_or_short);
  RUN_TEST(test_accepts_placeholder_lookalikes);
  RUN_TEST(test_ap_refuses_to_start_without_a_usable_passphrase);
  RUN_TEST(test_ssid_prefers_device_id);
  RUN_TEST(test_ssid_falls_back_only_when_no_device_id);
  RUN_TEST(test_ssid_rejects_truncation);
  RUN_TEST(test_ssid_degenerate_inputs);
  return UNITY_END();
}
