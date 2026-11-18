// Host-native unit tests for config_load_policy.h (which doc stays live on a
// failed /config.json read, and what an fs.write config body must be).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "config_load_policy.h"
#include <stdlib.h>
#include <string>

void setUp(void) {}
void tearDown(void) {}

// The file as a stream, one byte per read() like a LittleFS File.
struct FakeFile {
  const std::string& text;
  size_t pos = 0;
  int read() { return pos < text.size() ? (unsigned char)text[pos++] : -1; }
  size_t readBytes(char* buf, size_t n) {
    size_t i = 0;
    for (int c; i < n && (c = read()) >= 0; i++) buf[i] = (char)c;
    return i;
  }
};

// A LittleFS stand-in: mounts or not, holds the file text or no file.
// parse() reads the way LittleFsConfigSource does.
struct FakeFs {
  bool        mounts = true;
  bool        exists = true;
  std::string text;
  bool mount() { return mounts; }
  bool parse(JsonDocument& doc, DeserializationError& err) {
    if (!exists) return false;
    FakeFile f{text};
    configParseWhole(f, doc, err);
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

// CR #129: the first object loaded and config.reload said ok.
void test_trailing_content_keeps_the_live_doc(void) {
  const char* bad[] = {
    "{\"schema\":1,\"mqtt\":{\"broker\":\"new.example\"}}{\"wifi\":{}}",
    "{\"mqtt\":{\"broker\":\"new.example\"}} x",
    "{\"mqtt\":{\"broker\":\"new.example\"}}\n}",
  };
  for (const char* text : bad) {
    JsonDocument live;
    liveWithBroker(live);
    FakeFs fs;
    fs.text = text;
    DeserializationError err;
    TEST_ASSERT_EQUAL_INT_MESSAGE(CONFIG_LOAD_PARSE_FAILED, configLoadInto(fs, live, err), text);
    TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  }
  JsonDocument live;
  FakeFs fs;
  fs.text = " {\"mqtt\":{\"broker\":\"new.example\"}}\r\n\t ";
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_OK, configLoadInto(fs, live, err));
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
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

// CR #128: ArduinoJson stops at the first value, and the next boot would
// drop everything after it.
void test_config_bytes_refuse_trailing_bytes(void) {
  const char two[] = "{\"wifi\":{}}{\"mqtt\":{}}";
  TEST_ASSERT_FALSE(configBytesAreObject(two, sizeof(two) - 1));
  const char junk[] = "{\"a\":\"}\"} x";
  TEST_ASSERT_FALSE(configBytesAreObject(junk, sizeof(junk) - 1));
  const char esc[] = "{\"a\":\"\\\\\"}{}";
  TEST_ASSERT_FALSE(configBytesAreObject(esc, sizeof(esc) - 1));
  const char nul[] = "{\"a\":1}\0";
  TEST_ASSERT_FALSE(configBytesAreObject(nul, sizeof(nul)));
  const char ws[] = " {\"a\":\"\\\"}\"}\r\n\t ";
  TEST_ASSERT_TRUE(configBytesAreObject(ws, sizeof(ws) - 1));
  const char single[] = "{'a':'\"'}{\"b\":1}";
  TEST_ASSERT_FALSE(configNothingAfterObject(single, sizeof(single) - 1));
  const char open[] = "{\"a\":[1";
  TEST_ASSERT_FALSE(configNothingAfterObject(open, sizeof(open) - 1));
}

// The prefix hold a replace clears and a failed persist puts back.
struct FakeHold {
  char disk[8];
  bool held;
};

static bool saveOk()   { return true; }
static bool saveFail() { return false; }
static int saves;
static bool saveCount() { saves++; return true; }

// CR #128: "[]" saved fine and the next boot refused the file.
void test_replace_refuses_a_non_object(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeHold hold = {"boot", true};
  DeserializationError err;
  saves = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_NOT_OBJECT, configReplaceLive(live, hold, "[]", err, saveCount));
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_NOT_OBJECT, configReplaceLive(live, hold, "null", err, saveCount));
  TEST_ASSERT_EQUAL_INT(0, saves);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(hold.held);
}

// CR #129: save() persisted the first object and dropped the rest.
void test_replace_refuses_trailing_content(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeHold hold = {"boot", true};
  DeserializationError err;
  saves = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_NOT_OBJECT,
                        configReplaceLive(live, hold, "{\"mqtt\":{\"broker\":\"new.example\"}}{\"wifi\":{}}", err, saveCount));
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_NOT_OBJECT,
                        configReplaceLive(live, hold, "{\"mqtt\":{}} x", err, saveCount));
  TEST_ASSERT_EQUAL_INT(0, saves);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(hold.held);
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_OK,
                        configReplaceLive(live, hold, "{\"mqtt\":{\"broker\":\"new.example\"}}\r\n ", err, saveCount));
  TEST_ASSERT_EQUAL_INT(1, saves);
}

void test_replace_malformed_keeps_doc_and_hold(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeHold hold = {"boot", true};
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_PARSE_FAILED, configReplaceLive(live, hold, "{\"mqtt\":", err, saveOk));
  TEST_ASSERT_TRUE((bool)err);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(hold.held);
}

void test_replace_saved_installs_doc_and_clears_hold(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeHold hold = {"boot", true};
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_OK,
                        configReplaceLive(live, hold, "{\"mqtt\":{\"broker\":\"new.example\"}}", err, saveOk));
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_FALSE(hold.held);
}

// CR #128: a failed save rolled back through load(). When that read failed
// too, the unsaved push stayed live and the hold was gone.
void test_replace_failed_persist_restores_doc_and_hold(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeHold hold = {"boot", true};
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_REPLACE_PERSIST_FAILED,
                        configReplaceLive(live, hold, "{\"mqtt\":{\"broker\":\"new.example\"}}", err, saveFail));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(hold.held);
  TEST_ASSERT_EQUAL_STRING("boot", hold.disk);
}

static void editNetwork(JsonDocument& d) {
  d["mqtt"]["broker"] = "new.example";
  d["wifi"]["networks"][0]["ssid"] = "rejected";
}

void test_edit_saved_keeps_the_edit(void) {
  JsonDocument live;
  liveWithBroker(live);
  TEST_ASSERT_EQUAL_INT(CONFIG_EDIT_OK, configEditLive(live, editNetwork, saveOk));
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("rejected", live["wifi"]["networks"][0]["ssid"] | "");
}

// CR #131: the setup page rolled a failed save back through load(), and a
// failed load kept the rejected network live.
void test_edit_failed_persist_restores_the_doc(void) {
  JsonDocument live;
  liveWithBroker(live);
  TEST_ASSERT_EQUAL_INT(CONFIG_EDIT_PERSIST_FAILED, configEditLive(live, editNetwork, saveFail));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(live["wifi"].isNull());
}

// An allocator that runs out on demand.
struct FlakyAllocator : ArduinoJson::Allocator {
  bool fail = false;
  void* allocate(size_t n) override { return fail ? nullptr : malloc(n); }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override { return fail ? nullptr : realloc(p, n); }
};

static bool g_edited;
static void markEdited(JsonDocument&) { g_edited = true; }

// No room to keep the doc as it was: nothing is edited or saved.
void test_edit_without_room_for_the_copy_changes_nothing(void) {
  FlakyAllocator heap;
  JsonDocument live(&heap);
  liveWithBroker(live);
  heap.fail = true;
  g_edited = false;
  saves = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_EDIT_NO_COPY, configEditLive(live, markEdited, saveCount));
  TEST_ASSERT_FALSE(g_edited);
  TEST_ASSERT_EQUAL_INT(0, saves);
  heap.fail = false;
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_good_file_replaces_the_live_doc);
  RUN_TEST(test_missing_file_keeps_the_live_doc);
  RUN_TEST(test_malformed_file_keeps_the_live_doc);
  RUN_TEST(test_non_object_file_keeps_the_live_doc);
  RUN_TEST(test_trailing_content_keeps_the_live_doc);
  RUN_TEST(test_mount_failure_keeps_the_live_doc);
  RUN_TEST(test_boot_failure_leaves_an_empty_doc);
  RUN_TEST(test_config_bytes_must_be_one_object);
  RUN_TEST(test_config_bytes_refuse_trailing_bytes);
  RUN_TEST(test_replace_refuses_a_non_object);
  RUN_TEST(test_replace_refuses_trailing_content);
  RUN_TEST(test_replace_malformed_keeps_doc_and_hold);
  RUN_TEST(test_replace_saved_installs_doc_and_clears_hold);
  RUN_TEST(test_replace_failed_persist_restores_doc_and_hold);
  RUN_TEST(test_edit_saved_keeps_the_edit);
  RUN_TEST(test_edit_failed_persist_restores_the_doc);
  RUN_TEST(test_edit_without_room_for_the_copy_changes_nothing);
  return UNITY_END();
}
