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
  long size() const { return exists ? (long)text.size() : -1; }
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

// Counts what write() saw, so a test sees a write that should not happen.
static int         writes;
static std::string written;
static bool writeOk(JsonVariantConst src) {
  writes++;
  written.clear();
  serializeJson(src, written);
  return true;
}
static bool writeFail(JsonVariantConst) {
  writes++;
  return false;
}

// An allocator that runs out on demand and counts its allocations.
struct FlakyAllocator : ArduinoJson::Allocator {
  bool fail   = false;
  int  allocs = 0;
  void* allocate(size_t n) override {
    if (fail) return nullptr;
    allocs++;
    return malloc(n);
  }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override {
    if (fail) return nullptr;
    allocs++;
    return realloc(p, n);
  }
};

// The replace path: parse the pushed text the way Config::replace does.
static bool parseText(const char* json, JsonDocument& doc, DeserializationError& err) {
  ConfigTextReader in{json};
  configParseWhole(in, doc, err);
  return !err;
}

// CR #128: "[]" saved fine and the next boot refused the file.
void test_commit_refuses_a_non_object(void) {
  JsonDocument live;
  liveWithBroker(live);
  writes = 0;
  const char* bad[] = {"[]", "null", "42"};
  for (const char* text : bad) {
    JsonDocument staged;
    DeserializationError err;
    TEST_ASSERT_TRUE(parseText(text, staged, err));
    TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_NOT_OBJECT, configCommit(live, staged, writeOk));
  }
  TEST_ASSERT_EQUAL_INT(0, writes);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
}

// CR #129: save() persisted the first object and dropped the rest.
void test_replace_refuses_trailing_content(void) {
  JsonDocument staged;
  DeserializationError err;
  TEST_ASSERT_FALSE(parseText("{\"mqtt\":{\"broker\":\"new.example\"}}{\"wifi\":{}}", staged, err));
  TEST_ASSERT_FALSE(parseText("{\"mqtt\":{}} x", staged, err));
  TEST_ASSERT_FALSE(parseText("{\"mqtt\":", staged, err));
  TEST_ASSERT_TRUE(parseText("{\"mqtt\":{\"broker\":\"new.example\"}}\r\n ", staged, err));
}

static void editNetwork(JsonDocument& d) {
  d["mqtt"]["broker"] = "new.example";
  d["wifi"]["networks"][0]["ssid"] = "rejected";
}

// The staged doc moves in: no allocation, and the write saw what went live.
void test_commit_saved_moves_the_staged_doc_in(void) {
  FlakyAllocator heap;
  JsonDocument live(&heap);
  liveWithBroker(live);
  JsonDocument staged(&heap);
  staged["mqtt"]["broker"] = "old.example";
  editNetwork(staged);
  writes = 0;
  const int before = heap.allocs;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, staged, writeOk));
  TEST_ASSERT_EQUAL_INT(before, heap.allocs);
  TEST_ASSERT_EQUAL_INT(1, writes);
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("rejected", live["wifi"]["networks"][0]["ssid"] | "");
  TEST_ASSERT_TRUE(written.find("rejected") != std::string::npos);
}

// CR #131: the setup page rolled a failed save back through load(), and a
// failed load kept the rejected network live.
void test_commit_failed_persist_leaves_live_alone(void) {
  JsonDocument live;
  liveWithBroker(live);
  JsonDocument staged;
  editNetwork(staged);
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_PERSIST_FAILED, configCommit(live, staged, writeFail));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(live["wifi"].isNull());
}

// CR #131: an edit that ran out of heap half way was written to flash.
void test_commit_refuses_an_overflowed_edit_before_writing(void) {
  JsonDocument live;
  liveWithBroker(live);
  FlakyAllocator heap;
  JsonDocument staged(&heap);
  staged["mqtt"]["broker"] = "old.example";
  heap.fail = true;
  staged["wifi"]["networks"][0]["ssid"] = std::string("a copied string the heap has no room for");
  heap.fail = false;
  TEST_ASSERT_TRUE(staged.overflowed());
  writes = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_NO_HEAP, configCommit(live, staged, writeOk));
  TEST_ASSERT_EQUAL_INT(0, writes);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
}

// An edit whose fn turns the root into an array never reaches the file.
void test_commit_refuses_an_edit_that_made_root_an_array(void) {
  JsonDocument live;
  liveWithBroker(live);
  JsonDocument staged;
  staged["mqtt"]["broker"] = "new.example";
  staged.to<JsonArray>().add(1);
  writes = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_NOT_OBJECT, configCommit(live, staged, writeOk));
  TEST_ASSERT_EQUAL_INT(0, writes);
  TEST_ASSERT_TRUE(live.is<JsonObject>());
}

// A stage source: the FakeFs file, or the live doc when there is none.
struct StageFs : FakeFs {
  bool liveCopies = true;
  bool copyLive(JsonDocument& doc) {
    if (!liveCopies) return false;
    doc["mqtt"]["broker"] = "live.example";
    return true;
  }
};

// Decision 1: an edit applies on top of the file, live text only without one.
void test_stage_prefers_the_file_and_falls_back_to_live(void) {
  StageFs fs;
  fs.text = "{\"mqtt\":{\"broker\":\"disk.example\"}}";
  JsonDocument staged;
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_FILE, configStage(fs, staged, err));
  TEST_ASSERT_EQUAL_STRING("disk.example", staged["mqtt"]["broker"] | "");
  fs.exists = false;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_LIVE, configStage(fs, staged, err));
  TEST_ASSERT_EQUAL_STRING("live.example", staged["mqtt"]["broker"] | "");
  fs.liveCopies = false;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_NO_LIVE, configStage(fs, staged, err));
}

// An edit on a file the next boot would refuse is refused, not merged.
void test_edit_refuses_a_file_with_trailing_content(void) {
  StageFs fs;
  JsonDocument staged;
  DeserializationError err;
  const char* bad[] = {"{\"mqtt\":{}} x", "{\"a\":1}{\"b\":2}", "[1]", "{\"mqtt\":"};
  for (const char* text : bad) {
    fs.text = text;
    TEST_ASSERT_EQUAL_INT_MESSAGE(CONFIG_STAGE_PARSE_FAILED, configStage(fs, staged, err), text);
  }
}

// A held boot prefix stays live across a commit, and the file keeps the
// prefix it had: the hold is applied once, inside the commit.
void test_commit_keeps_the_boot_prefix_hold(void) {
  JsonDocument live;
  live["mqtt"]["topic_prefix"] = "boot";
  live["mqtt"]["broker"]       = "old.example";
  ConfigPrefixHold hold        = {"disk", true, true};
  JsonDocument staged;
  staged["mqtt"]["topic_prefix"] = "disk";
  staged["mqtt"]["broker"]       = "new.example";
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, staged, writeOk, &hold));
  TEST_ASSERT_TRUE(written.find("\"disk\"") != std::string::npos);
  TEST_ASSERT_TRUE(written.find("boot") == std::string::npos);
  TEST_ASSERT_EQUAL_STRING("boot", live["mqtt"]["topic_prefix"] | "");
  TEST_ASSERT_EQUAL_STRING("new.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_TRUE(hold.held);
  TEST_ASSERT_EQUAL_STRING("disk", hold.disk);
  // No hold before: none after.
  ConfigPrefixHold none = {};
  JsonDocument next;
  next["mqtt"]["topic_prefix"] = "other";
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, next, writeOk, &none));
  TEST_ASSERT_FALSE(none.held);
  TEST_ASSERT_EQUAL_STRING("other", live["mqtt"]["topic_prefix"] | "");
}

// No heap for the overlay: the prefix on disk goes live and the hold is dropped.
void test_commit_without_room_for_the_hold_drops_it(void) {
  JsonDocument live;
  live["mqtt"]["topic_prefix"] = std::string("boot");
  ConfigPrefixHold hold        = {"disk", true, true};
  FlakyAllocator heap;
  JsonDocument staged(&heap);
  staged["mqtt"]["topic_prefix"] = "disk";
  heap.fail = true;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, staged, writeOk, &hold));
  heap.fail = false;
  TEST_ASSERT_FALSE(hold.held);
}

// The pushed text is read one byte at a time or in blocks.
void test_text_reader_reads_blocks(void) {
  ConfigTextReader in{"abcde"};
  char buf[4];
  TEST_ASSERT_EQUAL_UINT(4, in.readBytes(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT('e', in.read());
  TEST_ASSERT_EQUAL_UINT(0, in.readBytes(buf, sizeof(buf)));
  TEST_ASSERT_EQUAL_INT(-1, in.read());
}

// A refused commit leaves the hold as it was.
void test_failed_commit_keeps_the_hold(void) {
  JsonDocument live;
  live["mqtt"]["topic_prefix"] = "boot";
  ConfigPrefixHold hold        = {"disk", true, true};
  JsonDocument staged;
  staged["mqtt"]["topic_prefix"] = "disk";
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_PERSIST_FAILED, configCommit(live, staged, writeFail, &hold));
  TEST_ASSERT_TRUE(hold.held);
  TEST_ASSERT_EQUAL_STRING("boot", live["mqtt"]["topic_prefix"] | "");
}

void test_hold_refuses_a_non_string_or_long_prefix(void) {
  ConfigPrefixHold hold = {};
  JsonDocument doc;
  doc["mqtt"]["topic_prefix"] = 5;
  TEST_ASSERT_EQUAL_INT(CONFIG_HOLD_TYPE, configHoldPrefix(doc, hold, "boot"));
  TEST_ASSERT_EQUAL_INT(CONFIG_HOLD_NULL, configHoldPrefix(doc, hold, nullptr));
  doc["mqtt"].remove("topic_prefix");
  std::string longPrefix(CONFIG_PREFIX_CAP, 'x');
  TEST_ASSERT_EQUAL_INT(CONFIG_HOLD_LENGTH, configHoldPrefix(doc, hold, longPrefix.c_str()));
  TEST_ASSERT_FALSE(hold.held);
  TEST_ASSERT_EQUAL_INT(CONFIG_HOLD_OK, configHoldPrefix(doc, hold, "boot"));
  TEST_ASSERT_FALSE(hold.diskPresent);
  TEST_ASSERT_EQUAL_STRING("boot", doc["mqtt"]["topic_prefix"] | "");
}

// A config object of exactly n bytes of JSON text.
static std::string objectOfSize(size_t n) {
  std::string t = "{\"a\":\"";
  t += std::string(n - t.size() - 2, 'x');
  t += "\"}";
  return t;
}

static std::string json(JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  return out;
}

// Nested objects n deep: {"a":{"a":...{}}}.
static std::string nested(int n) {
  std::string t;
  for (int i = 1; i < n; i++) t += "{\"a\":";
  t += "{}";
  for (int i = 1; i < n; i++) t += "}";
  return t;
}

// Decision 2: a config file past 16 KB is not parsed, at boot or on a reload.
void test_load_refuses_a_file_past_the_cap(void) {
  JsonDocument live;
  liveWithBroker(live);
  FakeFs fs;
  fs.text = objectOfSize(CONFIG_FILE_MAX + 1);
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_TOO_LARGE, configLoadInto(fs, live, err));
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  TEST_ASSERT_EQUAL_STRING("config.too_large", configLoadEvent(CONFIG_LOAD_TOO_LARGE));
  fs.text = objectOfSize(CONFIG_FILE_MAX);
  TEST_ASSERT_EQUAL_INT(CONFIG_LOAD_OK, configLoadInto(fs, live, err));
  StageFs sfs;
  sfs.text = objectOfSize(CONFIG_FILE_MAX + 1);
  JsonDocument staged;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_TOO_LARGE, configStage(sfs, staged, err));
}

// A commit never writes a file the next boot would refuse as too large.
void test_commit_refuses_a_doc_past_the_cap(void) {
  JsonDocument live;
  liveWithBroker(live);
  JsonDocument staged;
  staged["a"] = std::string(CONFIG_FILE_MAX, 'x');
  writes = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_TOO_LARGE, configCommit(live, staged, writeOk));
  TEST_ASSERT_EQUAL_INT(0, writes);
  TEST_ASSERT_EQUAL_STRING("old.example", live["mqtt"]["broker"] | "");
  JsonDocument fits;
  fits["a"] = std::string(CONFIG_FILE_MAX - 8, 'x');
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, fits, writeOk));
}

// Decision 2: every config parse stops at CONFIG_MAX_DEPTH (8) levels.
void test_config_parse_refuses_depth_past_8(void) {
  TEST_ASSERT_EQUAL_INT(8, CONFIG_MAX_DEPTH);
  JsonDocument doc;
  DeserializationError err;
  std::string ok = nested(CONFIG_MAX_DEPTH);
  TEST_ASSERT_TRUE(parseText(ok.c_str(), doc, err));
  std::string deep = nested(CONFIG_MAX_DEPTH + 1);
  TEST_ASSERT_FALSE(parseText(deep.c_str(), doc, err));
  TEST_ASSERT_TRUE(err == DeserializationError::TooDeep);
  TEST_ASSERT_TRUE(configBytesAreObject(ok.data(), ok.size()));
  TEST_ASSERT_FALSE(configBytesAreObject(deep.data(), deep.size()));
  std::string big = objectOfSize(CONFIG_FILE_MAX + 1);
  TEST_ASSERT_FALSE(configBytesAreObject(big.data(), big.size()));
}

// The setup portal is the way back from a broken file: a corrupt, oversized
// or non-object file stages a fresh doc that carries only the schema.
void test_rescue_stage_replaces_a_broken_file(void) {
  const std::string broken[] = {"{\"mqtt\":", "{\"a\":1} x", "[1]", objectOfSize(CONFIG_FILE_MAX + 1),
                                nested(CONFIG_MAX_DEPTH + 1)};
  for (const std::string& text : broken) {
    StageFs fs;
    fs.text = text;
    JsonDocument staged;
    DeserializationError err;
    TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_RESCUED, configStageRescue(fs, staged, err, 3));
    TEST_ASSERT_EQUAL_STRING("{\"schema\":3}", json(staged).c_str());
  }
}

// A readable file or a missing one stages exactly as configStage does.
void test_rescue_stage_keeps_a_good_file(void) {
  StageFs fs;
  fs.text = "{\"mqtt\":{\"broker\":\"disk.example\"}}";
  JsonDocument staged;
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_FILE, configStageRescue(fs, staged, err, 3));
  TEST_ASSERT_EQUAL_STRING("disk.example", staged["mqtt"]["broker"] | "");
  fs.exists = false;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_LIVE, configStageRescue(fs, staged, err, 3));
  TEST_ASSERT_EQUAL_STRING("live.example", staged["mqtt"]["broker"] | "");
  fs.liveCopies = false;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_NO_LIVE, configStageRescue(fs, staged, err, 3));
}

// The rescued doc plus the form goes through the commit and replaces the file.
void test_rescue_commit_writes_only_the_form(void) {
  StageFs fs;
  fs.text = "{\"mqtt\":{\"broker\":\"half";
  JsonDocument staged;
  DeserializationError err;
  TEST_ASSERT_EQUAL_INT(CONFIG_STAGE_RESCUED, configStageRescue(fs, staged, err, 1));
  staged["wifi"]["networks"][0]["ssid"] = "home";
  JsonDocument live;
  writes = 0;
  TEST_ASSERT_EQUAL_INT(CONFIG_COMMIT_OK, configCommit(live, staged, writeOk));
  TEST_ASSERT_EQUAL_INT(1, writes);
  TEST_ASSERT_EQUAL_STRING("{\"schema\":1,\"wifi\":{\"networks\":[{\"ssid\":\"home\"}]}}", written.c_str());
  TEST_ASSERT_EQUAL_STRING("home", live["wifi"]["networks"][0]["ssid"] | "");
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
  RUN_TEST(test_commit_refuses_a_non_object);
  RUN_TEST(test_replace_refuses_trailing_content);
  RUN_TEST(test_commit_saved_moves_the_staged_doc_in);
  RUN_TEST(test_commit_failed_persist_leaves_live_alone);
  RUN_TEST(test_commit_refuses_an_overflowed_edit_before_writing);
  RUN_TEST(test_commit_refuses_an_edit_that_made_root_an_array);
  RUN_TEST(test_stage_prefers_the_file_and_falls_back_to_live);
  RUN_TEST(test_edit_refuses_a_file_with_trailing_content);
  RUN_TEST(test_commit_keeps_the_boot_prefix_hold);
  RUN_TEST(test_commit_without_room_for_the_hold_drops_it);
  RUN_TEST(test_text_reader_reads_blocks);
  RUN_TEST(test_failed_commit_keeps_the_hold);
  RUN_TEST(test_hold_refuses_a_non_string_or_long_prefix);
  RUN_TEST(test_load_refuses_a_file_past_the_cap);
  RUN_TEST(test_commit_refuses_a_doc_past_the_cap);
  RUN_TEST(test_config_parse_refuses_depth_past_8);
  RUN_TEST(test_rescue_stage_replaces_a_broken_file);
  RUN_TEST(test_rescue_stage_keeps_a_good_file);
  RUN_TEST(test_rescue_commit_writes_only_the_form);
  return UNITY_END();
}
