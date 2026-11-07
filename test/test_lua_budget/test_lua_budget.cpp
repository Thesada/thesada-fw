// Host-native unit tests for lua_budget_policy.h (Lua call budget).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "lua_budget_policy.h"

void setUp(void) {}
void tearDown(void) {}

static LuaBudget fresh() {
  LuaBudget b = {0, 0, 0, false};
  return b;
}

void test_idle_budget_never_raises(void) {
  LuaBudget b = fresh();
  TEST_ASSERT_FALSE(luaBudgetActive(b));
  TEST_ASSERT_FALSE(luaBudgetTick(b, 999999));
  TEST_ASSERT_FALSE(luaBudgetSpent(b));
  TEST_ASSERT_FALSE(luaBudgetLeave(b));
  TEST_ASSERT_EQUAL_UINT16(0, b.depth);
}

void test_call_raises_once_spent_and_stays_spent(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 1000);
  TEST_ASSERT_TRUE(luaBudgetActive(b));
  TEST_ASSERT_FALSE(luaBudgetTick(b, 1000 + LUA_SCRIPT_BUDGET_MS - 1));
  TEST_ASSERT_FALSE(luaBudgetSpent(b));
  TEST_ASSERT_TRUE(luaBudgetTick(b, 1000 + LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetSpent(b));
  TEST_ASSERT_TRUE(luaBudgetTick(b, 1000));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
  TEST_ASSERT_FALSE(luaBudgetActive(b));
}

// A runaway callback that keeps re-entering Lua through a binding must not
// restart its own clock on every nested call.
void test_nested_call_shares_the_outer_clock(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  for (uint32_t t = 1000; t < LUA_SCRIPT_BUDGET_MS; t += 1000) {
    luaBudgetEnter(b, t);
    TEST_ASSERT_FALSE(luaBudgetTick(b, t));
    TEST_ASSERT_FALSE(luaBudgetLeave(b));
  }
  luaBudgetEnter(b, LUA_SCRIPT_BUDGET_MS);
  TEST_ASSERT_TRUE(luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
  TEST_ASSERT_TRUE(luaBudgetSpent(b));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
  TEST_ASSERT_FALSE(luaBudgetActive(b));
}

// The next outermost call starts a fresh budget.
void test_next_call_rearms(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  TEST_ASSERT_TRUE(luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS + 5));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
  TEST_ASSERT_FALSE(luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS + 6));
  luaBudgetEnter(b, 50000);
  TEST_ASSERT_FALSE(luaBudgetSpent(b));
  TEST_ASSERT_FALSE(luaBudgetTick(b, 50001));
  TEST_ASSERT_FALSE(luaBudgetLeave(b));
}

// A caught budget error leaves the call reporting the overrun.
void test_caught_overrun_reported_at_leave(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS);
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
}

void test_budget_across_millis_wrap(void) {
  LuaBudget b = fresh();
  const uint32_t start = 0xFFFFFFFFu - 2000u;
  luaBudgetEnter(b, start);
  TEST_ASSERT_FALSE(luaBudgetTick(b, 1000));
  TEST_ASSERT_TRUE(luaBudgetTick(b, start + LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
}

// An 11 s Telegram send is native time: the script keeps its own 10 s.
void test_credit_gives_back_native_time(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  luaBudgetCredit(b, 11000);
  TEST_ASSERT_FALSE(luaBudgetTick(b, 11000));
  TEST_ASSERT_FALSE(luaBudgetTick(b, 11000 + LUA_SCRIPT_BUDGET_MS - 1));
  TEST_ASSERT_TRUE(luaBudgetTick(b, 11000 + LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
}

// A loop of slow sends gets LUA_NATIVE_CREDIT_MS back in all, nested calls
// included, then its native time counts.
void test_credit_capped_per_outermost_call(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  luaBudgetEnter(b, 0);
  luaBudgetCredit(b, LUA_NATIVE_CREDIT_MS - 1000);
  TEST_ASSERT_FALSE(luaBudgetLeave(b));
  TEST_ASSERT_FALSE(luaBudgetTick(b, LUA_NATIVE_CREDIT_MS - 1000));
  luaBudgetCredit(b, 5000);
  TEST_ASSERT_EQUAL_UINT32(LUA_NATIVE_CREDIT_MS, b.creditMs);
  TEST_ASSERT_EQUAL_UINT32(LUA_NATIVE_CREDIT_MS, b.startMs);
  luaBudgetCredit(b, 5000);
  TEST_ASSERT_EQUAL_UINT32(LUA_NATIVE_CREDIT_MS, b.startMs);
  TEST_ASSERT_TRUE(luaBudgetTick(b, LUA_NATIVE_CREDIT_MS + LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
}

void test_credit_spent_or_idle_changes_nothing(void) {
  LuaBudget b = fresh();
  luaBudgetCredit(b, 5000);
  TEST_ASSERT_EQUAL_UINT32(0, b.startMs);
  TEST_ASSERT_EQUAL_UINT32(0, b.creditMs);
  luaBudgetEnter(b, 0);
  TEST_ASSERT_TRUE(luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS));
  luaBudgetCredit(b, 5000);
  TEST_ASSERT_TRUE(luaBudgetTick(b, LUA_SCRIPT_BUDGET_MS));
  TEST_ASSERT_TRUE(luaBudgetLeave(b));
}

void test_next_call_resets_credit(void) {
  LuaBudget b = fresh();
  luaBudgetEnter(b, 0);
  luaBudgetCredit(b, LUA_NATIVE_CREDIT_MS);
  TEST_ASSERT_FALSE(luaBudgetLeave(b));
  luaBudgetEnter(b, 100000);
  TEST_ASSERT_EQUAL_UINT32(0, b.creditMs);
  luaBudgetCredit(b, 11000);
  TEST_ASSERT_FALSE(luaBudgetTick(b, 111000));
  TEST_ASSERT_FALSE(luaBudgetLeave(b));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_idle_budget_never_raises);
  RUN_TEST(test_call_raises_once_spent_and_stays_spent);
  RUN_TEST(test_nested_call_shares_the_outer_clock);
  RUN_TEST(test_next_call_rearms);
  RUN_TEST(test_caught_overrun_reported_at_leave);
  RUN_TEST(test_budget_across_millis_wrap);
  RUN_TEST(test_credit_gives_back_native_time);
  RUN_TEST(test_credit_capped_per_outermost_call);
  RUN_TEST(test_credit_spent_or_idle_changes_nothing);
  RUN_TEST(test_next_call_resets_credit);
  return UNITY_END();
}
