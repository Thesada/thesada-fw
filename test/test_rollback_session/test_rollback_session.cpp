// Host-native unit tests for mqttRollbackSessionDecide in mqtt_rollback_policy.h.
// Its own unit: the starved allocator kept the analyzer off the rollback unit.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdlib.h>
#include "mqtt_rollback_policy.h"
#include "mqtt_topic_policy.h"
#include <string>

void setUp(void) {}
void tearDown(void) {}

static const char* GOOD = "{\"broker\":\"mqtt.thesada.app\",\"port\":8884}";
static const char* BAD  = "{\"broker\":\"bad.invalid\",\"port\":8884}";
static const char* BAD2 = "{\"broker\":\"other.invalid\",\"port\":8884}";

struct FailingAllocator : ArduinoJson::Allocator {
  void* allocate(size_t) override { return nullptr; }
  void  deallocate(void* p) override { free(p); }
  void* reallocate(void*, size_t) override { return nullptr; }
};

// A snapshot that ran out of heap while it was parsed is not invalid: the
// fallback stays for the next attempt. Only a bad snapshot or an edit ends it.
void test_rollback_session_survives_a_no_heap_parse(void) {
  FailingAllocator none;
  JsonDocument starved(&none);
  TEST_ASSERT_EQUAL_INT(MQTT_LG_RETRY, mqttRollbackSessionDecide(true, BAD, BAD, GOOD, starved));
  JsonDocument lg;
  TEST_ASSERT_EQUAL_INT(MQTT_LG_DIAL, mqttRollbackSessionDecide(true, BAD, BAD, GOOD, lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD2, GOOD, lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(false, BAD, BAD, GOOD, lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD, "{\"broker\":", lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD, "{\"port\":8884}", lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD, "[]", lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD, nullptr, lg));
  TEST_ASSERT_EQUAL_INT(MQTT_LG_LIVE, mqttRollbackSessionDecide(true, BAD, BAD, "[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[[]", lg));
}

// The session endpoint: a set host replaces it. A fallback that ended on a live
// config with no broker clears it and dials nothing, so live creds never reach
// the last-good broker. A plain live session keeps what begin() set.
void test_session_endpoint_clears_when_fallback_ends_without_broker(void) {
  TEST_ASSERT_EQUAL_INT(MQTT_EP_SET, mqttSessionEndpoint(true, true, "lg.example"));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_SET, mqttSessionEndpoint(false, true, "live.example"));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_CLEAR, mqttSessionEndpoint(false, true, ""));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_CLEAR, mqttSessionEndpoint(false, true, nullptr));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_KEEP, mqttSessionEndpoint(false, false, ""));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_KEEP, mqttSessionEndpoint(false, false, "live.example"));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_KEEP, mqttSessionEndpoint(true, false, ""));
}


// A live broker past the host buffer dials nothing, never the old host with live creds.
void test_endpoint_overlong_host_clears(void) {
  std::string host(MQTT_BROKER_HOST_CAP, 'h');
  TEST_ASSERT_EQUAL_INT(MQTT_EP_CLEAR, mqttSessionEndpoint(false, true, host.c_str()));
  TEST_ASSERT_EQUAL_INT(MQTT_EP_CLEAR, mqttSessionEndpoint(true, true, host.c_str()));
  host.pop_back();
  TEST_ASSERT_EQUAL_INT(MQTT_EP_SET, mqttSessionEndpoint(false, true, host.c_str()));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_rollback_session_survives_a_no_heap_parse);
  RUN_TEST(test_session_endpoint_clears_when_fallback_ends_without_broker);
  RUN_TEST(test_endpoint_overlong_host_clears);
  return UNITY_END();
}
