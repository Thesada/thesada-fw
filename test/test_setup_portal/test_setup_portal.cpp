// Host-native unit tests for setup_portal_policy.h.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "setup_portal_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_portal_opens_only_on_the_ap_without_a_station(void) {
  TEST_ASSERT_TRUE(setupPortalOpen(true, 0, false));
  TEST_ASSERT_FALSE(setupPortalOpen(false, 0, false));
  TEST_ASSERT_FALSE(setupPortalOpen(true, 1, false));
  TEST_ASSERT_TRUE(setupPortalOpen(true, 1, true));
  TEST_ASSERT_FALSE(setupPortalOpen(false, 0, true));
}

void test_ssid_is_one_to_32_bytes_without_controls(void) {
  TEST_ASSERT_TRUE(setupSsidUsable("RebelIOT"));
  TEST_ASSERT_TRUE(setupSsidUsable("a b"));
  TEST_ASSERT_TRUE(setupSsidUsable("bad<ssid>"));
  TEST_ASSERT_TRUE(setupSsidUsable("say \"hi\""));
  TEST_ASSERT_TRUE(setupSsidUsable("caf\xc3\xa9"));
  TEST_ASSERT_FALSE(setupSsidUsable(""));
  TEST_ASSERT_FALSE(setupSsidUsable(nullptr));
  TEST_ASSERT_FALSE(setupSsidUsable("has\nline"));
  char longSsid[34];
  memset(longSsid, 'a', 33);
  longSsid[33] = '\0';
  TEST_ASSERT_FALSE(setupSsidUsable(longSsid));
  longSsid[32] = '\0';
  TEST_ASSERT_TRUE(setupSsidUsable(longSsid));
}

void test_passphrase_is_8_to_63_printable(void) {
  TEST_ASSERT_TRUE(setupPassUsable("12345678"));
  TEST_ASSERT_FALSE(setupPassUsable("short"));
  TEST_ASSERT_FALSE(setupPassUsable(nullptr));
  char pass63[64];
  memset(pass63, 'p', 63);
  pass63[63] = '\0';
  TEST_ASSERT_TRUE(setupPassUsable(pass63));
  char pass64[65];
  memset(pass64, 'p', 64);
  pass64[64] = '\0';
  TEST_ASSERT_FALSE(setupPassUsable(pass64));
}

void test_form_needs_an_https_url(void) {
  TEST_ASSERT_TRUE(setupFormUsable("RebelIOT", "12345678", "https://thesada.app"));
  TEST_ASSERT_FALSE(setupFormUsable("RebelIOT", "12345678", "http://thesada.app"));
  TEST_ASSERT_FALSE(setupFormUsable("RebelIOT", "short", "https://thesada.app"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_portal_opens_only_on_the_ap_without_a_station);
  RUN_TEST(test_ssid_is_one_to_32_bytes_without_controls);
  RUN_TEST(test_passphrase_is_8_to_63_printable);
  RUN_TEST(test_form_needs_an_https_url);
  return UNITY_END();
}
