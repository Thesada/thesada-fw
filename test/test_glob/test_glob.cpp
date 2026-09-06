// Host-native unit tests for glob_policy.h (wildcard fs.rm / fs.ls).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "glob_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- globHasWildcard ----------------------------------------------------------

void test_detects_wildcards(void) {
  TEST_ASSERT_TRUE(globHasWildcard("*.csv"));
  TEST_ASSERT_TRUE(globHasWildcard("log?.txt"));
  TEST_ASSERT_FALSE(globHasWildcard("plain.txt"));
  TEST_ASSERT_FALSE(globHasWildcard(nullptr));
}

// --- globMatch ----------------------------------------------------------------

void test_star_matches_any_run(void) {
  TEST_ASSERT_TRUE(globMatch("*.csv", "readings.csv"));
  TEST_ASSERT_TRUE(globMatch("*", "anything"));
  TEST_ASSERT_TRUE(globMatch("log*", "log"));
  TEST_ASSERT_FALSE(globMatch("*.csv", "readings.txt"));
}

void test_question_matches_exactly_one(void) {
  TEST_ASSERT_TRUE(globMatch("log?.txt", "log1.txt"));
  TEST_ASSERT_FALSE(globMatch("log?.txt", "log.txt"));
  TEST_ASSERT_FALSE(globMatch("log?.txt", "log12.txt"));
}

// A literal pattern still has to match exactly, so the same matcher serves
// the no-wildcard case without a separate path.
void test_literal_pattern_is_exact(void) {
  TEST_ASSERT_TRUE(globMatch("config.json", "config.json"));
  TEST_ASSERT_FALSE(globMatch("config.json", "config.json.bak"));
}

// Backtracking: a greedy first star must not consume the tail.
void test_multiple_stars_backtrack(void) {
  TEST_ASSERT_TRUE(globMatch("*-*.log", "owb-2026.log"));
  TEST_ASSERT_TRUE(globMatch("a*b*c", "axxbyyc"));
  TEST_ASSERT_FALSE(globMatch("a*b*c", "axxbyy"));
}

void test_null_is_never_a_match(void) {
  TEST_ASSERT_FALSE(globMatch(nullptr, "x"));
  TEST_ASSERT_FALSE(globMatch("*", nullptr));
}

// --- globSplit ----------------------------------------------------------------

void test_splits_dir_and_pattern(void) {
  char d[64], p[64];
  TEST_ASSERT_TRUE(globSplit("/sd/logs/*.csv", d, sizeof(d), p, sizeof(p)));
  TEST_ASSERT_EQUAL_STRING("/sd/logs", d);
  TEST_ASSERT_EQUAL_STRING("*.csv", p);
}

// A pattern directly under root keeps the root as its directory.
void test_splits_at_root(void) {
  char d[64], p[64];
  TEST_ASSERT_TRUE(globSplit("/*.log", d, sizeof(d), p, sizeof(p)));
  TEST_ASSERT_EQUAL_STRING("/", d);
  TEST_ASSERT_EQUAL_STRING("*.log", p);
}

// Only the last segment may hold a wildcard - matching across directories is
// not supported, and silently treating it as literal would delete the wrong set.
void test_wildcard_outside_last_segment_is_refused(void) {
  char d[64], p[64];
  TEST_ASSERT_FALSE(globSplit("/sd/*/x.csv", d, sizeof(d), p, sizeof(p)));
  TEST_ASSERT_FALSE(globSplit("/*/logs/x.csv", d, sizeof(d), p, sizeof(p)));
}

void test_split_refuses_overflow(void) {
  char d[4], p[4];
  TEST_ASSERT_FALSE(globSplit("/sd/logs/verylongname*.csv", d, sizeof(d), p, sizeof(p)));
}

// --- globRmProtected ----------------------------------------------------------

// A wildcard at the LittleFS root must never take the two files the firmware
// needs to boot and to verify TLS, whatever the pattern says.
void test_root_config_and_ca_are_protected(void) {
  TEST_ASSERT_TRUE(globRmProtected("/", "config.json"));
  TEST_ASSERT_TRUE(globRmProtected("/", "ca.crt"));
}

void test_other_root_files_are_not_protected(void) {
  TEST_ASSERT_FALSE(globRmProtected("/", "readings.csv"));
  TEST_ASSERT_FALSE(globRmProtected("/", "config.json.bak"));
}

// Same names elsewhere are ordinary files - only the live ones at the root
// are load-bearing.
void test_same_names_off_root_are_not_protected(void) {
  TEST_ASSERT_FALSE(globRmProtected("/sd", "config.json"));
  TEST_ASSERT_FALSE(globRmProtected("/backup", "ca.crt"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_detects_wildcards);
  RUN_TEST(test_star_matches_any_run);
  RUN_TEST(test_question_matches_exactly_one);
  RUN_TEST(test_literal_pattern_is_exact);
  RUN_TEST(test_multiple_stars_backtrack);
  RUN_TEST(test_null_is_never_a_match);
  RUN_TEST(test_splits_dir_and_pattern);
  RUN_TEST(test_splits_at_root);
  RUN_TEST(test_wildcard_outside_last_segment_is_refused);
  RUN_TEST(test_split_refuses_overflow);
  RUN_TEST(test_root_config_and_ca_are_protected);
  RUN_TEST(test_other_root_files_are_not_protected);
  RUN_TEST(test_same_names_off_root_are_not_protected);
  return UNITY_END();
}
