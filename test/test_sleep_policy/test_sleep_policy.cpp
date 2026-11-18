// Host-native unit tests for sleep_policy.h (SleepManager awake window).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "sleep_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_span_floor_and_plain(void) {
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MIN * 1000UL, sleepWakeSpanMs(0));
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MIN * 1000UL, sleepWakeSpanMs(9));
  TEST_ASSERT_EQUAL_UINT32(30000UL, sleepWakeSpanMs(30));
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MAX * 1000UL, sleepWakeSpanMs(SLEEP_WAKE_S_MAX));
}

// CR #126: wake_s 2,592,000 put the deadline past the signed range, so the
// node slept straight after boot. 4,294,968 s wrapped the x1000 to 704 ms.
void test_span_capped_no_overflow(void) {
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MAX * 1000UL, sleepWakeSpanMs(2592000UL));
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MAX * 1000UL, sleepWakeSpanMs(4294968UL));
  TEST_ASSERT_EQUAL_UINT32(SLEEP_WAKE_S_MAX * 1000UL, sleepWakeSpanMs(0xFFFFFFFFUL));
}

void test_done_plain(void) {
  uint32_t span = sleepWakeSpanMs(30);
  TEST_ASSERT_FALSE(sleepWakeDone(1000, 1000, span));
  TEST_ASSERT_FALSE(sleepWakeDone(1000 + span - 1, 1000, span));
  TEST_ASSERT_TRUE(sleepWakeDone(1000 + span, 1000, span));
}

void test_huge_wake_not_done_at_boot(void) {
  uint32_t span = sleepWakeSpanMs(2592000UL);
  TEST_ASSERT_FALSE(sleepWakeDone(5000, 5000, span));
  TEST_ASSERT_FALSE(sleepWakeDone(5000 + 60000, 5000, span));
  TEST_ASSERT_TRUE(sleepWakeDone(5000 + span, 5000, span));
}

void test_done_across_wrap(void) {
  uint32_t start = 0xFFFFF000UL;
  uint32_t span  = sleepWakeSpanMs(SLEEP_WAKE_S_MAX);
  TEST_ASSERT_FALSE(sleepWakeDone(start, start, span));
  TEST_ASSERT_FALSE(sleepWakeDone(start + span - 1, start, span));
  TEST_ASSERT_TRUE(sleepWakeDone(start + span, start, span));
}

// A pending image keeps the unit awake until the health gate can confirm it,
// so a 30 s awake window no longer ends in a bootloader rollback.
void test_pending_image_holds_sleep_until_the_health_gate(void) {
  const uint32_t gate = 300000;
  TEST_ASSERT_TRUE(sleepHeldForOta(true, 30000, gate));
  TEST_ASSERT_TRUE(sleepHeldForOta(true, gate, gate));
  TEST_ASSERT_TRUE(sleepHeldForOta(true, gate + SLEEP_OTA_GRACE_MS - 1, gate));
}

// The hold is bounded: an image that will not confirm (crash streak at the
// limit, unread streak) does not keep a battery unit awake.
void test_hold_ends_after_the_gate_and_grace(void) {
  const uint32_t gate = 300000;
  TEST_ASSERT_FALSE(sleepHeldForOta(true, gate + SLEEP_OTA_GRACE_MS, gate));
}

void test_confirmed_image_never_holds_sleep(void) {
  TEST_ASSERT_FALSE(sleepHeldForOta(false, 0, 300000));
  TEST_ASSERT_FALSE(sleepHeldForOta(false, 30000, 300000));
}

// An image that passes the health gate on its next confirm try.
struct FakeOta {
  static bool pending;
  static int tries;
  static void confirmIfHealthy() { tries++; pending = false; }
  static bool awaitingConfirm() { return pending; }
};
bool FakeOta::pending = true;
int FakeOta::tries = 0;

// CR #128: a long OTA check passed the gate after OTA's own confirm try, and
// sleep read the stale pending state past the grace and rolled the image back.
void test_sleep_confirms_before_reading_pending(void) {
  FakeOta::pending = true;
  FakeOta::tries = 0;
  bool pending = true;
  TEST_ASSERT_FALSE(sleepHeldForOtaNow<FakeOta>(400000, 300000, pending));
  TEST_ASSERT_FALSE(pending);
  TEST_ASSERT_EQUAL_INT(1, FakeOta::tries);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_pending_image_holds_sleep_until_the_health_gate);
  RUN_TEST(test_hold_ends_after_the_gate_and_grace);
  RUN_TEST(test_confirmed_image_never_holds_sleep);
  RUN_TEST(test_sleep_confirms_before_reading_pending);
  RUN_TEST(test_span_floor_and_plain);
  RUN_TEST(test_span_capped_no_overflow);
  RUN_TEST(test_done_plain);
  RUN_TEST(test_huge_wake_not_done_at_boot);
  RUN_TEST(test_done_across_wrap);
  return UNITY_END();
}
