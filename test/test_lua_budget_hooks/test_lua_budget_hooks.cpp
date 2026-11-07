// Host tests for lua_budget_hooks.h on the Lua fork the boards link: the
// budget holds in steady state, in coroutines and under pcall.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "lua_budget_hooks.h"

static uint32_t clockMs = 0;
static uint32_t stepMs = 0;  // added on every clock read
static int feeds = 0;
static uint32_t fakeNow() { clockMs += stepMs; return clockMs; }
static void fakeFeed() { feeds++; }

static LuaBudgetHost host;
static lua_State* L = nullptr;
static char msg[160];

// slowSend(credit): blocks 11 s of fake time, given back when credit is true.
static int slowSend(lua_State* S) {
  const uint32_t t0 = clockMs;
  clockMs += 11000;
  if (lua_toboolean(S, 1)) luaBudgetCredit(host.budget, clockMs - t0);
  return 0;
}

void setUp(void) {
  clockMs = 0;
  stepMs = 0;
  feeds = 0;
  host = {{0, 0, 0, false}, fakeNow, fakeFeed};
  L = luaL_newstate();
  luaBudgetInstall(L, &host);
  luaOpenLibs(L);
  luaBudgetWrapProtectedCalls(L);
  lua_register(L, "slowSend", slowSend);
}

void tearDown(void) {
  lua_close(L);
  L = nullptr;
}

// in: chunk. out: luaBudgetedCall code, its error text in msg.
static int run(const char* code) {
  TEST_ASSERT_EQUAL_INT(LUA_OK, luaL_loadstring(L, code));
  const int rc = luaBudgetedCall(L, 0, 0);
  msg[0] = '\0';
  if (rc != LUA_OK) {
    const char* e = lua_tostring(L, -1);
    snprintf(msg, sizeof(msg), "%s", e ? e : "");
    lua_pop(L, 1);
  }
  TEST_ASSERT_EQUAL_INT(0, lua_gettop(L));
  return rc;
}

static bool globalIsNil(const char* name) {
  const bool nil = lua_getglobal(L, name) == LUA_TNIL;
  lua_pop(L, 1);
  return nil;
}

// Bounded, so a missing hook fails the test instead of hanging it.
#define RUNAWAY "for i = 1, 100000000 do end "

void test_runaway_call_raises(void) {
  stepMs = 1;
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run(RUNAWAY));
  TEST_ASSERT_NOT_NULL(strstr(msg, "script.budget_exceeded"));
  TEST_ASSERT_TRUE(feeds > 1);
  TEST_ASSERT_FALSE(luaBudgetActive(host.budget));
}

void test_call_inside_budget_returns_and_feeds(void) {
  stepMs = 1;
  TEST_ASSERT_EQUAL_INT(LUA_OK, run("for i = 1, 100000 do end"));
  TEST_ASSERT_TRUE(feeds > 1);
}

void test_coroutine_created_after_setup_is_budgeted(void) {
  stepMs = 1;
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run(
      "local co = coroutine.create(function() " RUNAWAY "end) "
      "finished = coroutine.resume(co)"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "script.budget_exceeded"));
  TEST_ASSERT_EQUAL_INT(LUA_TBOOLEAN, lua_getglobal(L, "finished"));
  TEST_ASSERT_FALSE(lua_toboolean(L, -1));
  lua_pop(L, 1);
}

// A coroutine saved by one call runs under the budget of the call resuming it.
void test_saved_coroutine_runs_under_the_resuming_call(void) {
  stepMs = 1;
  TEST_ASSERT_EQUAL_INT(LUA_OK, run(
      "co = coroutine.create(function() coroutine.yield() " RUNAWAY "end) "
      "coroutine.resume(co)"));
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run("coroutine.resume(co)"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "script.budget_exceeded"));
}

void test_pcall_cannot_swallow_the_budget(void) {
  stepMs = 1;
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run(
      "pcall(function() " RUNAWAY "end) survived = true"));
  TEST_ASSERT_TRUE(globalIsNil("survived"));
}

void test_credited_native_time_is_not_script_time(void) {
  TEST_ASSERT_EQUAL_INT(LUA_OK, run("slowSend(true) for i = 1, 5000 do end"));
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run("slowSend(false) for i = 1, 5000 do end"));
  TEST_ASSERT_NOT_NULL(strstr(msg, "script.budget_exceeded"));
}

// Credit stops at LUA_NATIVE_CREDIT_MS, so the seventh 11 s send spends the budget.
void test_loop_of_slow_sends_ends(void) {
  TEST_ASSERT_EQUAL_INT(LUA_ERRRUN, run(
      "for n = 1, 100 do slowSend(true) for i = 1, 2000 do end sends = n end"));
  TEST_ASSERT_EQUAL_INT(LUA_TNUMBER, lua_getglobal(L, "sends"));
  TEST_ASSERT_EQUAL_INT(6, (int)lua_tointeger(L, -1));
  lua_pop(L, 1);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_runaway_call_raises);
  RUN_TEST(test_call_inside_budget_returns_and_feeds);
  RUN_TEST(test_coroutine_created_after_setup_is_budgeted);
  RUN_TEST(test_saved_coroutine_runs_under_the_resuming_call);
  RUN_TEST(test_pcall_cannot_swallow_the_budget);
  RUN_TEST(test_credited_native_time_is_not_script_time);
  RUN_TEST(test_loop_of_slow_sends_ends);
  return UNITY_END();
}
