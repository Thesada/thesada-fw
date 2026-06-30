// Host-native unit tests for cli_payload.h ("<field>\n<value>" parse bounds).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "cli_payload.h"

void setUp(void) {}
void tearDown(void) {}

void test_normal_split(void) {
  const char* p = "mqtt.password\nhunter2";
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::Ok ==
    cliSplitFieldValue(p, strlen(p), field, sizeof(field), &value, &vlen));
  TEST_ASSERT_EQUAL_STRING("mqtt.password", field);
  TEST_ASSERT_EQUAL_UINT(7, (unsigned)vlen);
  TEST_ASSERT_EQUAL_STRING_LEN("hunter2", value, 7);
}

void test_missing_newline(void) {
  const char* p = "mqtt.password";
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::NoNewline ==
    cliSplitFieldValue(p, strlen(p), field, sizeof(field), &value, &vlen));
}

void test_overlong_field_rejected_not_clipped(void) {
  // 50-char field (> 48 cap) then newline + value. Must reject, not clip.
  char p[80];
  memset(p, 'A', 50); p[50] = '\n'; memcpy(p + 51, "val", 3);
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::FieldTooLong ==
    cliSplitFieldValue(p, 54, field, sizeof(field), &value, &vlen));
}

void test_empty_value(void) {
  const char* p = "web.password\n";
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::Ok ==
    cliSplitFieldValue(p, strlen(p), field, sizeof(field), &value, &vlen));
  TEST_ASSERT_EQUAL_STRING("web.password", field);
  TEST_ASSERT_EQUAL_UINT(0, (unsigned)vlen);
}

void test_value_length_uses_real_field_len(void) {
  // Regression for the over-read: a 47-char field (fits the 48 cap) must yield
  // a value length derived from the real field length, never a clipped one.
  char p[80];
  memset(p, 'B', 47); p[47] = '\n'; memcpy(p + 48, "secretval", 9);
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::Ok ==
    cliSplitFieldValue(p, 48 + 9, field, sizeof(field), &value, &vlen));
  TEST_ASSERT_EQUAL_UINT(9, (unsigned)vlen);
}

void test_newline_in_value_kept(void) {
  const char* p = "field\nval\nwith\nnl";
  char field[48]; const char* value; size_t vlen;
  TEST_ASSERT_TRUE(CliSplit::Ok ==
    cliSplitFieldValue(p, strlen(p), field, sizeof(field), &value, &vlen));
  TEST_ASSERT_EQUAL_STRING("field", field);
  TEST_ASSERT_EQUAL_UINT(11, (unsigned)vlen);   // "val\nwith\nnl"
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_normal_split);
  RUN_TEST(test_missing_newline);
  RUN_TEST(test_overlong_field_rejected_not_clipped);
  RUN_TEST(test_empty_value);
  RUN_TEST(test_value_length_uses_real_field_len);
  RUN_TEST(test_newline_in_value_kept);
  return UNITY_END();
}
