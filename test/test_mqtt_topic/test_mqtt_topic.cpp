// Host-native unit tests for mqtt_topic_policy.h (refuse, never clip, a topic).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include "mqtt_topic_policy.h"

void setUp(void) {}
void tearDown(void) {}

static const size_t kCap = 160;  // MQTT_TOPIC_CAP

// Reaches mqttTopicVfmt without the printf attribute, so a null format compiles clean.
static bool vfmt(char* out, size_t cap, const char* fmt, ...) {  // NOLINT(cert-dcl50-cpp): test shim
  va_list ap;
  va_start(ap, fmt);
  bool fit = mqttTopicVfmt(out, cap, fmt, ap);
  va_end(ap);
  return fit;
}

// A prefix at Config::TOPIC_PREFIX_CAP - 1 (127 chars).
static void longPrefix(char* out) {
  memset(out, 'p', 127);
  out[127] = '\0';
}

// thesada-fw#56: 127 + "/sensor/temperature/" (20) + a 13-char slug is 160
// chars, one past the buffer. snprintf clipped it to a different topic.
void test_long_prefix_plus_slug_is_refused_not_clipped(void) {
  char prefix[128];
  longPrefix(prefix);
  char topic[kCap];
  TEST_ASSERT_FALSE(mqttTopicFmt(topic, sizeof(topic), "%s/sensor/temperature/%s",
                                 prefix, "living_room_1"));
  TEST_ASSERT_EQUAL_STRING("", topic);
}

// One char shorter fills the buffer exactly and is kept whole.
void test_topic_that_fills_the_buffer_fits(void) {
  char prefix[128];
  longPrefix(prefix);
  char topic[kCap];
  TEST_ASSERT_TRUE(mqttTopicFmt(topic, sizeof(topic), "%s/sensor/temperature/%s",
                                prefix, "living_room_"));
  TEST_ASSERT_EQUAL_UINT(kCap - 1, (unsigned)strlen(topic));
  TEST_ASSERT_EQUAL_STRING("living_room_", topic + 127 + 20);
}

// A refused topic must not leave an earlier topic behind in a reused buffer.
void test_refusal_clears_a_reused_buffer(void) {
  char topic[32];
  TEST_ASSERT_TRUE(mqttTopicFmt(topic, sizeof(topic), "%s/status", "a/b"));
  TEST_ASSERT_EQUAL_STRING("a/b/status", topic);
  TEST_ASSERT_FALSE(mqttTopicFmt(topic, sizeof(topic), "%s/status",
                                 "a-prefix-far-longer-than-the-buffer"));
  TEST_ASSERT_EQUAL_STRING("", topic);
}

// An empty topic is a protocol error, so it is refused like a clipped one.
void test_empty_topic_refused(void) {
  char topic[16] = "stale";
  TEST_ASSERT_FALSE(mqttTopicFmt(topic, sizeof(topic), "%s", ""));
  TEST_ASSERT_EQUAL_STRING("", topic);
}

void test_null_arguments(void) {
  char topic[16] = "stale";
  TEST_ASSERT_FALSE(mqttTopicFmt(nullptr, sizeof(topic), "%s", "a"));
  TEST_ASSERT_FALSE(mqttTopicFmt(topic, 0, "%s", "a"));
  TEST_ASSERT_EQUAL_STRING("stale", topic);
  TEST_ASSERT_FALSE(vfmt(topic, sizeof(topic), nullptr));
  TEST_ASSERT_EQUAL_STRING("", topic);
}

void test_topic_fits(void) {
  char t[kCap + 8];
  memset(t, 't', sizeof(t));
  t[kCap - 1] = '\0';
  TEST_ASSERT_TRUE(mqttTopicFits(t, kCap));
  t[kCap - 1] = 't';
  t[kCap] = '\0';
  TEST_ASSERT_FALSE(mqttTopicFits(t, kCap));
  TEST_ASSERT_FALSE(mqttTopicFits("", kCap));
  TEST_ASSERT_FALSE(mqttTopicFits(nullptr, kCap));
}

// thesada-fw#78: a 96-byte broker host was strncpy-clipped to 95 and dialed.
// It is refused, and the buffer keeps the previous broker for the reinit.
void test_long_broker_host_is_refused_and_previous_kept(void) {
  char live[MQTT_BROKER_HOST_CAP];
  TEST_ASSERT_TRUE(mqttHostCopy(live, sizeof(live), "mqtt.example.net"));
  char host[MQTT_BROKER_HOST_CAP + 1];
  memset(host, 'h', MQTT_BROKER_HOST_CAP);
  host[MQTT_BROKER_HOST_CAP] = '\0';
  TEST_ASSERT_FALSE(mqttHostCopy(live, sizeof(live), host));
  TEST_ASSERT_EQUAL_STRING("mqtt.example.net", live);
}

// 95 chars plus the NUL fill the buffer and are copied whole.
void test_broker_host_that_fills_the_buffer_fits(void) {
  char live[MQTT_BROKER_HOST_CAP] = "old";
  char host[MQTT_BROKER_HOST_CAP];
  memset(host, 'h', MQTT_BROKER_HOST_CAP - 1);
  host[MQTT_BROKER_HOST_CAP - 1] = '\0';
  TEST_ASSERT_TRUE(mqttHostFits(host, sizeof(live)));
  TEST_ASSERT_TRUE(mqttHostCopy(live, sizeof(live), host));
  TEST_ASSERT_EQUAL_STRING(host, live);
}

void test_broker_host_null_arguments(void) {
  char live[8] = "old";
  TEST_ASSERT_FALSE(mqttHostFits(nullptr, sizeof(live)));
  TEST_ASSERT_FALSE(mqttHostCopy(live, sizeof(live), nullptr));
  TEST_ASSERT_FALSE(mqttHostCopy(nullptr, sizeof(live), "a"));
  TEST_ASSERT_FALSE(mqttHostCopy(live, 0, "a"));
  TEST_ASSERT_EQUAL_STRING("old", live);
}

// 26.11.1 built "<id>_<slug>_<suffix>" in uid[48]. With a 20-char device id
// that clipped a temp slug past 21 chars, power past 20 and current past 18.
void test_ha_legacy_uid_matches_the_clipped_release_uid(void) {
  const char* id = "thesada-0123456789ab";
  struct { const char* suffix; size_t longest; } cases[] = {
      {"temp", 21}, {"power", 20}, {"current", 18}};
  for (const auto& c : cases) {
    std::string slug(c.longest, 's');
    char uid[96];
    char legacy[HA_LEGACY_UID_CAP];
    (void)snprintf(uid, sizeof(uid), "%s_%s_%s", id, slug.c_str(), c.suffix);
    TEST_ASSERT_FALSE(haLegacyUid(legacy, sizeof(legacy), uid));
    TEST_ASSERT_EQUAL_STRING("", legacy);
    slug += 's';
    (void)snprintf(uid, sizeof(uid), "%s_%s_%s", id, slug.c_str(), c.suffix);
    char released[48];
    (void)snprintf(released, sizeof(released), "%s_%s_%s", id, slug.c_str(), c.suffix);
    TEST_ASSERT_TRUE(haLegacyUid(legacy, sizeof(legacy), uid));
    TEST_ASSERT_EQUAL_STRING(released, legacy);
  }
}

void test_ha_legacy_uid_null_arguments(void) {
  char legacy[8] = "old";
  TEST_ASSERT_FALSE(haLegacyUid(legacy, sizeof(legacy), nullptr));
  TEST_ASSERT_EQUAL_STRING("", legacy);
  TEST_ASSERT_FALSE(haLegacyUid(nullptr, sizeof(legacy), "abcdefghij"));
  TEST_ASSERT_FALSE(haLegacyUid(legacy, 0, "abcdefghij"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_long_prefix_plus_slug_is_refused_not_clipped);
  RUN_TEST(test_topic_that_fills_the_buffer_fits);
  RUN_TEST(test_refusal_clears_a_reused_buffer);
  RUN_TEST(test_empty_topic_refused);
  RUN_TEST(test_null_arguments);
  RUN_TEST(test_topic_fits);
  RUN_TEST(test_long_broker_host_is_refused_and_previous_kept);
  RUN_TEST(test_broker_host_that_fills_the_buffer_fits);
  RUN_TEST(test_broker_host_null_arguments);
  RUN_TEST(test_ha_legacy_uid_matches_the_clipped_release_uid);
  RUN_TEST(test_ha_legacy_uid_null_arguments);
  return UNITY_END();
}
