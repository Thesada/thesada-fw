// Host-native unit tests for alert_payload_policy.h (Telegram relay alert JSON).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string>
#include "alert_payload_policy.h"

void setUp(void) {}
void tearDown(void) {}

static size_t jsonLen(const char* sev, const char* code, const std::string& msg) {
  return strlen("{\"severity\":\"\",\"code\":\"\",\"message\":\"\"}") +
         alertJsonStrLen(sev, strlen(sev)) + alertJsonStrLen(code, strlen(code)) +
         alertJsonStrLen(msg.c_str(), msg.size());
}

void test_short_message_fits_whole(void) {
  TEST_ASSERT_EQUAL_UINT(5, alertMessageFit("warn", "telegram", "hello", 256));
}

void test_long_message_is_cut_to_the_slot(void) {
  std::string msg(400, 'a');
  size_t n = alertMessageFit("warn", "telegram", msg.c_str(), 256);
  TEST_ASSERT_TRUE(n < 400);
  TEST_ASSERT_EQUAL_UINT(255, jsonLen("warn", "telegram", msg.substr(0, n)));
}

void test_escapes_count_double(void) {
  std::string msg(300, '"');
  size_t n = alertMessageFit("warn", "telegram", msg.c_str(), 256);
  TEST_ASSERT_TRUE(jsonLen("warn", "telegram", msg.substr(0, n)) <= 255);
  TEST_ASSERT_TRUE(jsonLen("warn", "telegram", msg.substr(0, n + 1)) > 255);
}

void test_control_chars_take_six(void) {
  TEST_ASSERT_EQUAL_UINT(6, alertJsonCharLen(0x01));
  TEST_ASSERT_EQUAL_UINT(2, alertJsonCharLen('\n'));
  TEST_ASSERT_EQUAL_UINT(1, alertJsonCharLen('a'));
  TEST_ASSERT_EQUAL_UINT(1, alertJsonCharLen(0xC3));
}

void test_cut_never_splits_a_utf8_char(void) {
  std::string msg;
  for (int i = 0; i < 200; i++) msg += "\xC3\xA9";
  size_t n = alertMessageFit("warn", "telegram", msg.c_str(), 256);
  TEST_ASSERT_TRUE(n > 0 && n < msg.size());
  TEST_ASSERT_EQUAL_UINT(0, n % 2);
}

void test_frame_too_big_or_null_gives_zero(void) {
  std::string code(300, 'c');
  TEST_ASSERT_EQUAL_UINT(0, alertMessageFit("warn", code.c_str(), "hi", 256));
  TEST_ASSERT_EQUAL_UINT(0, alertMessageFit(nullptr, "c", "hi", 256));
  TEST_ASSERT_EQUAL_UINT(0, alertMessageFit("warn", "c", "hi", 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_short_message_fits_whole);
  RUN_TEST(test_long_message_is_cut_to_the_slot);
  RUN_TEST(test_escapes_count_double);
  RUN_TEST(test_control_chars_take_six);
  RUN_TEST(test_cut_never_splits_a_utf8_char);
  RUN_TEST(test_frame_too_big_or_null_gives_zero);
  return UNITY_END();
}
