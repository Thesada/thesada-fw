// Host-native unit tests for ttl_policy.h (auth-state TTLs on 64-bit uptime).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ttl_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const uint64_t WRAP     = 1ull << 32;  // millis() rollover, ~49.7 d
static const uint64_t TOKEN_MS = 3600000;     // bearer token TTL
static const uint64_t WS_MS    = 30000;       // WS grant TTL

void test_active_before_expiry(void) {
  TEST_ASSERT_TRUE(ttlActive(1000, 1000 + WS_MS));
}

void test_inactive_at_and_after_expiry(void) {
  TEST_ASSERT_FALSE(ttlActive(1000 + WS_MS, 1000 + WS_MS));
  TEST_ASSERT_FALSE(ttlActive(1001 + WS_MS, 1000 + WS_MS));
}

// expiry == 0 is the empty/consumed slot sentinel, never active.
void test_zero_expiry_is_empty_slot(void) {
  TEST_ASSERT_FALSE(ttlActive(0, 0));
  TEST_ASSERT_FALSE(ttlActive(WRAP - 16, 0));
}

// On millis() an expired slot nobody overwrote read live again from 2^31 to
// 2^32 ms after expiry. It has to stay dead at every distance.
void test_expired_slot_stays_dead_past_2_31_ms(void) {
  const uint64_t token = 1000 + TOKEN_MS;
  TEST_ASSERT_FALSE(ttlActive(token + (WRAP / 2) + 5, token));
  TEST_ASSERT_FALSE(ttlActive(token + WRAP - 1, token));
  TEST_ASSERT_FALSE(ttlActive(token + WRAP + (WRAP / 2), token));

  const uint64_t grant = 5000 + WS_MS;
  TEST_ASSERT_FALSE(ttlActive(grant + (WRAP / 2) + 1, grant));
}

// A slot issued just before uptime passes 2^32 ms keeps its full TTL.
void test_active_across_the_millis_wrap(void) {
  const uint64_t expiry = (WRAP - 4000) + WS_MS;
  TEST_ASSERT_TRUE(ttlActive(WRAP - 4000, expiry));
  TEST_ASSERT_TRUE(ttlActive(WRAP + 20000, expiry));
  TEST_ASSERT_FALSE(ttlActive(expiry, expiry));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_active_before_expiry);
  RUN_TEST(test_inactive_at_and_after_expiry);
  RUN_TEST(test_zero_expiry_is_empty_slot);
  RUN_TEST(test_expired_slot_stays_dead_past_2_31_ms);
  RUN_TEST(test_active_across_the_millis_wrap);
  return UNITY_END();
}
