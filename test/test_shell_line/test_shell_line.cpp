// Host-native unit tests for shell_line_policy.h (shell line split limits).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include <string>
#include "shell_line_policy.h"

void setUp(void) {}
void tearDown(void) {}

static char  buf[SHELL_LINE_MAX];
static char* argv[SHELL_ARGS_MAX];

// --- shellFits ----------------------------------------------------------------

void test_fits_needs_room_for_the_nul(void) {
  TEST_ASSERT_TRUE(shellFits("abc", 4));
  TEST_ASSERT_FALSE(shellFits("abcd", 4));
  TEST_ASSERT_TRUE(shellFits("", 1));
  TEST_ASSERT_FALSE(shellFits("", 0));
}

void test_fits_refuses_null(void) {
  TEST_ASSERT_FALSE(shellFits(nullptr, 128));
}

// --- shellSplitLine -----------------------------------------------------------

void test_split_tokenises_on_spaces_and_tabs(void) {
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine("  fs.mv\t/a  /sd/b ", buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(3, argc);
  TEST_ASSERT_EQUAL_STRING("fs.mv", argv[0]);
  TEST_ASSERT_EQUAL_STRING("/a", argv[1]);
  TEST_ASSERT_EQUAL_STRING("/sd/b", argv[2]);
}

void test_split_blank_line_is_zero_args(void) {
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::Ok, shellSplitLine(" \t ", buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(0, argc);
}

// A config.set of two wifi networks is ~300 bytes; the old 256-byte parse
// buffer cut the JSON and config.set stored the stub as a string.
void test_split_keeps_a_line_past_the_old_256_byte_buffer(void) {
  std::string value = "[";
  while (value.size() < 600) value += "{\"ssid\":\"net\",\"password\":\"secret-pass\"},";
  value.back() = ']';
  std::string line = "config.set wifi.networks " + value;
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(3, argc);
  TEST_ASSERT_EQUAL_STRING(value.c_str(), argv[2]);
}

void test_split_takes_the_longest_line_that_fits(void) {
  std::string line(SHELL_LINE_MAX - 1, 'x');
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(1, argc);
  TEST_ASSERT_EQUAL(SHELL_LINE_MAX - 1, strlen(argv[0]));
}

void test_split_refuses_a_line_one_byte_too_long(void) {
  std::string line(SHELL_LINE_MAX, 'x');
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::TooLong,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(0, argc);
}

void test_split_refuses_null(void) {
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::TooLong, shellSplitLine(nullptr, buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(0, argc);
}

void test_split_takes_exactly_max_args(void) {
  std::string line = "cmd";
  for (int i = 1; i < SHELL_ARGS_MAX; i++) line += " a";
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(SHELL_ARGS_MAX, argc);
}

// The old parser stopped at 16 and ran the command on the first 16 words.
void test_split_refuses_a_token_past_max_args(void) {
  std::string line = "cmd";
  for (int i = 1; i <= SHELL_ARGS_MAX; i++) line += " a";
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::TooManyArgs,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(0, argc);
}

void test_split_trailing_space_past_max_args_is_not_a_token(void) {
  std::string line = "cmd";
  for (int i = 1; i < SHELL_ARGS_MAX; i++) line += " a";
  line += " \t ";
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(SHELL_ARGS_MAX, argc);
}

void test_split_honours_a_small_buffer(void) {
  char small[8];
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::TooLong, shellSplitLine("fs.ls /sd", small, sizeof(small), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(ShellLine::Ok, shellSplitLine("fs.ls /", small, sizeof(small), argv, SHELL_ARGS_MAX, &argc));
  TEST_ASSERT_EQUAL(2, argc);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fits_needs_room_for_the_nul);
  RUN_TEST(test_fits_refuses_null);
  RUN_TEST(test_split_tokenises_on_spaces_and_tabs);
  RUN_TEST(test_split_blank_line_is_zero_args);
  RUN_TEST(test_split_keeps_a_line_past_the_old_256_byte_buffer);
  RUN_TEST(test_split_takes_the_longest_line_that_fits);
  RUN_TEST(test_split_refuses_a_line_one_byte_too_long);
  RUN_TEST(test_split_refuses_null);
  RUN_TEST(test_split_takes_exactly_max_args);
  RUN_TEST(test_split_refuses_a_token_past_max_args);
  RUN_TEST(test_split_trailing_space_past_max_args_is_not_a_token);
  RUN_TEST(test_split_honours_a_small_buffer);
  return UNITY_END();
}
