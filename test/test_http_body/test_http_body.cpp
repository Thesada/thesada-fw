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

// --- one file cap, both directions (#66) --------------------------------------

// The dashboard editor loads up to HTTP_FILE_MAX, so what it loaded fits a POST back.
void test_file_body_cap_matches_the_read_cap(void) {
  TEST_ASSERT_EQUAL_UINT32(HTTP_FILE_MAX, HTTP_BODY_FILE_MAX);
  TEST_ASSERT_EQUAL_UINT32(65536, HTTP_FILE_MAX);
  HttpBody* b = httpBodyBegin(HTTP_FILE_MAX, HTTP_BODY_FILE_MAX);
  TEST_ASSERT_NOT_NULL(b);
  free(b);
  TEST_ASSERT_NULL(httpBodyBegin(HTTP_FILE_MAX + 1, HTTP_BODY_FILE_MAX));
}

// --- httpBodyVerdict (#67) ----------------------------------------------------

// A chunked request has Content-Length 0 and nothing buffered. Read as an
// empty body it truncated the target, so it must be refused first.
void test_chunked_is_length_required_not_empty(void) {
  TEST_ASSERT_EQUAL(HTTP_BODY_LENGTH_REQUIRED,
                    httpBodyVerdict(true, true, 0, HTTP_BODY_FILE_MAX, nullptr));
  TEST_ASSERT_EQUAL(HTTP_BODY_LENGTH_REQUIRED,
                    httpBodyVerdict(true, true, 10, HTTP_BODY_FILE_MAX, nullptr));
}

void test_unauthorized_wins_over_everything(void) {
  TEST_ASSERT_EQUAL(HTTP_BODY_UNAUTHORIZED,
                    httpBodyVerdict(false, true, 0, HTTP_BODY_FILE_MAX, nullptr));
  TEST_ASSERT_EQUAL(HTTP_BODY_UNAUTHORIZED,
                    httpBodyVerdict(false, false, 99999, HTTP_BODY_FILE_MAX, nullptr));
}

void test_plain_bodies_keep_their_verdicts(void) {
  TEST_ASSERT_EQUAL(HTTP_BODY_EMPTY, httpBodyVerdict(true, false, 0, HTTP_BODY_FILE_MAX, nullptr));
  TEST_ASSERT_EQUAL(HTTP_BODY_TOO_LARGE,
                    httpBodyVerdict(true, false, HTTP_BODY_CFG_MAX + 1, HTTP_BODY_CFG_MAX, nullptr));
  TEST_ASSERT_EQUAL(HTTP_BODY_INCOMPLETE, httpBodyVerdict(true, false, 4, HTTP_BODY_CMD_MAX, nullptr));
  HttpBody* b = httpBodyBegin(4, HTTP_BODY_CMD_MAX);
  TEST_ASSERT_TRUE(httpBodyAppend(b, U("ab"), 2, 0));
  TEST_ASSERT_EQUAL(HTTP_BODY_INCOMPLETE, httpBodyVerdict(true, false, 4, HTTP_BODY_CMD_MAX, b));
  TEST_ASSERT_TRUE(httpBodyAppend(b, U("cd"), 2, 2));
  TEST_ASSERT_EQUAL(HTTP_BODY_OK, httpBodyVerdict(true, false, 4, HTTP_BODY_CMD_MAX, b));
  free(b);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_whole_body_in_chunks);
  RUN_TEST(test_over_cap_and_zero_refused);
  RUN_TEST(test_null_buffer_takes_nothing);
  RUN_TEST(test_overrun_and_gap_refused);
  RUN_TEST(test_two_requests_do_not_share);
  RUN_TEST(test_file_body_cap_matches_the_read_cap);
  RUN_TEST(test_chunked_is_length_required_not_empty);
  RUN_TEST(test_unauthorized_wins_over_everything);
  RUN_TEST(test_plain_bodies_keep_their_verdicts);
  return UNITY_END();
}
