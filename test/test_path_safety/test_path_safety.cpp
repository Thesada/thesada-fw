// Host-native unit tests for path_safety_policy.h (operator-supplied paths).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "path_safety_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_plain_absolute_paths_pass(void) {
  TEST_ASSERT_TRUE(pathSafePolicy("/"));
  TEST_ASSERT_TRUE(pathSafePolicy("/config.json"));
  TEST_ASSERT_TRUE(pathSafePolicy("/scripts/alerts.lua"));
  TEST_ASSERT_TRUE(pathSafePolicy("/sd/readings.csv"));
}

void test_empty_and_null_fail(void) {
  TEST_ASSERT_FALSE(pathSafePolicy(nullptr));
  TEST_ASSERT_FALSE(pathSafePolicy(""));
}

// Everything the transports pass in is treated as absolute, so a relative
// path is a caller bug, not a shorthand to resolve.
void test_relative_paths_fail(void) {
  TEST_ASSERT_FALSE(pathSafePolicy("config.json"));
  TEST_ASSERT_FALSE(pathSafePolicy("./config.json"));
  TEST_ASSERT_FALSE(pathSafePolicy(" /config.json"));
}

void test_traversal_fails_anywhere_in_the_path(void) {
  TEST_ASSERT_FALSE(pathSafePolicy("/../config.json"));
  TEST_ASSERT_FALSE(pathSafePolicy("/scripts/../../config.json"));
  TEST_ASSERT_FALSE(pathSafePolicy("/scripts/.."));
}

// The rule is a substring match, so a legitimate double dot inside a filename
// is rejected too. Documented on purpose: the false positive is cheap, and
// loosening it means parsing segments.
void test_dots_inside_a_filename_also_fail(void) {
  TEST_ASSERT_FALSE(pathSafePolicy("/data/log..txt"));
  TEST_ASSERT_FALSE(pathSafePolicy("/backup..2026/x"));
}

// A single dot is not traversal and stays allowed.
void test_single_dot_segments_pass(void) {
  TEST_ASSERT_TRUE(pathSafePolicy("/data/.hidden"));
  TEST_ASSERT_TRUE(pathSafePolicy("/a.b/c.d"));
}

// Empty segments are rejected so one file has one spelling. The allow-listed
// dynamic open sites assume that.
void test_empty_segments_fail(void) {
  TEST_ASSERT_FALSE(pathSafePolicy("//"));
  TEST_ASSERT_FALSE(pathSafePolicy("//config.json"));
  TEST_ASSERT_FALSE(pathSafePolicy("/scripts//alerts.lua"));
  TEST_ASSERT_FALSE(pathSafePolicy("/scripts/alerts.lua//"));
}

// Trailing slash on a directory is normal usage and must survive.
void test_trailing_slash_passes(void) {
  TEST_ASSERT_TRUE(pathSafePolicy("/scripts/"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_plain_absolute_paths_pass);
  RUN_TEST(test_empty_and_null_fail);
  RUN_TEST(test_relative_paths_fail);
  RUN_TEST(test_traversal_fails_anywhere_in_the_path);
  RUN_TEST(test_dots_inside_a_filename_also_fail);
  RUN_TEST(test_single_dot_segments_pass);
  RUN_TEST(test_empty_segments_fail);
  RUN_TEST(test_trailing_slash_passes);
  return UNITY_END();
}
