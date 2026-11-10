#include <unity.h>
#include <string>
#include "config_schema_policy.h"

static int steps;
static size_t lastChanged;
static void onStep(int, const char*, size_t changed) { steps++; lastChanged = changed; }

void setUp() { steps = 0; lastChanged = 0; }
void tearDown() {}

static std::string out(JsonDocument& d) { std::string s; serializeJson(d, s); return s; }

void test_absent_schema_is_zero() {
  JsonDocument d; deserializeJson(d, "{\"wifi\":{}}");
  TEST_ASSERT_EQUAL(0, configSchemaVersion(d.as<JsonVariantConst>()));
}

void test_bad_schema_is_invalid_and_untouched() {
  const char* in[] = {"{\"schema\":\"1\"}", "{\"schema\":-1}", "{\"schema\":1.5}"};
  for (const char* j : in) {
    JsonDocument d; deserializeJson(d, j);
    TEST_ASSERT_EQUAL(CFG_SCHEMA_INVALID, configMigrate(d, onStep));
    std::string s = out(d);
    JsonDocument ref; deserializeJson(ref, j);
    TEST_ASSERT_EQUAL_STRING(out(ref).c_str(), s.c_str());
  }
  TEST_ASSERT_EQUAL(0, steps);
}

void test_newer_schema_left_alone() {
  JsonDocument d; deserializeJson(d, "{\"schema\":99,\"mqtt\":{\"user:\":\"x\"}}");
  TEST_ASSERT_EQUAL(CFG_SCHEMA_NEWER, configMigrate(d, onStep));
  TEST_ASSERT_EQUAL_STRING("x", d["mqtt"]["user:"] | "");
  TEST_ASSERT_EQUAL(0, steps);
}

void test_current_schema_untouched() {
  JsonDocument d; deserializeJson(d, "{\"schema\":1,\"mqtt\":{\"user:\":\"x\"}}");
  TEST_ASSERT_EQUAL(CFG_SCHEMA_CURRENT, configMigrate(d, onStep));
  TEST_ASSERT_EQUAL(0, steps);
}

void test_colon_duplicate_dropped_plain_kept() {
  JsonDocument d;
  deserializeJson(d, "{\"mqtt\":{\"password\":\"good\",\"password:\":\"stale\",\"user\":\"u\",\"user:\":\"v\"}}");
  TEST_ASSERT_EQUAL(CFG_SCHEMA_MIGRATED, configMigrate(d, onStep));
  TEST_ASSERT_EQUAL_STRING("good", d["mqtt"]["password"] | "");
  TEST_ASSERT_EQUAL_STRING("u", d["mqtt"]["user"] | "");
  TEST_ASSERT_TRUE(d["mqtt"]["password:"].isNull());
  TEST_ASSERT_TRUE(d["mqtt"]["user:"].isNull());
  TEST_ASSERT_EQUAL(1, d["schema"].as<int>());
  TEST_ASSERT_EQUAL(1, steps);
  TEST_ASSERT_EQUAL(2, lastChanged);
}

void test_colon_only_key_renamed() {
  JsonDocument d; deserializeJson(d, "{\"mqtt\":{\"user:\":\"v\"}}");
  configMigrate(d, onStep);
  TEST_ASSERT_EQUAL_STRING("v", d["mqtt"]["user"] | "");
  TEST_ASSERT_TRUE(d["mqtt"]["user:"].isNull());
}

void test_nested_objects_and_arrays() {
  JsonDocument d;
  deserializeJson(d, "{\"wifi\":{\"networks\":[{\"ssid\":\"a\",\"password:\":\"p\"}]},\"top:\":{\"k:\":1}}");
  configMigrate(d, onStep);
  TEST_ASSERT_EQUAL_STRING("p", d["wifi"]["networks"][0]["password"] | "");
  TEST_ASSERT_TRUE(d["wifi"]["networks"][0]["password:"].isNull());
  TEST_ASSERT_EQUAL(1, d["top"]["k"].as<int>());
  TEST_ASSERT_TRUE(d["top:"].isNull());
}

void test_lone_colon_key_and_values_untouched() {
  JsonDocument d; deserializeJson(d, "{\":\":1,\"url\":\"http://h:1/\",\"a\":\"b:\"}");
  configMigrate(d, onStep);
  TEST_ASSERT_EQUAL(1, d[":"].as<int>());
  TEST_ASSERT_EQUAL_STRING("http://h:1/", d["url"] | "");
  TEST_ASSERT_EQUAL_STRING("b:", d["a"] | "");
  TEST_ASSERT_EQUAL(0, lastChanged);
}

void test_migration_is_idempotent() {
  JsonDocument d; deserializeJson(d, "{\"mqtt\":{\"user:\":\"v\"}}");
  configMigrate(d, onStep);
  const std::string once = out(d);
  configDropColonKeys(d.as<JsonVariant>());
  TEST_ASSERT_EQUAL_STRING(once.c_str(), out(d).c_str());
  TEST_ASSERT_EQUAL(CFG_SCHEMA_CURRENT, configMigrate(d, onStep));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_absent_schema_is_zero);
  RUN_TEST(test_bad_schema_is_invalid_and_untouched);
  RUN_TEST(test_newer_schema_left_alone);
  RUN_TEST(test_current_schema_untouched);
  RUN_TEST(test_colon_duplicate_dropped_plain_kept);
  RUN_TEST(test_colon_only_key_renamed);
  RUN_TEST(test_nested_objects_and_arrays);
  RUN_TEST(test_lone_colon_key_and_values_untouched);
  RUN_TEST(test_migration_is_idempotent);
  return UNITY_END();
}
