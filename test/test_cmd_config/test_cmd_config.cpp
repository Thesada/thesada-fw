// Host-native unit tests for cmd_config_policy.h.
// SPDX-License-Identifier: GPL-3.0-only
#include <string.h>
#include <unity.h>
#include "cmd_config_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const char* OBJ = "{\"mqtt\":{\"broker\":\"mqtt.example.com\"}}";

static CmdConfigVerdict verdict(bool tls, const char* s) {
  return cmdConfigVerdict(tls, s, s ? strlen(s) : 0);
}

void test_verified_object_is_accepted(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_OK, verdict(true, OBJ));
  const char* padded = "\n  {\"wifi\":{\"ssid\":\"x\"},\"mqtt\":{\"port\":8883,\"broker\":\"b\"}}";
  TEST_ASSERT_EQUAL(CMD_CONFIG_OK, verdict(true, padded));
}

void test_unverified_session_is_refused(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_TLS, verdict(false, OBJ));
}

void test_empty_and_oversize_are_refused(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, cmdConfigVerdict(true, OBJ, 0));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, cmdConfigVerdict(true, nullptr, 2));
  char big[CMD_CONFIG_MAX + 1];
  memset(big, 'a', sizeof(big));
  big[0] = '{';
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, cmdConfigVerdict(true, big, CMD_CONFIG_MAX));
}

void test_non_object_is_refused(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "[1,2]"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "\"mqtt\""));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":\"b\"}} trailing"));
}

void test_missing_or_empty_broker_is_refused(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_BROKER, verdict(true, "{}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_BROKER, verdict(true, "\n  {\"mqtt\":{}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_BROKER, verdict(true, "{\"mqtt\":{\"broker\":\"\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_BROKER, verdict(true, "{\"broker\":\"mqtt.example.com\"}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":\"mqtt.example.com\"}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":1}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_BROKER, verdict(true, "{\"other\":{\"mqtt\":{\"broker\":\"b\"}}}"));
}

void test_malformed_containers_are_refused(void) {
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":\"b"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_OK, verdict(true, "{\"extra\":{},\"arr\":[],\"mqtt\":{\"broker\":\"b\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"extra\":{\"a\":1,},\"mqtt\":{\"broker\":\"b\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"extra\":{\"a\":1 !},\"mqtt\":{\"broker\":\"b\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"user\":xyz,\"broker\":\"b\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":\"b\" !}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":\"b\"} !}"));
}

void test_deep_nesting_is_refused(void) {
  char deep[256];
  size_t n = 0;
  const char* head = "{\"mqtt\":{\"broker\":\"b\"},\"deep\":";
  memcpy(deep + n, head, strlen(head));
  n += strlen(head);
  for (int i = 0; i < CMD_CONFIG_MAX_DEPTH; i++) deep[n++] = '{';
  deep[n++] = '}';
  for (int i = 0; i < CMD_CONFIG_MAX_DEPTH; i++) deep[n++] = '}';
  deep[n++] = '}';
  deep[n] = '\0';
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, cmdConfigVerdict(true, deep, n));
}

void test_other_json_shapes_still_parse(void) {
  const char* doc =
      "{\"flag\":true,\"off\":false,\"n\":null,\"count\":-2.5e1,"
      "\"list\":[\"a\",{\"k\":1}],\"mqtt\":{\"user\":\"u\",\"broker\":\"bro\\nker\"}}";
  TEST_ASSERT_EQUAL(CMD_CONFIG_OK, verdict(true, doc));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE, verdict(true, "{\"mqtt\":{\"broker\":\"b\"},}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE,
                    verdict(true, "{\"mqtt\":{\"broker\":\"a\"},\"mqtt\":{\"broker\":\"b\"}}"));
  TEST_ASSERT_EQUAL(CMD_CONFIG_REFUSE_SHAPE,
                    verdict(true, "{\"mqtt\":{\"broker\":\"a\",\"broker\":\"b\"}}"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_verified_object_is_accepted);
  RUN_TEST(test_unverified_session_is_refused);
  RUN_TEST(test_empty_and_oversize_are_refused);
  RUN_TEST(test_non_object_is_refused);
  RUN_TEST(test_missing_or_empty_broker_is_refused);
  RUN_TEST(test_other_json_shapes_still_parse);
  RUN_TEST(test_deep_nesting_is_refused);
  RUN_TEST(test_malformed_containers_are_refused);
  return UNITY_END();
}
