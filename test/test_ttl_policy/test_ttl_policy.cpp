// Host-native unit tests for ttl_policy.h (rollover-safe millis() TTLs, F4).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ttl_policy.h"

void setUp(void) {}
void tearDown(void) {}

// --- ttlActive ---------------------------------------------------------------

void test_active_before_expiry(void) {
  TEST_ASSERT_TRUE(ttlActive(1000u, 31000u));
}

void test_inactive_at_and_after_expiry(void) {
  TEST_ASSERT_FALSE(ttlActive(31000u, 31000u));
  TEST_ASSERT_FALSE(ttlActive(31001u, 31000u));
}

// expiry == 0 is the empty/consumed slot sentinel, never active.
void test_zero_expiry_is_empty_slot(void) {
  TEST_ASSERT_FALSE(ttlActive(0u, 0u));
  TEST_ASSERT_FALSE(ttlActive(0xFFFFFFF0u, 0u));
}

// The F4 case: expiry computed just after the wrap (now + ttl overflowed),
// now still just below the wrap. Plain `now < expiry` says expired; the
// subtraction form keeps the window open.
void test_active_across_rollover(void) {
  uint32_t now    = 0xFFFFF000u;           // ~4 s before the wrap
  uint32_t expiry = now + 30000u;          // wraps to 0x00006C18
  TEST_ASSERT_TRUE(expiry < now);          // proves the wrap happened
  TEST_ASSERT_TRUE(ttlActive(now, expiry));
  TEST_ASSERT_TRUE(ttlActive(now + 20000u, expiry));   // now also wrapped
  TEST_ASSERT_FALSE(ttlActive(expiry + 1u, expiry));   // past it -> expired
}

// Inverse F4 case: stale expiry from before the wrap must not spring back
// to life after now wraps (plain compare would call it active again).
void test_stale_entry_stays_expired_after_rollover(void) {
  uint32_t expiry = 0xFFFFFFF0u;           // expired long ago, pre-wrap
  uint32_t now    = 5000u;                 // now has wrapped past it
  TEST_ASSERT_FALSE(ttlActive(now, expiry));
}

// --- ttlReached --------------------------------------------------------------

void test_reached_at_and_after(void) {
  TEST_ASSERT_FALSE(ttlReached(999u, 1000u));
  TEST_ASSERT_TRUE(ttlReached(1000u, 1000u));
  TEST_ASSERT_TRUE(ttlReached(1001u, 1000u));
}

// Lockout release across the wrap: until wrapped, now not yet.
void test_reached_across_rollover(void) {
  uint32_t until = 0xFFFFFF00u + 30000u;   // wraps
  TEST_ASSERT_FALSE(ttlReached(0xFFFFFF80u, until));  // still locked out
  TEST_ASSERT_TRUE(ttlReached(until + 1u, until));    // released after
}

// --- ttlRemaining ------------------------------------------------------------

void test_remaining_signs(void) {
  TEST_ASSERT_EQUAL_INT32(500, ttlRemaining(1000u, 1500u));
  TEST_ASSERT_EQUAL_INT32(-500, ttlRemaining(1500u, 1000u));
}

// Eviction ordering must hold when one expiry wrapped and the other did not:
// raw uint compares would sort the wrapped (numerically small) one as oldest
// even though it expires later.
void test_remaining_orders_correctly_across_rollover(void) {
  uint32_t now = 0xFFFFFB00u;
  uint32_t soon = now + 10000u;            // wraps, expires in 10 s
  uint32_t later = now + 3600000u;         // wraps, expires in 1 h
  TEST_ASSERT_TRUE(ttlRemaining(now, soon) < ttlRemaining(now, later));
  TEST_ASSERT_TRUE(ttlRemaining(now, soon) > 0);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_active_before_expiry);
  RUN_TEST(test_inactive_at_and_after_expiry);
  RUN_TEST(test_zero_expiry_is_empty_slot);
  RUN_TEST(test_active_across_rollover);
  RUN_TEST(test_stale_entry_stays_expired_after_rollover);
  RUN_TEST(test_reached_at_and_after);
  RUN_TEST(test_reached_across_rollover);
  RUN_TEST(test_remaining_signs);
  RUN_TEST(test_remaining_orders_correctly_across_rollover);
  return UNITY_END();
}
