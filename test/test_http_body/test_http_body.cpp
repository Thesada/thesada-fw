// Host-native unit tests for http_body_policy.h (per-request POST bodies).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdlib.h>
#include <string.h>
#include "http_body_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const uint8_t* U(const char* s) { return (const uint8_t*)s; }

void test_whole_body_in_chunks(void) {
  HttpBody* b = httpBodyBegin(11, HTTP_BODY_CMD_MAX);
  TEST_ASSERT_NOT_NULL(b);
  TEST_ASSERT_FALSE(httpBodyComplete(b));
  TEST_ASSERT_TRUE(httpBodyAppend(b, U("{\"cmd\":"), 7, 0));
  TEST_ASSERT_FALSE(httpBodyComplete(b));
  TEST_ASSERT_TRUE(httpBodyAppend(b, U("\"v\"}"), 4, 7));
  TEST_ASSERT_TRUE(httpBodyComplete(b));
  TEST_ASSERT_EQUAL_STRING("{\"cmd\":\"v\"}", b->data);
  free(b);
}

// A declared total past the cap is never allocated, so a stalled
// Content-Length cannot pin heap.
void test_over_cap_and_zero_refused(void) {
  TEST_ASSERT_NULL(httpBodyBegin(HTTP_BODY_CFG_MAX + 1, HTTP_BODY_CFG_MAX));
  TEST_ASSERT_NULL(httpBodyBegin(0, HTTP_BODY_CFG_MAX));
  HttpBody* b = httpBodyBegin(HTTP_BODY_CFG_MAX, HTTP_BODY_CFG_MAX);
  TEST_ASSERT_NOT_NULL(b);
  free(b);
}

// An unauthenticated chunk 0 leaves no buffer: later chunks land nowhere.
void test_null_buffer_takes_nothing(void) {
  TEST_ASSERT_FALSE(httpBodyAppend(nullptr, U("x"), 1, 0));
  TEST_ASSERT_FALSE(httpBodyComplete(nullptr));
}

// Chunks past the declared total or out of order poison the buffer for good.
void test_overrun_and_gap_refused(void) {
  HttpBody* b = httpBodyBegin(4, 64);
  TEST_ASSERT_FALSE(httpBodyAppend(b, U("abcde"), 5, 0));
  TEST_ASSERT_FALSE(httpBodyAppend(b, U("abcd"), 4, 0));
  TEST_ASSERT_FALSE(httpBodyComplete(b));
  free(b);

  b = httpBodyBegin(4, 64);
  TEST_ASSERT_TRUE(httpBodyAppend(b, U("ab"), 2, 0));
  TEST_ASSERT_FALSE(httpBodyAppend(b, U("cd"), 2, 3));
  TEST_ASSERT_FALSE(httpBodyComplete(b));
  free(b);
}

// Each request owns its buffer: a second request's chunk 0 cannot reset or
// write into the first one.
void test_two_requests_do_not_share(void) {
  HttpBody* op  = httpBodyBegin(6, 64);
  HttpBody* atk = httpBodyBegin(6, 64);
  TEST_ASSERT_TRUE(httpBodyAppend(op, U("{\"a\""), 4, 0));
  TEST_ASSERT_TRUE(httpBodyAppend(atk, U("EVIL!!"), 6, 0));
  TEST_ASSERT_TRUE(httpBodyAppend(op, U(":1"), 2, 4));
  TEST_ASSERT_EQUAL_STRING("{\"a\":1", op->data);
  free(op);
  free(atk);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_whole_body_in_chunks);
  RUN_TEST(test_over_cap_and_zero_refused);
  RUN_TEST(test_null_buffer_takes_nothing);
  RUN_TEST(test_overrun_and_gap_refused);
  RUN_TEST(test_two_requests_do_not_share);
  return UNITY_END();
}
