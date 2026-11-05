// Host-native unit tests for lua_timer_policy.h (Node.setTimeout deadlines).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <math.h>
#include "lua_timer_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const uint64_t WRAP32 = 0x100000000ULL;  // where 32-bit millis() wraps

void test_fireAt_adds_delay(void) {
  uint64_t at = 0;
  TEST_ASSERT_TRUE(luaTimerFireAt(1000, 500.0, &at));
  TEST_ASSERT_EQUAL_UINT64(1500, at);
}

// 1 h timer set 30 min before the 32-bit wrap: a uint32 deadline wraps to a
// small value and fires at once; the 64-bit one waits the hour.
void test_timer_across_32bit_wrap_waits_full_delay(void) {
  uint64_t now = WRAP32 - 1800000ULL;
  uint64_t at  = 0;
  TEST_ASSERT_TRUE(luaTimerFireAt(now, 3600000.0, &at));
  TEST_ASSERT_EQUAL_UINT64(WRAP32 + 1800000ULL, at);
  TEST_ASSERT_FALSE(luaTimerDue(now, at));
  TEST_ASSERT_FALSE(luaTimerDue(at - 1, at));
  TEST_ASSERT_TRUE(luaTimerDue(at, at));
}

// Uptime past the wrap keeps counting, so a timer set there still waits.
void test_timer_after_32bit_wrap(void) {
  uint64_t now = WRAP32 + 5000ULL;
  uint64_t at  = 0;
  TEST_ASSERT_TRUE(luaTimerFireAt(now, 1000.0, &at));
  TEST_ASSERT_FALSE(luaTimerDue(now, at));
  TEST_ASSERT_TRUE(luaTimerDue(now + 1000, at));
}

void test_nan_delay_is_refused(void) {
  uint64_t at = 42;
  TEST_ASSERT_FALSE(luaTimerFireAt(1000, NAN, &at));
  TEST_ASSERT_EQUAL_UINT64(42, at);
}

void test_negative_delay_is_due_now(void) {
  uint64_t at = 0;
  TEST_ASSERT_TRUE(luaTimerFireAt(1000, -5.0, &at));
  TEST_ASSERT_EQUAL_UINT64(1000, at);
  TEST_ASSERT_TRUE(luaTimerFireAt(1000, -INFINITY, &at));
  TEST_ASSERT_EQUAL_UINT64(1000, at);
  TEST_ASSERT_TRUE(luaTimerDue(1000, at));
}

// math.huge or a delay past INT_MAX caps at 30 days, never fires early.
void test_huge_delay_caps(void) {
  uint64_t at = 0;
  TEST_ASSERT_TRUE(luaTimerFireAt(1000, INFINITY, &at));
  TEST_ASSERT_EQUAL_UINT64(1000 + LUA_TIMER_MAX_DELAY_MS, at);
  TEST_ASSERT_TRUE(luaTimerFireAt(1000, 3e9, &at));
  TEST_ASSERT_EQUAL_UINT64(1000 + LUA_TIMER_MAX_DELAY_MS, at);
  TEST_ASSERT_FALSE(luaTimerDue(1000, at));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fireAt_adds_delay);
  RUN_TEST(test_timer_across_32bit_wrap_waits_full_delay);
  RUN_TEST(test_timer_after_32bit_wrap);
  RUN_TEST(test_nan_delay_is_refused);
  RUN_TEST(test_negative_delay_is_due_now);
  RUN_TEST(test_huge_delay_caps);
  return UNITY_END();
}
