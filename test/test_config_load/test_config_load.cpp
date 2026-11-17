// Host-native unit tests for config_load_policy.h (which doc stays live on a
// failed /config.json read, and what an fs.write config body must be).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "config_load_policy.h"
#include <string>

void setUp(void) {}
void tearDown(void) {}

// A LittleFS stand-in: mounts or not, holds the file text or no file.
struct FakeFs {
  bool        mounts = true;
  bool        exists = true;
  std::string text;
  bool mount() { return mounts; }
  bool parse(JsonDocument& doc, DeserializationError& err) {
    if (!exists) return false;
    err = deserializeJson(doc, text);
    return true;
  }
};

static void liveWithBroker(JsonDocument& live) {
  live.clear();
  live["mqtt"]["broker"] = "old.example";
}

void test_good_file_replaces_the_live_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.text = "{\"mqtt\":{\"broker\":\"new.example\"}}";
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_OK, configLoadInto(fs, live, err));
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
}

// The reload after fs.rm /config.json and the one after a corrupt write end
// the same way: the doc the unit runs on stays.
void test_missing_file_keeps_the_live_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.exists = false;
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_FILE_MISSING, configLoadInto(fs, live, err));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("config.file_missing", configLoadEvent(CONFIG_LOAD_FILE_MISSING));
}

void test_malformed_file_keeps_the_live_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.text = "{\"mqtt\":{\"broker\":\"half";
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_PARSE_FAILED, configLoadInto(fs, live, err));
  TEST_ASSERT_TRUE((bool)err);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("config.parse_failed", configLoadEvent(CONFIG_LOAD_PARSE_FAILED));
}

// Valid JSON that is not an object would serve defaults for every key.
void test_non_object_file_keeps_the_live_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.text = "[1,2]";
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_PARSE_FAILED, configLoadInto(fs, live, err));
  TEST_ASSERT_FALSE((bool)err);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
}

void test_mount_failure_keeps_the_live_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.mounts = false;
  fs.text = "{\"mqtt\":{\"broker\":\"new.example\"}}";
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_MOUNT_FAILED, configLoadInto(fs, live, err));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("config.mount_failed", configLoadEvent(CONFIG_LOAD_MOUNT_FAILED));
}

// At boot the live doc is empty, so a failure runs on defaults.
void test_boot_failure_leaves_an_empty_doc(void) {
  JsonDocument live;
  FakeFs fs;
  fs.exists = false;
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_FILE_MISSING, configLoadInto(fs, live, err));
  TEST_ASSERT_TRUE(live.isNull());
  TEST_ASSERT_EQUAL_STRING("config.loaded", configLoadEvent(CONFIG_LOAD_OK));
}

void test_config_bytes_must_be_one_object(void) {
  const char ok[] = "{\"wifi\":{}}";
  TEST_ASSERT_TRUE(configBytesAreObject(ok, sizeof(ok) - 1));
  TEST_ASSERT_FALSE(configBytesAreObject("{\"wifi\":", 8));
  TEST_ASSERT_FALSE(configBytesAreObject("[]", 2));
  TEST_ASSERT_FALSE(configBytesAreObject("42", 2));
  TEST_ASSERT_FALSE(configBytesAreObject("", 0));
  TEST_ASSERT_FALSE(configBytesAreObject(nullptr, 4));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_good_file_replaces_the_live_doc);
  RUN_TEST(test_missing_file_keeps_the_live_doc);
  RUN_TEST(test_malformed_file_keeps_the_live_doc);
  RUN_TEST(test_non_object_file_keeps_the_live_doc);
  RUN_TEST(test_mount_failure_keeps_the_live_doc);
  RUN_TEST(test_boot_failure_leaves_an_empty_doc);
  RUN_TEST(test_config_bytes_must_be_one_object);
  return UNITY_END();
}
