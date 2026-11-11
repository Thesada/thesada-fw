// Host-native unit tests for elapsed_policy.h (millis() deadlines and spans).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "elapsed_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_reached_plain(void) {
  TEST_ASSERT_FALSE(msReached(1000, 2000));
  TEST_ASSERT_TRUE(msReached(2000, 2000));
  TEST_ASSERT_TRUE(msReached(2001, 2000));
}

// A deadline set just before the 49.7-day wrap lands past zero: not reached
// before the wrap, reached after it.
void test_reached_across_wrap(void) {
  uint32_t start = 0xFFFFF000UL;
  uint32_t at    = start + 10000UL;
  TEST_ASSERT_TRUE(at < start);
  TEST_ASSERT_FALSE(msReached(start, at));
  TEST_ASSERT_FALSE(msReached(0xFFFFFFFFUL, at));
  TEST_ASSERT_TRUE(msReached(at, at));
  TEST_ASSERT_TRUE(msReached(at + 1, at));
}

void test_within_plain(void) {
  TEST_ASSERT_TRUE(msWithin(1000, 1000, 500));
  TEST_ASSERT_TRUE(msWithin(1499, 1000, 500));
  TEST_ASSERT_FALSE(msWithin(1500, 1000, 500));
  TEST_ASSERT_FALSE(msWithin(1000, 1000, 0));
}

// A 5 min download cap started 4 s before the wrap still runs its 5 min.
void test_within_across_wrap(void) {
  uint32_t start = 0xFFFFF000UL;
  uint32_t span  = 5UL * 60UL * 1000UL;
  TEST_ASSERT_TRUE(msWithin(start, start, span));
  TEST_ASSERT_TRUE(msWithin(start + 100, start, span));
  TEST_ASSERT_TRUE(msWithin(start + span - 1, start, span));
  TEST_ASSERT_FALSE(msWithin(start + span, start, span));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reached_plain);
  RUN_TEST(test_reached_across_wrap);
  RUN_TEST(test_within_plain);
  RUN_TEST(test_within_across_wrap);
  return UNITY_END();
}
