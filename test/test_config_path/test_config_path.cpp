// Host-native unit tests for config_path_policy.h, the dotted-key set and
// delete shell config.set / config.del run inside Config::edit.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "config_path_policy.h"
#include <string.h>
#include <string>

void setUp(void) {}
void tearDown(void) {}

static char msg[200];

static std::string json(JsonDocument& doc) {
  std::string s;
  serializeJson(doc, s);
  return s;
}

void test_set_creates_missing_sections(void) {
  JsonDocument doc;
  deserializeJson(doc, "{}");
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "gnss.enabled", "true", msg, sizeof(msg)));
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "mqtt.port", "8883", msg, sizeof(msg)));
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "mqtt.broker", "\"b.example\"", msg, sizeof(msg)));
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "a.f", "1.5", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("{\"gnss\":{\"enabled\":true},\"mqtt\":{\"port\":8883,\"broker\":\"\\\"b.example\\\"\"},\"a\":{\"f\":1.5}}",
                           json(doc).c_str());
}

// The value is copied into the doc, never kept as a pointer to the caller's buffer.
void test_set_copies_the_value(void) {
  JsonDocument doc;
  deserializeJson(doc, "{}");
  char value[8] = "abc";
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "a.b", value, msg, sizeof(msg)));
  value[0] = 'X';
  TEST_ASSERT_EQUAL_STRING("abc", doc["a"]["b"] | "");
}

void test_set_takes_a_json_literal(void) {
  JsonDocument doc;
  deserializeJson(doc, "{}");
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "wifi.networks", " [{\"ssid\":\"x\"}] ", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("x", doc["wifi"]["networks"][0]["ssid"] | "");
  // Not JSON after all: kept as the string typed.
  TEST_ASSERT_TRUE(configPathApply(doc.as<JsonObject>(), "a.b", "{not json", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("{not json", doc["a"]["b"] | "");
}

void test_set_walks_and_extends_arrays(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"wifi\":{\"networks\":[{\"ssid\":\"a\"}]}}");
  JsonObject root = doc.as<JsonObject>();
  TEST_ASSERT_TRUE(configPathApply(root, "wifi.networks.0.ssid", "b", msg, sizeof(msg)));
  TEST_ASSERT_TRUE(configPathApply(root, "wifi.networks.1", "{\"ssid\":\"c\"}", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("b", doc["wifi"]["networks"][0]["ssid"] | "");
  TEST_ASSERT_EQUAL_STRING("c", doc["wifi"]["networks"][1]["ssid"] | "");
  TEST_ASSERT_FALSE(configPathApply(root, "wifi.networks.3", "x", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Array index out of range (would leave hole)", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "wifi.networks.5.ssid", "x", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Parent array index out of range", msg);
}

void test_set_refuses_what_it_cannot_address(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"a\":{\"s\":\"x\"}}");
  JsonObject root = doc.as<JsonObject>();
  TEST_ASSERT_FALSE(configPathApply(root, "top", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Cannot set top-level keys directly (use section.key format)", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "b.list.0", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Parent path 'b.list' is not an array. Run config.set b.list [] first.", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "c.list.0.x", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Parent path 'c.list' doesn't exist. Run config.set c.list [] first.", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "a.s.t.u", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Parent key not found", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "a.s.t", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Parent is not an object or array", msg);
  std::string longKey = "a." + std::string(CONFIG_KEY_MAX, 'k');
  TEST_ASSERT_FALSE(configPathApply(root, longKey.c_str(), "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Key too long (max 127 bytes)", msg);
}

// Decision 2: a key may address at most CONFIG_MAX_DEPTH levels, and a JSON
// literal only the levels the key leaves.
void test_config_path_refuses_depth_past_8(void) {
  JsonDocument doc;
  deserializeJson(doc, "{}");
  JsonObject root = doc.as<JsonObject>();
  TEST_ASSERT_TRUE(configPathApply(root, "a.b.c.d.e.f.g.h", "1", msg, sizeof(msg)));
  TEST_ASSERT_FALSE(configPathApply(root, "a.b.c.d.e.f.g.h.i", "1", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Key deeper than 8 levels", msg);
  TEST_ASSERT_FALSE(configPathApply(root, "a.b.c.d.e.f.g.x", "[1]", msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Value nested past 8 levels", msg);
  TEST_ASSERT_TRUE(configPathApply(root, "a.b.c.d.e.f.y", "[1]", msg, sizeof(msg)));
  TEST_ASSERT_FALSE(configPathApply(root, "a.b.c.d.e.f.z", "[[1]]", msg, sizeof(msg)));
}

void test_delete_removes_members_and_nulls_array_slots(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"a\":{\"x\":1,\"y\":2},\"l\":{\"v\":[1,2,3]},\"lora\":{}}");
  JsonObject root = doc.as<JsonObject>();
  TEST_ASSERT_TRUE(configPathApply(root, "a.x", nullptr, msg, sizeof(msg)));
  TEST_ASSERT_TRUE(configPathApply(root, "l.v.1", nullptr, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(configPathApply(root, "l.v.3", nullptr, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Array index out of range", msg);
  TEST_ASSERT_TRUE(configPathApply(root, "lora", nullptr, msg, sizeof(msg)));
  TEST_ASSERT_FALSE(configPathApply(root, "gone", nullptr, msg, sizeof(msg)));
  TEST_ASSERT_EQUAL_STRING("Key not found", msg);
  TEST_ASSERT_EQUAL_STRING("{\"a\":{\"y\":2},\"l\":{\"v\":[1,null,3]}}", json(doc).c_str());
}

void test_array_index_is_digits_only(void) {
  size_t idx = 99;
  TEST_ASSERT_TRUE(configArrayIndex("12", idx));
  TEST_ASSERT_EQUAL_UINT(12, idx);
  TEST_ASSERT_FALSE(configArrayIndex("1a", idx));
  TEST_ASSERT_FALSE(configArrayIndex("", idx));
  TEST_ASSERT_FALSE(configArrayIndex(nullptr, idx));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_set_creates_missing_sections);
  RUN_TEST(test_set_copies_the_value);
  RUN_TEST(test_set_takes_a_json_literal);
  RUN_TEST(test_set_walks_and_extends_arrays);
  RUN_TEST(test_set_refuses_what_it_cannot_address);
  RUN_TEST(test_config_path_refuses_depth_past_8);
  RUN_TEST(test_delete_removes_members_and_nulls_array_slots);
  RUN_TEST(test_array_index_is_digits_only);
  return UNITY_END();
}
