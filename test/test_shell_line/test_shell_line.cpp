// Host-native unit tests for shell_line_policy.h (line split, nesting, result).
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

// --- restFrom (#42) ------------------------------------------------------------

// A passphrase with two spaces or a tab was rebuilt from argv with one space.
void test_rest_keeps_inner_whitespace_of_the_value(void) {
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine("config.set  wifi.password \t two  spaces\tand tab \t", buf, sizeof(buf),
                                   argv, SHELL_ARGS_MAX, &argc, 2));
  TEST_ASSERT_EQUAL(3, argc);
  TEST_ASSERT_EQUAL_STRING("config.set", argv[0]);
  TEST_ASSERT_EQUAL_STRING("wifi.password", argv[1]);
  TEST_ASSERT_EQUAL_STRING("two  spaces\tand tab", argv[2]);
}

void test_rest_is_absent_when_the_line_stops_before_it(void) {
  int argc = -1;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine("config.set key  \t", buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc, 2));
  TEST_ASSERT_EQUAL(2, argc);
  TEST_ASSERT_EQUAL_STRING("key", argv[1]);
}

// The rest is one token, so a value of many words is not refused as too many.
void test_rest_is_one_token_past_max_args(void) {
  std::string line = "secret.set wifi.password";
  for (int i = 0; i < SHELL_ARGS_MAX * 2; i++) line += " w";
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(line.c_str(), buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc, 2));
  TEST_ASSERT_EQUAL(3, argc);
  TEST_ASSERT_EQUAL(SHELL_ARGS_MAX * 4 - 1, strlen(argv[2]));
}

// restFrom 1 isolates the command name for the registry lookup.
void test_rest_from_one_splits_off_the_name(void) {
  int argc = 0;
  TEST_ASSERT_EQUAL(ShellLine::Ok,
                    shellSplitLine(" fs.write /a  b ", buf, sizeof(buf), argv, SHELL_ARGS_MAX, &argc, 1));
  TEST_ASSERT_EQUAL(2, argc);
  TEST_ASSERT_EQUAL_STRING("fs.write", argv[0]);
  TEST_ASSERT_EQUAL_STRING("/a  b", argv[1]);
}

// --- shellParseSlot (#35) -------------------------------------------------------

// One static buffer let a nested console command overwrite the outer argv.
void test_nested_command_gets_its_own_parse_slot(void) {
  TEST_ASSERT_EQUAL(0, shellParseSlot(0));
  TEST_ASSERT_EQUAL(1, shellParseSlot(1));
  TEST_ASSERT_NOT_EQUAL(shellParseSlot(0), shellParseSlot(1));
}

void test_nesting_past_the_slots_is_refused(void) {
  TEST_ASSERT_EQUAL(-1, shellParseSlot(SHELL_NEST_MAX));
  TEST_ASSERT_EQUAL(-1, shellParseSlot(-1));
}

// --- shellPumpAllowed (#69) -------------------------------------------------------

// The net.http task can reach a console pump in a modem hard reset. It must
// not run lines on the static buffer and depth the loop task is using.
void test_pump_runs_only_on_the_console_task(void) {
  int loopTask = 0, fetchTask = 0;
  TEST_ASSERT_TRUE(shellPumpAllowed(&loopTask, &loopTask));
  TEST_ASSERT_FALSE(shellPumpAllowed(&fetchTask, &loopTask));
}

void test_pump_runs_before_the_owner_is_known(void) {
  int loopTask = 0;
  TEST_ASSERT_TRUE(shellPumpAllowed(&loopTask, nullptr));
}

// --- shellDispatchResult (#33) ----------------------------------------------------

// Every shell error went out on cli/response as ok:true.
void test_shell_errors_are_not_ok(void) {
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::TooLong, 0, false, false) == ShellResult::Failed);
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::TooManyArgs, 0, false, false) == ShellResult::Failed);
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::Ok, 1, false, false) == ShellResult::Failed);
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::Ok, 3, true, true) == ShellResult::Failed);
}

void test_a_command_that_ran_clean_is_ok(void) {
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::Ok, 3, true, false) == ShellResult::Ok);
  TEST_ASSERT_TRUE(shellDispatchResult(ShellLine::Ok, 0, false, false) == ShellResult::Ok);
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
  RUN_TEST(test_rest_keeps_inner_whitespace_of_the_value);
  RUN_TEST(test_rest_is_absent_when_the_line_stops_before_it);
  RUN_TEST(test_rest_is_one_token_past_max_args);
  RUN_TEST(test_rest_from_one_splits_off_the_name);
  RUN_TEST(test_nested_command_gets_its_own_parse_slot);
  RUN_TEST(test_nesting_past_the_slots_is_refused);
  RUN_TEST(test_pump_runs_only_on_the_console_task);
  RUN_TEST(test_pump_runs_before_the_owner_is_known);
  RUN_TEST(test_shell_errors_are_not_ok);
  RUN_TEST(test_a_command_that_ran_clean_is_ok);
  return UNITY_END();
}
