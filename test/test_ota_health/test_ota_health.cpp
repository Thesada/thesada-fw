// Host-native unit tests for ota_health_policy.h.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "ota_health_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_a_confirmed_image_is_left_alone(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMark(false, true, OTA_HEALTH_MIN_MS));
  TEST_ASSERT_FALSE(otaHealthShouldMark(false, false, 0));
}

void test_mqtt_confirms_immediately(void) {
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, true, 0));
}

void test_uptime_confirms_without_mqtt(void) {
  TEST_ASSERT_FALSE(otaHealthShouldMark(true, false, OTA_HEALTH_MIN_MS - 1));
  TEST_ASSERT_TRUE(otaHealthShouldMark(true, false, OTA_HEALTH_MIN_MS));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_a_confirmed_image_is_left_alone);
  RUN_TEST(test_mqtt_confirms_immediately);
  RUN_TEST(test_uptime_confirms_without_mqtt);
  return UNITY_END();
}
