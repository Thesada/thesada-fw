// Host-native unit tests for login_rate_policy.h (HTTP login lockout table).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "login_rate_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const uint64_t WRAP = 1ull << 32;  // millis() rollover, ~49.7 d

template <size_t N>
static void failN(LoginRateTable<N>& t, const char* ip, int n, uint64_t now) {
  for (int i = 0; i < n; i++) t.fail(ip, now);
}

void test_unknown_ip_allowed(void) {
  LoginRateTable<> t;
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", 0));
}

void test_fifth_fail_locks_for_lockout(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 4, 1000);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", 1000));
  t.fail("192.0.2.1", 1000);
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", 1000));
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", 1000 + LOGIN_RATE_LOCKOUT_MS - 1));
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", 1000 + LOGIN_RATE_LOCKOUT_MS));
  TEST_ASSERT_TRUE(t.allow("192.0.2.2", 1000));  // per ip
}

// After a lockout one wrong guess re-locks: 1 guess per 30 s, not 5.
void test_wrong_guess_after_lockout_relocks(void) {
  LoginRateTable<> t;
  const uint64_t end = LOGIN_RATE_LOCKOUT_MS;
  failN(t, "192.0.2.1", 5, 0);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", end));
  t.fail("192.0.2.1", end);
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", end));
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", end + LOGIN_RATE_LOCKOUT_MS - 1));
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", end + LOGIN_RATE_LOCKOUT_MS));
}

// Same when nothing called allow() between the lockout end and the guess.
void test_unobserved_release_still_relocks(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 5, 0);
  t.fail("192.0.2.1", LOGIN_RATE_LOCKOUT_MS + 1);
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", LOGIN_RATE_LOCKOUT_MS + 1));
}

// LOGIN_RATE_QUIET_MS with no failure forgets the ip, lockout or not.
void test_quiet_period_clears(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 5, 0);
  failN(t, "192.0.2.1", 4, LOGIN_RATE_QUIET_MS);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", LOGIN_RATE_QUIET_MS));

  failN(t, "192.0.2.2", 4, 1000);
  failN(t, "192.0.2.2", 1, 1000 + LOGIN_RATE_QUIET_MS - 1);  // not quiet yet
  TEST_ASSERT_FALSE(t.allow("192.0.2.2", 1000 + LOGIN_RATE_QUIET_MS - 1));
}

// A successful login after a lockout forgets the ip at once.
void test_success_after_lockout_clears(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 5, 0);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", LOGIN_RATE_LOCKOUT_MS));
  t.reset("192.0.2.1");
  failN(t, "192.0.2.1", 4, LOGIN_RATE_LOCKOUT_MS);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", LOGIN_RATE_LOCKOUT_MS));
}

// A guess while locked neither extends the lockout nor overflows the count.
void test_fail_while_locked_is_ignored(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 5, 0);
  failN(t, "192.0.2.1", 300, 10000);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", LOGIN_RATE_LOCKOUT_MS));
}

void test_reset_forgets_ip(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 4, 0);
  t.reset("192.0.2.1");
  failN(t, "192.0.2.1", 4, 0);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", 0));
  t.reset("192.0.2.9");  // unknown ip: no-op
  t.fail("192.0.2.1", 0);
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", 0));
}

// On millis() a never-locked entry read 429 for 24.8 d once uptime passed
// 2^31 ms, and a released one 2^31 ms after release.
void test_no_lockout_appears_past_the_millis_wrap(void) {
  LoginRateTable<> t;
  t.fail("192.0.2.1", 1000);
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", (WRAP / 2) + 1000));
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", WRAP - 1));
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", WRAP + (WRAP / 2) + 1000));

  failN(t, "192.0.2.2", 5, 0);
  TEST_ASSERT_TRUE(t.allow("192.0.2.2", LOGIN_RATE_LOCKOUT_MS + (WRAP / 2) + 1));
}

// A lockout that straddles the 32-bit wrap still holds for its full 30 s.
void test_lockout_across_the_millis_wrap(void) {
  LoginRateTable<> t;
  failN(t, "192.0.2.1", 5, WRAP - 1000);
  TEST_ASSERT_FALSE(t.allow("192.0.2.1", WRAP + 1000));
  TEST_ASSERT_TRUE(t.allow("192.0.2.1", WRAP - 1000 + LOGIN_RATE_LOCKOUT_MS));
}

// 16 IPs with one typo each must not leave a 17th IP unlimited.
void test_full_table_still_limits_a_new_ip(void) {
  LoginRateTable<> t;
  char ip[16];
  for (int i = 0; i < 16; i++) {
    (void)snprintf(ip, sizeof(ip), "198.51.100.%d", i);
    t.fail(ip, 0);
  }
  failN(t, "203.0.113.1", 5, 0);
  TEST_ASSERT_FALSE(t.allow("203.0.113.1", 0));
}

// Full table: a new ip counts on the overflow entry, every listed ip keeps its count.
void test_full_table_keeps_every_count(void) {
  LoginRateTable<2> t;
  failN(t, "A", 4, 0);
  failN(t, "B", 1, 0);
  t.fail("C", 0);                       // overflow, B keeps its one
  t.fail("A", 0);
  TEST_ASSERT_FALSE(t.allow("A", 0));
  failN(t, "B", 4, 0);
  TEST_ASSERT_FALSE(t.allow("B", 0));
}

// 16 locked ips and a 17th: the 17th must not unlock one of them (#68).
void test_full_table_never_evicts_a_locked_ip(void) {
  LoginRateTable<> t;
  char ip[16];
  for (int i = 0; i < 16; i++) {
    (void)snprintf(ip, sizeof(ip), "198.51.100.%d", i);
    failN(t, ip, 5, 0);
  }
  failN(t, "203.0.113.1", 5, 0);
  for (int i = 0; i < 16; i++) {
    (void)snprintf(ip, sizeof(ip), "198.51.100.%d", i);
    TEST_ASSERT_FALSE(t.allow(ip, 0));
  }
  TEST_ASSERT_FALSE(t.allow("203.0.113.1", 0));
}

// Two spare ips alternating over a full table both hit the limit: before the
// overflow entry they evicted each other and never locked.
void test_spare_ips_cannot_thrash_a_slot(void) {
  LoginRateTable<2> t;
  failN(t, "A", 2, 0);
  for (int i = 0; i < 5; i++) {
    t.fail("B", 0);
    t.fail("C", 0);
  }
  TEST_ASSERT_FALSE(t.allow("B", 0));
  TEST_ASSERT_FALSE(t.allow("C", 0));
}

// A locked overflow refuses every ip without a slot, a listed ip keeps its own
// count, and the overflow ages like any entry.
void test_locked_overflow_refuses_unlisted_ips(void) {
  LoginRateTable<2> t;
  const uint64_t end = LOGIN_RATE_LOCKOUT_MS;
  failN(t, "A", 1, 0);
  failN(t, "B", 1, 0);
  failN(t, "C", 5, 0);
  TEST_ASSERT_FALSE(t.allow("D", 0));
  TEST_ASSERT_TRUE(t.allow("A", 0));
  TEST_ASSERT_FALSE(t.allow("D", end - 1));
  TEST_ASSERT_TRUE(t.allow("D", end));
  t.fail("D", end);                     // one guess re-locks the shared entry
  TEST_ASSERT_FALSE(t.allow("C", end));
}

// A success never clears the overflow, but frees its own slot for a new ip.
void test_success_frees_a_slot_not_the_overflow(void) {
  LoginRateTable<2> t;
  failN(t, "A", 1, 0);
  failN(t, "B", 1, 0);
  failN(t, "C", 5, 0);
  t.reset("C");                         // unlisted: no-op
  TEST_ASSERT_FALSE(t.allow("D", 0));
  t.reset("A");
  TEST_ASSERT_TRUE(t.allow("D", 0));
  failN(t, "D", 4, 0);                  // own slot: counts from zero
  TEST_ASSERT_TRUE(t.allow("D", 0));
}

// The overflow is forgotten after the quiet period like any ip.
void test_overflow_quiet_period_clears(void) {
  LoginRateTable<1> t;
  const uint64_t q = LOGIN_RATE_QUIET_MS;
  t.fail("A", 0);
  failN(t, "B", 5, 0);                  // overflow, locked
  t.fail("A", q - 1);                   // A stays live, the table stays full
  failN(t, "B", 4, q);                  // overflow forgotten: 4, not 5
  TEST_ASSERT_TRUE(t.allow("B", q));
  t.fail("B", q);
  TEST_ASSERT_FALSE(t.allow("B", q));
}

// A quiet entry is a free slot, taken before the overflow entry.
void test_quiet_slot_reused_before_eviction(void) {
  LoginRateTable<2> t;
  const uint64_t q = LOGIN_RATE_QUIET_MS;
  failN(t, "A", 4, 0);
  t.fail("B", q - 1);                   // table full; A not quiet yet
  t.fail("C", q);                       // A is quiet now: C takes its slot, B is kept
  failN(t, "B", 4, q);                  // 1 + 4 = 5 only if B was kept
  TEST_ASSERT_FALSE(t.allow("B", q));
}

// A released lockout is still live: a new ip does not take its slot.
void test_released_lockout_is_not_a_free_slot(void) {
  LoginRateTable<2> t;
  failN(t, "A", 5, 0);
  failN(t, "B", 3, 0);
  t.fail("C", LOGIN_RATE_LOCKOUT_MS);   // overflow
  t.fail("A", LOGIN_RATE_LOCKOUT_MS);
  TEST_ASSERT_FALSE(t.allow("A", LOGIN_RATE_LOCKOUT_MS));
  failN(t, "B", 2, LOGIN_RATE_LOCKOUT_MS);
  TEST_ASSERT_FALSE(t.allow("B", LOGIN_RATE_LOCKOUT_MS));
}

void test_empty_or_null_ip_is_ignored(void) {
  LoginRateTable<1> t;
  TEST_ASSERT_TRUE(t.allow(nullptr, 0));
  TEST_ASSERT_TRUE(t.allow("", 0));
  failN(t, "A", 4, 0);
  t.fail("", 0);                        // must not evict A
  t.fail(nullptr, 0);
  t.reset(nullptr);
  t.fail("A", 0);
  TEST_ASSERT_FALSE(t.allow("A", 0));
}

// An ip longer than the slot still finds its own (truncated) entry.
void test_overlong_ip_finds_its_slot(void) {
  LoginRateTable<> t;
  char ip[LOGIN_RATE_IP_CAP + 20];
  memset(ip, 'f', sizeof(ip) - 1);
  ip[sizeof(ip) - 1] = '\0';
  failN(t, ip, 5, 0);
  TEST_ASSERT_FALSE(t.allow(ip, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_unknown_ip_allowed);
  RUN_TEST(test_fifth_fail_locks_for_lockout);
  RUN_TEST(test_wrong_guess_after_lockout_relocks);
  RUN_TEST(test_unobserved_release_still_relocks);
  RUN_TEST(test_quiet_period_clears);
  RUN_TEST(test_success_after_lockout_clears);
  RUN_TEST(test_fail_while_locked_is_ignored);
  RUN_TEST(test_reset_forgets_ip);
  RUN_TEST(test_no_lockout_appears_past_the_millis_wrap);
  RUN_TEST(test_lockout_across_the_millis_wrap);
  RUN_TEST(test_full_table_still_limits_a_new_ip);
  RUN_TEST(test_full_table_keeps_every_count);
  RUN_TEST(test_full_table_never_evicts_a_locked_ip);
  RUN_TEST(test_spare_ips_cannot_thrash_a_slot);
  RUN_TEST(test_locked_overflow_refuses_unlisted_ips);
  RUN_TEST(test_success_frees_a_slot_not_the_overflow);
  RUN_TEST(test_overflow_quiet_period_clears);
  RUN_TEST(test_quiet_slot_reused_before_eviction);
  RUN_TEST(test_released_lockout_is_not_a_free_slot);
  RUN_TEST(test_empty_or_null_ip_is_ignored);
  RUN_TEST(test_overlong_ip_finds_its_slot);
  return UNITY_END();
}
