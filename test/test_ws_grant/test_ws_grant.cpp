// Host-native unit tests for ws_grant_policy.h (single-use WS grants).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "ws_grant_policy.h"

static WsGrant t[WS_GRANT_SLOTS];
static const char* TA = "00112233445566778899aabbccddeeff";
static const char* TB = "ffeeddccbbaa99887766554433221100";
static const char* IP = "192.0.2.10";

void setUp(void) { memset(t, 0, sizeof(t)); }
void tearDown(void) {}

void test_issue_then_consume_once(void) {
  TEST_ASSERT_EQUAL(0, wsGrantIssue(t, 1000, TA, IP));
  TEST_ASSERT_TRUE(wsGrantConsume(t, 1001, TA, IP));
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1002, TA, IP));
}

void test_wrong_token_or_ip_refused(void) {
  wsGrantIssue(t, 1000, TA, IP);
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1001, TB, IP));
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1001, TA, "192.0.2.11"));
  TEST_ASSERT_TRUE(wsGrantConsume(t, 1001, TA, IP));
}

void test_expired_refused(void) {
  wsGrantIssue(t, 1000, TA, IP);
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1000 + WS_GRANT_TTL_MS, TA, IP));
}

void test_two_clients_one_ip_both_work(void) {
  wsGrantIssue(t, 1000, TA, IP);
  wsGrantIssue(t, 1000, TB, IP);
  TEST_ASSERT_TRUE(wsGrantConsume(t, 1001, TB, IP));
  TEST_ASSERT_TRUE(wsGrantConsume(t, 1001, TA, IP));
}

void test_full_table_refuses_never_evicts(void) {
  char tok[WS_TOKEN_HEX + 1];
  for (uint8_t i = 0; i < WS_GRANT_SLOTS; i++) {
    uint8_t b[16] = {0};
    b[15] = i;
    wsTokenHex(b, 16, tok);
    TEST_ASSERT_EQUAL(i, wsGrantIssue(t, 1000, tok, IP));
  }
  TEST_ASSERT_EQUAL(-1, wsGrantIssue(t, 1001, TA, IP));
  TEST_ASSERT_TRUE(wsGrantConsume(t, 1002, tok, IP));
  TEST_ASSERT_GREATER_OR_EQUAL(0, wsGrantIssue(t, 1003, TA, IP));
}

void test_expired_slot_is_reused(void) {
  for (uint8_t i = 0; i < WS_GRANT_SLOTS; i++) wsGrantIssue(t, 1000, i % 2 ? TB : TA, IP);
  TEST_ASSERT_EQUAL(0, wsGrantIssue(t, 1000 + WS_GRANT_TTL_MS, TA, IP));
}

void test_bad_input_refused(void) {
  TEST_ASSERT_EQUAL(-1, wsGrantIssue(t, 1000, "short", IP));
  TEST_ASSERT_EQUAL(-1, wsGrantIssue(t, 1000, nullptr, IP));
  TEST_ASSERT_EQUAL(-1, wsGrantIssue(t, 1000, TA, nullptr));
  char longIp[WS_GRANT_IP_CAP + 4];
  memset(longIp, '1', sizeof(longIp) - 1);
  longIp[sizeof(longIp) - 1] = '\0';
  TEST_ASSERT_EQUAL(-1, wsGrantIssue(t, 1000, TA, longIp));
  wsGrantIssue(t, 1000, TA, IP);
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1001, "", IP));
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1001, nullptr, IP));
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1001, TA, nullptr));
}

void test_empty_table_matches_nothing(void) {
  TEST_ASSERT_FALSE(wsGrantConsume(t, 1, "00000000000000000000000000000000", ""));
}

void test_hex_encoding(void) {
  const uint8_t b[3] = {0x00, 0xa5, 0xff};
  char out[7];
  wsTokenHex(b, 3, out);
  TEST_ASSERT_EQUAL_STRING("00a5ff", out);
  TEST_ASSERT_TRUE(constTimeEq("abc", "abc", 3));
  TEST_ASSERT_FALSE(constTimeEq("abc", "abd", 3));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_issue_then_consume_once);
  RUN_TEST(test_wrong_token_or_ip_refused);
  RUN_TEST(test_expired_refused);
  RUN_TEST(test_two_clients_one_ip_both_work);
  RUN_TEST(test_full_table_refuses_never_evicts);
  RUN_TEST(test_expired_slot_is_reused);
  RUN_TEST(test_bad_input_refused);
  RUN_TEST(test_empty_table_matches_nothing);
  RUN_TEST(test_hex_encoding);
  return UNITY_END();
}
