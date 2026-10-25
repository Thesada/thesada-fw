// Host-native unit tests for boot_wait_policy.h (task watchdog wait budget).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "boot_wait_policy.h"

void setUp(void) {}
void tearDown(void) {}

// Every poll step a wait uses sleeps well under the watchdog period.
void test_poll_steps_fit_watchdog(void) {
  const uint32_t steps[] = {WIFI_POLL_STEP_MS, NTP_POLL_STEP_MS};
  for (uint32_t s : steps) {
    TEST_ASSERT_EQUAL_UINT32(s, waitStepMs(s));
    TEST_ASSERT_TRUE(waitStepMs(s) * 2 <= TASK_WDT_TIMEOUT_MS);
  }
}

// Any requested step is clamped to 1..WAIT_STEP_MAX_MS.
void test_wait_step_clamps(void) {
  TEST_ASSERT_EQUAL_UINT32(1, waitStepMs(0));
  TEST_ASSERT_EQUAL_UINT32(WAIT_STEP_MAX_MS, waitStepMs(WAIT_STEP_MAX_MS));
  TEST_ASSERT_EQUAL_UINT32(WAIT_STEP_MAX_MS, waitStepMs(TASK_WDT_TIMEOUT_MS));
  TEST_ASSERT_EQUAL_UINT32(WAIT_STEP_MAX_MS, waitStepMs(0xFFFFFFFFUL));
  TEST_ASSERT_TRUE(WAIT_STEP_MAX_MS * 2 <= TASK_WDT_TIMEOUT_MS);
}

void test_ssid_timeout_clamps(void) {
  TEST_ASSERT_EQUAL_UINT32(10000, wifiSsidTimeoutMs(WIFI_SSID_TIMEOUT_DEF_S));
  TEST_ASSERT_EQUAL_UINT32(1000, wifiSsidTimeoutMs(0));
  TEST_ASSERT_EQUAL_UINT32(1000, wifiSsidTimeoutMs(-5));
  TEST_ASSERT_EQUAL_UINT32(60000, wifiSsidTimeoutMs(60));
  TEST_ASSERT_EQUAL_UINT32(60000, wifiSsidTimeoutMs(3600));
  TEST_ASSERT_EQUAL_UINT32(60000, wifiSsidTimeoutMs(INT32_MAX));
}

void test_retries_clamp(void) {
  TEST_ASSERT_EQUAL_UINT8(2, wifiRetries(WIFI_RETRIES_DEF));
  TEST_ASSERT_EQUAL_UINT8(1, wifiRetries(0));
  TEST_ASSERT_EQUAL_UINT8(1, wifiRetries(-1));
  TEST_ASSERT_EQUAL_UINT8(5, wifiRetries(5));
  TEST_ASSERT_EQUAL_UINT8(5, wifiRetries(255));
  TEST_ASSERT_EQUAL_UINT8(5, wifiRetries(256));
}

// The issue case: defaults, two failing SSIDs = 40 s, past the 30 s period,
// which is why the connect loop must feed per step.
void test_default_two_ssids_exceeds_period(void) {
  uint32_t b = wifiConnectBudgetMs(WIFI_RETRIES_DEF, 2, WIFI_SSID_TIMEOUT_DEF_S);
  TEST_ASSERT_EQUAL_UINT32(40000, b);
  TEST_ASSERT_TRUE(b > TASK_WDT_TIMEOUT_MS);
}

// Any config: the total stays bounded, and the unfed span is one poll step.
void test_any_config_is_bounded(void) {
  const uint32_t cap = (uint32_t)WIFI_RETRIES_MAX * WIFI_CANDIDATES_MAX *
                       (uint32_t)WIFI_SSID_TIMEOUT_MAX_S * 1000UL;
  const int32_t retries[]  = {-1, 0, 1, 2, 5, 6, 255, INT32_MAX};
  const int32_t ssids[]    = {-1, 0, 1, 2, 8, 9, 64};
  const int32_t timeouts[] = {-1, 0, 1, 10, 60, 61, 3600, INT32_MAX};
  for (int32_t r : retries)
    for (int32_t n : ssids)
      for (int32_t t : timeouts) {
        TEST_ASSERT_TRUE(wifiConnectBudgetMs(r, n, t) <= cap);
        TEST_ASSERT_TRUE(waitStepMs(WIFI_POLL_STEP_MS) < TASK_WDT_TIMEOUT_MS);
      }
  TEST_ASSERT_EQUAL_UINT32(cap, wifiConnectBudgetMs(INT32_MAX, 64, INT32_MAX));
  TEST_ASSERT_EQUAL_UINT32(0, wifiConnectBudgetMs(2, 0, 10));
  TEST_ASSERT_EQUAL_UINT32(0, wifiConnectBudgetMs(2, -3, 10));
}

void test_budget_spent_rollover_safe(void) {
  TEST_ASSERT_FALSE(waitBudgetSpent(1000, 1000, 15000));
  TEST_ASSERT_FALSE(waitBudgetSpent(1000, 15999, 15000));
  TEST_ASSERT_TRUE(waitBudgetSpent(1000, 16000, 15000));
  // Start just before the 32-bit wrap, now just after it.
  TEST_ASSERT_FALSE(waitBudgetSpent(0xFFFFF000UL, 0x00000100UL, 15000));
  TEST_ASSERT_TRUE(waitBudgetSpent(0xFFFFF000UL, 0x00003000UL, 10000));
}

// A boot script is cut off well inside one watchdog period of hook feeds.
void test_lua_budget_inside_period(void) {
  TEST_ASSERT_TRUE(LUA_SCRIPT_BUDGET_MS < TASK_WDT_TIMEOUT_MS);
  TEST_ASSERT_TRUE(LUA_HOOK_COUNT > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_poll_steps_fit_watchdog);
  RUN_TEST(test_wait_step_clamps);
  RUN_TEST(test_ssid_timeout_clamps);
  RUN_TEST(test_retries_clamp);
  RUN_TEST(test_default_two_ssids_exceeds_period);
  RUN_TEST(test_any_config_is_bounded);
  RUN_TEST(test_budget_spent_rollover_safe);
  RUN_TEST(test_lua_budget_inside_period);
  return UNITY_END();
}
