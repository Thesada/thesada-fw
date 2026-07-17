// Host-native unit tests for log_kv_policy.h (Log::kvf formatting core).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "log_kv_policy.h"

// Mirrors the LOG_LINE_LEN default in Log.h / thesada_config.h.
static const size_t CAP = 220;

void setUp(void) {}
void tearDown(void) {}

// Normal structured event formats verbatim and reports a fit.
void test_format_fits(void) {
  char buf[CAP];
  TEST_ASSERT_TRUE(logKvFormat(buf, sizeof(buf),
                               "temp.sensor_new bus=%u addr=%s", 1u, "28ff64"));
  TEST_ASSERT_EQUAL_STRING("temp.sensor_new bus=1 addr=28ff64", buf);
}

// A value expanding past CAP truncates: NUL at cap-1, prefix intact, fit=false.
void test_truncation_stays_nul_terminated(void) {
  char big[512];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';

  char buf[CAP + 1];
  buf[CAP] = '\x7f';  // canary just past the formatting window
  TEST_ASSERT_FALSE(logKvFormat(buf, CAP, "ota.event blob=%s", big));
  TEST_ASSERT_EQUAL_CHAR('\0', buf[CAP - 1]);
  TEST_ASSERT_EQUAL_UINT(CAP - 1, strlen(buf));
  TEST_ASSERT_EQUAL_MEMORY("ota.event blob=xxx", buf, 18);
  TEST_ASSERT_EQUAL_CHAR('\x7f', buf[CAP]);  // never writes past cap
}

// Exact boundary: cap-1 chars fit; one more char truncates.
void test_boundary_exact_fit(void) {
  char fill[CAP];
  memset(fill, 'a', sizeof(fill) - 1);
  fill[CAP - 1] = '\0';  // strlen = CAP-1: exactly fills buf

  char buf[CAP];
  TEST_ASSERT_TRUE(logKvFormat(buf, sizeof(buf), "%s", fill));
  TEST_ASSERT_EQUAL_UINT(CAP - 1, strlen(buf));

  char over[CAP + 1];
  memset(over, 'b', sizeof(over) - 1);
  over[CAP] = '\0';  // strlen = CAP: one past what fits
  TEST_ASSERT_FALSE(logKvFormat(buf, sizeof(buf), "%s", over));
  TEST_ASSERT_EQUAL_UINT(CAP - 1, strlen(buf));
}

// Degenerate buffers refuse instead of writing.
void test_degenerate_buffers(void) {
  char buf[8] = "keep";
  TEST_ASSERT_FALSE(logKvFormat(buf, 0, "x"));
  TEST_ASSERT_EQUAL_STRING("keep", buf);  // cap=0 must not touch the buffer
  TEST_ASSERT_FALSE(logKvFormat(nullptr, 8, "x"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_format_fits);
  RUN_TEST(test_truncation_stays_nul_terminated);
  RUN_TEST(test_boundary_exact_fit);
  RUN_TEST(test_degenerate_buffers);
  return UNITY_END();
}
