// Host-native unit tests for secret_keymap.h (logical field -> NVS key).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "secret_keymap.h"

void setUp(void) {}
void tearDown(void) {}

void test_scalar_fields_map_to_short_keys(void) {
  char key[16];
  TEST_ASSERT_TRUE(secretNvsKeyFor("mqtt.password", key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("mqtt_password", key);
  TEST_ASSERT_TRUE(secretNvsKeyFor("telegram.bot_token", key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("telegram_token", key);   // not telegram_bot_token (18 > 15)
  TEST_ASSERT_TRUE(secretNvsKeyFor("web.password", key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("web_password", key);
  TEST_ASSERT_TRUE(secretNvsKeyFor("wifi.ap_password", key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING("ap_password", key);
}

void test_all_keys_fit_nvs_15_char_limit(void) {
  const char* fields[] = { "mqtt.password", "telegram.bot_token",
                           "web.password", "wifi.ap_password",
                           "wifi.password:a-very-long-ssid-name-exceeding-15" };
  char key[16];
  for (auto f : fields) {
    TEST_ASSERT_TRUE(secretNvsKeyFor(f, key, sizeof(key)));
    TEST_ASSERT_LESS_OR_EQUAL_UINT(15, strlen(key));
  }
}

void test_unknown_field_rejected(void) {
  char key[16];
  TEST_ASSERT_FALSE(secretNvsKeyFor("bogus.field", key, sizeof(key)));
  TEST_ASSERT_FALSE(secretNvsKeyFor("mqtt.broker", key, sizeof(key)));   // not a secret
  TEST_ASSERT_FALSE(secretNvsKeyFor("", key, sizeof(key)));
}

void test_wifi_password_keyed_by_ssid(void) {
  char key[16];
  TEST_ASSERT_TRUE(secretNvsKeyFor("wifi.password:RebelIOT", key, sizeof(key)));
  TEST_ASSERT_EQUAL_STRING_LEN("wifi_pw_", key, 8);
  TEST_ASSERT_FALSE(secretNvsKeyFor("wifi.password:", key, sizeof(key)));  // no ssid
}

void test_wifi_key_deterministic_and_ssid_distinct(void) {
  char a[16], b[16], c[16];
  secretWifiKey("RebelIOT", a, sizeof(a));
  secretWifiKey("RebelIOT", b, sizeof(b));
  secretWifiKey("OtherNet", c, sizeof(c));
  TEST_ASSERT_EQUAL_STRING(a, b);          // same ssid -> same key
  TEST_ASSERT_TRUE(strcmp(a, c) != 0);     // different ssid -> different key
}

void test_truncating_buffer_rejected(void) {
  char tiny[5];   // too small for any key -> clean failure, not a silent remap
  TEST_ASSERT_FALSE(secretNvsKeyFor("mqtt.password", tiny, sizeof(tiny)));
  TEST_ASSERT_FALSE(secretNvsKeyFor("wifi.password:RebelIOT", tiny, sizeof(tiny)));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_scalar_fields_map_to_short_keys);
  RUN_TEST(test_all_keys_fit_nvs_15_char_limit);
  RUN_TEST(test_unknown_field_rejected);
  RUN_TEST(test_wifi_password_keyed_by_ssid);
  RUN_TEST(test_wifi_key_deterministic_and_ssid_distinct);
  RUN_TEST(test_truncating_buffer_rejected);
  return UNITY_END();
}
