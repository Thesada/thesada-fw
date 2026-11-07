// Host-native unit tests for cell_ntp_policy.h (cellular NTP retry schedule).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "cell_ntp_policy.h"

void setUp(void) {}
void tearDown(void) {}

// The first try waits past the bearer warm-up, then 60 s doubling.
void test_delay_schedule(void) {
  TEST_ASSERT_EQUAL_UINT32(30000, cellNtpDelayMs(0));
  TEST_ASSERT_EQUAL_UINT32(60000, cellNtpDelayMs(1));
  TEST_ASSERT_EQUAL_UINT32(120000, cellNtpDelayMs(2));
  TEST_ASSERT_EQUAL_UINT32(240000, cellNtpDelayMs(3));
  TEST_ASSERT_EQUAL_UINT32(960000, cellNtpDelayMs(5));
}

// The backoff stops at 30 min and never overflows, however many tries.
void test_delay_caps(void) {
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_MAX_MS, cellNtpDelayMs(6));
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_MAX_MS, cellNtpDelayMs(40));
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_MAX_MS, cellNtpDelayMs(255));
  for (int a = 1; a < 256; a++) {
    TEST_ASSERT_TRUE(cellNtpDelayMs((uint8_t)a) >= cellNtpDelayMs((uint8_t)(a - 1)));
  }
}

void test_due(void) {
  TEST_ASSERT_FALSE(cellNtpDue(1000, 2000));
  TEST_ASSERT_TRUE(cellNtpDue(2000, 2000));
  TEST_ASSERT_TRUE(cellNtpDue(2001, 2000));
}

// A deadline set just before millis() wraps is still due after the wrap.
void test_due_across_wrap(void) {
  uint32_t start = 0xFFFFF000UL;
  uint32_t at    = start + cellNtpDelayMs(0);
  TEST_ASSERT_TRUE(at < start);
  TEST_ASSERT_FALSE(cellNtpDue(start, at));
  TEST_ASSERT_TRUE(cellNtpDue(at, at));
  TEST_ASSERT_TRUE(cellNtpDue(at + 5, at));
}

// The bench unit's ntp.server is a LAN router: never tried over cellular.
void test_private_literals_unusable(void) {
  const char* bad[] = {"172.20.0.1", "172.31.255.255", "10.0.0.1", "192.168.1.1",
                       "127.0.0.1", "169.254.1.1", "0.0.0.0", "", "printer.local",
                       "NAS.LOCAL", "printer.local.", "172.20.0.1.", "192.168.1.1."};
  for (const char* h : bad) TEST_ASSERT_FALSE_MESSAGE(cellNtpHostUsable(h), h);
  TEST_ASSERT_FALSE(cellNtpHostUsable(nullptr));
}

void test_public_hosts_usable(void) {
  const char* good[] = {"pool.ntp.org", "time.cloudflare.com", "216.239.35.0",
                        "172.15.0.1", "172.32.0.1", "100.64.0.1", "8.8.8.8",
                        "10.example.com", "1.2.3", "1.2.3.4.5", "256.1.1.1",
                        "1..2.3", "local", "my.locality", "192.0.2.1", "169.1.1.1",
                        "pool.ntp.org.", "8.8.8.8."};
  for (const char* h : good) TEST_ASSERT_TRUE_MESSAGE(cellNtpHostUsable(h), h);
}

// The copy in syncClock and the +CNTP command both take CELL_NTP_HOST_MAX.
void test_host_length_cap(void) {
  char host[CELL_NTP_HOST_MAX + 3];
  for (size_t i = 0; i < CELL_NTP_HOST_MAX; i++) host[i] = 'a';
  host[CELL_NTP_HOST_MAX] = '\0';
  TEST_ASSERT_TRUE(cellNtpHostUsable(host));
  host[CELL_NTP_HOST_MAX]     = '.';
  host[CELL_NTP_HOST_MAX + 1] = '\0';
  TEST_ASSERT_TRUE(cellNtpHostUsable(host));
  host[CELL_NTP_HOST_MAX]     = 'a';
  TEST_ASSERT_FALSE(cellNtpHostUsable(host));

  const char* out[CELL_NTP_PER_TRY];
  uint8_t n = cellNtpCandidates(host, "time.cloudflare.com", out);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("time.cloudflare.com", out[0]);
  TEST_ASSERT_EQUAL_STRING(CELL_NTP_DEFAULT, out[1]);
}

void test_wait_clamps(void) {
  TEST_ASSERT_EQUAL_UINT32(1000, cellNtpWaitMs(0));
  TEST_ASSERT_EQUAL_UINT32(1000, cellNtpWaitMs(-5));
  TEST_ASSERT_EQUAL_UINT32(10000, cellNtpWaitMs(10));
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_WAIT_MAX_MS, cellNtpWaitMs(15));
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_WAIT_MAX_MS, cellNtpWaitMs(60));
  TEST_ASSERT_EQUAL_UINT32(CELL_NTP_WAIT_MAX_MS, cellNtpWaitMs(INT32_MAX));
}

void test_candidates_skip_private(void) {
  const char* out[CELL_NTP_PER_TRY];
  uint8_t n = cellNtpCandidates("172.20.0.1", "time.cloudflare.com", out);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("time.cloudflare.com", out[0]);
  TEST_ASSERT_EQUAL_STRING("pool.ntp.org", out[1]);
}

void test_candidates_order_and_dedup(void) {
  const char* out[CELL_NTP_PER_TRY];
  uint8_t n = cellNtpCandidates("time.google.com", "time.cloudflare.com", out);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("time.google.com", out[0]);
  TEST_ASSERT_EQUAL_STRING("time.cloudflare.com", out[1]);

  n = cellNtpCandidates("POOL.ntp.org", "pool.ntp.org", out);
  TEST_ASSERT_EQUAL_UINT8(1, n);
  TEST_ASSERT_EQUAL_STRING("POOL.ntp.org", out[0]);

  n = cellNtpCandidates("pool.ntp.org.", "pool.ntp.org", out);
  TEST_ASSERT_EQUAL_UINT8(1, n);

  // A prefix is a different host, in either order.
  n = cellNtpCandidates("time.google.com", "time.google.com.au", out);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("time.google.com.au", out[1]);
  n = cellNtpCandidates("time.google.com.au", "time.google.com", out);
  TEST_ASSERT_EQUAL_UINT8(2, n);
  TEST_ASSERT_EQUAL_STRING("time.google.com", out[1]);
}

// Nothing usable configured still leaves the public default.
void test_candidates_fallback(void) {
  const char* out[CELL_NTP_PER_TRY];
  uint8_t n = cellNtpCandidates("192.168.0.1", nullptr, out);
  TEST_ASSERT_EQUAL_UINT8(1, n);
  TEST_ASSERT_EQUAL_STRING(CELL_NTP_DEFAULT, out[0]);
  n = cellNtpCandidates(nullptr, "", out);
  TEST_ASSERT_EQUAL_UINT8(1, n);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_delay_schedule);
  RUN_TEST(test_delay_caps);
  RUN_TEST(test_due);
  RUN_TEST(test_due_across_wrap);
  RUN_TEST(test_private_literals_unusable);
  RUN_TEST(test_public_hosts_usable);
  RUN_TEST(test_host_length_cap);
  RUN_TEST(test_wait_clamps);
  RUN_TEST(test_candidates_skip_private);
  RUN_TEST(test_candidates_order_and_dedup);
  RUN_TEST(test_candidates_fallback);
  return UNITY_END();
}
