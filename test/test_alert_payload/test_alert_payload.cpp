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

// ArduinoJson 7 TextFormatter::writeChar: named escapes take two bytes, the rest go raw.
static std::string serialize(const char* sev, const char* code, const std::string& msg) {
  auto str = [](const std::string& v) {
    std::string o = "\"";
    for (char c : v) {
      const char* named = strchr("\"\\\b\f\n\r\t", c);
      if (c && named) { o += '\\'; o += "\"\\bfnrt"[named - "\"\\\b\f\n\r\t"]; }
      else o += c;
    }
    return o + "\"";
  };
  return "{\"severity\":" + str(sev) + ",\"code\":" + str(code) + ",\"message\":" + str(msg) + "}";
}

void test_control_chars_count_as_written(void) {
  TEST_ASSERT_EQUAL_UINT(1, alertJsonCharLen(0x01));
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

void test_scrubbed_message_serializes_valid_and_fits(void) {
  std::string raw;
  for (int i = 0; i < 300; i++) raw += (i % 3) ? 'x' : '\x01';
  char buf[256];
  strncpy(buf, raw.c_str(), sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';
  alertScrubControls(buf);
  size_t n = alertMessageFit("warn", "telegram", buf, 256);
  std::string out = serialize("warn", "telegram", std::string(buf, n));
  TEST_ASSERT_EQUAL_UINT(255, out.size());
  for (unsigned char c : out) TEST_ASSERT_TRUE(c >= 0x20);
}

void test_scrub_keeps_named_escapes(void) {
  char buf[] = "a\nb\x02" "c\td";
  alertScrubControls(buf);
  TEST_ASSERT_EQUAL_STRING("a\nb c\td", buf);
}

void test_breadcrumb_room_is_kept(void) {
  std::string msg(400, 'a');
  const std::string crumb = " [heap=123456]";
  size_t n = alertMessageFit("warn", "telegram", msg.c_str(), 256 - crumb.size());
  std::string out = serialize("warn", "telegram", msg.substr(0, n) + crumb);
  TEST_ASSERT_EQUAL_UINT(255, out.size());
  TEST_ASSERT_TRUE(out.find(crumb) != std::string::npos);
}

void test_utf8_cut_keeps_whole_chars(void) {
  std::string code(31, 'c');
  code += "\xC3\xA9";
  TEST_ASSERT_EQUAL_UINT(31, alertUtf8Cut(code.c_str(), 32));
  TEST_ASSERT_EQUAL_UINT(33, alertUtf8Cut(code.c_str(), 33));
  TEST_ASSERT_EQUAL_UINT(5, alertUtf8Cut("short", 32));
  TEST_ASSERT_EQUAL_UINT(0, alertUtf8Cut(nullptr, 32));
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
  RUN_TEST(test_control_chars_count_as_written);
  RUN_TEST(test_scrubbed_message_serializes_valid_and_fits);
  RUN_TEST(test_scrub_keeps_named_escapes);
  RUN_TEST(test_breadcrumb_room_is_kept);
  RUN_TEST(test_utf8_cut_keeps_whole_chars);
  RUN_TEST(test_cut_never_splits_a_utf8_char);
  RUN_TEST(test_frame_too_big_or_null_gives_zero);
  return UNITY_END();
}
