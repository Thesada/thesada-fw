// Host-native unit tests for config_mask_policy.h: secret config fields read
// as a marker on every read path, and a marker written back keeps the secret.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "config_mask_policy.h"
#include "config_write_policy.h"
#include "secret_keymap.h"
#include <string.h>
#include <string>

void setUp(void) {}
void tearDown(void) {}

static const char* kSecrets[] = {"S-mqtt", "S-web", "S-bot", "S-ap", "S-claim", "S-wifi0", "S-wifi1", "S-sim", "S-psk"};

static const char kFull[] =
    "{\"device\":{\"name\":\"n1\"},"
    "\"mqtt\":{\"broker\":\"b.example\",\"port\":8883,\"user\":\"u1\",\"password\":\"S-mqtt\"},"
    "\"web\":{\"user\":\"admin\",\"password\":\"S-web\"},"
    "\"telegram\":{\"bot_token\":\"S-bot\",\"chat_ids\":[\"1\",\"2\"]},"
    "\"enroll\":{\"claim_code\":\"S-claim\"},"
    "\"wifi\":{\"ap_password\":\"S-ap\",\"networks\":["
    "{\"ssid\":\"home\",\"password\":\"S-wifi0\"},{\"ssid\":\"net2\",\"password\":\"S-wifi1\"},"
    "{\"ssid\":\"open\",\"password\":\"\"}]},"
    "\"cellular\":{\"apn\":\"iot\",\"sim_pin\":\"S-sim\"},"
    "\"lora\":{\"meshtastic\":{\"psk\":\"S-psk\"}},"
    "\"temperature\":{\"pin\":4,\"heartbeat_pin\":2}}";

static void parse(JsonDocument& doc, const char* text) {
  TEST_ASSERT_FALSE(deserializeJson(doc, text));
}

static std::string json(JsonVariantConst v) {
  std::string s;
  serializeJson(v, s);
  return s;
}

static void assertNoSecret(const std::string& s) {
  for (const char* sec : kSecrets) {
    if (s.find(sec) != std::string::npos) {
      TEST_FAIL_MESSAGE(sec);
    }
  }
}

// Every field the NVS keymap knows reads as secret by its leaf name.
void test_keymap_fields_are_secret(void) {
  const char* fields[] = {"enroll.claim_code", "mqtt.password", "telegram.bot_token", "web.password",
                          "wifi.ap_password"};
  char key[16];
  for (const char* f : fields) {
    TEST_ASSERT_TRUE_MESSAGE(secretNvsKeyFor(f, key, sizeof(key)), f);
    TEST_ASSERT_TRUE_MESSAGE(configPathIsSecret(f), f);
  }
  TEST_ASSERT_TRUE(configPathIsSecret("wifi.networks.0.password"));
}

void test_key_name_table(void) {
  const char* secret[] = {"password", "PASSWORD", "ap_password", "passwd", "bot_token", "token", "secret",
                          "client_secret", "api_key", "key", "passphrase", "credential", "pass", "mqtt_pass",
                          "pwd", "wifi-pwd", "psk", "seed", "sim_pin", "claim_code", "x.psk"};
  const char* plain[] = {"pin", "heartbeat_pin", "pin_clk", "ssid", "user", "broker", "port", "passive",
                         "bypass_ms", "spin", "seeds", "pskx", "apn", "chat_ids", "keys", "name", "", "tokens"};
  for (const char* k : secret) TEST_ASSERT_TRUE_MESSAGE(configKeyIsSecret(k), k);
  for (const char* k : plain) TEST_ASSERT_FALSE_MESSAGE(configKeyIsSecret(k), k);
  TEST_ASSERT_FALSE(configKeyIsSecret(nullptr));
}

void test_path_secret_any_segment(void) {
  TEST_ASSERT_TRUE(configPathIsSecret("mqtt.password"));
  TEST_ASSERT_TRUE(configPathIsSecret("lora.meshtastic.psk"));
  TEST_ASSERT_TRUE(configPathIsSecret("x.token.inner"));
  TEST_ASSERT_FALSE(configPathIsSecret("mqtt"));
  TEST_ASSERT_FALSE(configPathIsSecret("wifi.networks.0.ssid"));
  TEST_ASSERT_FALSE(configPathIsSecret(""));
  TEST_ASSERT_FALSE(configPathIsSecret(nullptr));
  char longKey[300];
  memset(longKey, 'a', sizeof(longKey) - 1);
  longKey[sizeof(longKey) - 1] = '\0';
  TEST_ASSERT_FALSE(configPathIsSecret(longKey));
  memcpy(longKey + 200, ".password", 10);
  TEST_ASSERT_TRUE(configPathIsSecret(longKey));
}

// The whole doc masked: no secret value, set vs empty kept, all else byte-identical.
void test_mask_tree_full_doc(void) {
  JsonDocument doc;
  parse(doc, kFull);
  TEST_ASSERT_EQUAL(10, configMaskTree(doc.as<JsonVariant>()));
  std::string s = json(doc);
  assertNoSecret(s);
  JsonDocument want;
  parse(want, kFull);
  want["mqtt"]["password"] = "(set)";
  want["web"]["password"] = "(set)";
  want["telegram"]["bot_token"] = "(set)";
  want["enroll"]["claim_code"] = "(set)";
  want["wifi"]["ap_password"] = "(set)";
  want["wifi"]["networks"][0]["password"] = "(set)";
  want["wifi"]["networks"][1]["password"] = "(set)";
  want["wifi"]["networks"][2]["password"] = "(empty)";
  want["cellular"]["sim_pin"] = "(set)";
  want["lora"]["meshtastic"]["psk"] = "(set)";
  TEST_ASSERT_EQUAL_STRING(json(want).c_str(), s.c_str());
}

void test_mask_non_string_and_null(void) {
  JsonDocument doc;
  parse(doc, "{\"a\":{\"token\":{\"x\":\"S-mqtt\"},\"pwd\":1234,\"password\":null,\"seed\":[\"S-web\"]}}");
  TEST_ASSERT_EQUAL(3, configMaskTree(doc.as<JsonVariant>()));
  TEST_ASSERT_EQUAL_STRING("{\"a\":{\"token\":\"(set)\",\"pwd\":\"(set)\",\"password\":null,\"seed\":\"(set)\"}}",
                           json(doc).c_str());
}

void test_mask_clean_doc_untouched(void) {
  JsonDocument doc;
  parse(doc, "{\"device\":{\"name\":\"n1\"},\"temperature\":{\"pin\":4}}");
  TEST_ASSERT_EQUAL(0, configMaskTree(doc.as<JsonVariant>()));
  TEST_ASSERT_EQUAL_STRING("{\"device\":{\"name\":\"n1\"},\"temperature\":{\"pin\":4}}", json(doc).c_str());
}

// config.get <key>: a secret leaf, a section, an array path.
void test_mask_node_for_path(void) {
  JsonDocument doc;
  parse(doc, kFull);
  std::string out;
  TEST_ASSERT_TRUE(configMaskedNodeText("mqtt.password", doc["mqtt"]["password"], out, false));
  TEST_ASSERT_EQUAL_STRING("\"(set)\"", out.c_str());
  TEST_ASSERT_TRUE(configMaskedNodeText("wifi.networks.2.password", doc["wifi"]["networks"][2]["password"], out, false));
  TEST_ASSERT_EQUAL_STRING("\"(empty)\"", out.c_str());
  TEST_ASSERT_TRUE(configMaskedNodeText("mqtt", doc["mqtt"], out, false));
  TEST_ASSERT_EQUAL_STRING("{\"broker\":\"b.example\",\"port\":8883,\"user\":\"u1\",\"password\":\"(set)\"}", out.c_str());
  TEST_ASSERT_TRUE(configMaskedNodeText("wifi.networks", doc["wifi"]["networks"], out, false));
  assertNoSecret(out);
  TEST_ASSERT_TRUE(configMaskedNodeText("wifi.networks.0", doc["wifi"]["networks"][0], out, false));
  TEST_ASSERT_EQUAL_STRING("{\"ssid\":\"home\",\"password\":\"(set)\"}", out.c_str());
  TEST_ASSERT_TRUE(configMaskedNodeText("", doc.as<JsonVariantConst>(), out, true));
  assertNoSecret(out);
  TEST_ASSERT_TRUE(out.find('\n') != std::string::npos);
  TEST_ASSERT_TRUE(configMaskedNodeText("mqtt.port", doc["mqtt"]["port"], out, false));
  TEST_ASSERT_EQUAL_STRING("8883", out.c_str());
  // The live doc is never changed by a masked read.
  TEST_ASSERT_EQUAL_STRING("S-mqtt", doc["mqtt"]["password"] | "");
}

// A clean subtree reads exactly as the plain serializer gives it.
void test_mask_node_clean_is_plain(void) {
  JsonDocument doc;
  parse(doc, "{\"a\":{\"f\":1.5,\"s\":\"x\"}}");
  std::string out, plain;
  TEST_ASSERT_TRUE(configMaskedNodeText("", doc.as<JsonVariantConst>(), out, true));
  serializeJsonPretty(doc, plain);
  TEST_ASSERT_EQUAL_STRING(plain.c_str(), out.c_str());
}

void test_marker_detection(void) {
  TEST_ASSERT_TRUE(configIsMaskMarker("(set)"));
  TEST_ASSERT_TRUE(configIsMaskMarker("(empty)"));
  TEST_ASSERT_FALSE(configIsMaskMarker("(SET)"));
  TEST_ASSERT_FALSE(configIsMaskMarker(""));
  TEST_ASSERT_FALSE(configIsMaskMarker(nullptr));
  TEST_ASSERT_FALSE(configIsMaskMarker("set"));
  TEST_ASSERT_TRUE(configTextHasMarker("{\"a\":\"(set)\"}", 13));
  TEST_ASSERT_TRUE(configTextHasMarker("{\"a\":\"(empty)\"}", 15));
  TEST_ASSERT_FALSE(configTextHasMarker("{\"a\":\"(se\"}", 11));
  TEST_ASSERT_FALSE(configTextHasMarker(nullptr, 0));
}

// A masked dump pushed back whole keeps every stored secret.
void test_keep_masked_round_trip(void) {
  JsonDocument stored, staged;
  parse(stored, kFull);
  parse(staged, kFull);
  configMaskTree(staged.as<JsonVariant>());
  staged["device"]["name"] = "n2";
  TEST_ASSERT_TRUE(configHasMaskMarker(staged.as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL(10, configKeepMasked(staged.as<JsonVariant>(), stored.as<JsonVariantConst>()));
  stored["device"]["name"] = "n2";
  TEST_ASSERT_EQUAL_STRING(json(stored).c_str(), json(staged).c_str());
  TEST_ASSERT_FALSE(configHasMaskMarker(staged.as<JsonVariantConst>()));
}

// Wifi networks match by ssid, so a reordered list keeps each password.
void test_keep_masked_wifi_by_ssid(void) {
  JsonDocument stored, staged;
  parse(stored, "{\"wifi\":{\"networks\":[{\"ssid\":\"home\",\"password\":\"S-wifi0\"},{\"ssid\":\"net2\",\"password\":\"S-wifi1\"}]}}");
  parse(staged, "{\"wifi\":{\"networks\":[{\"ssid\":\"net2\",\"password\":\"(set)\"},{\"ssid\":\"new\",\"password\":\"(set)\"},"
                "{\"ssid\":\"home\",\"password\":\"(set)\"},{\"ssid\":\"x\",\"password\":\"real\"}]}}");
  TEST_ASSERT_EQUAL(3, configKeepMasked(staged.as<JsonVariant>(), stored.as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL_STRING("{\"wifi\":{\"networks\":[{\"ssid\":\"net2\",\"password\":\"S-wifi1\"},{\"ssid\":\"new\"},"
                           "{\"ssid\":\"home\",\"password\":\"S-wifi0\"},{\"ssid\":\"x\",\"password\":\"real\"}]}}",
                           json(staged).c_str());
}

// No stored value: the marker is dropped, never written as the secret.
void test_keep_masked_no_stored_drops_marker(void) {
  JsonDocument stored, staged;
  parse(stored, "{\"mqtt\":{\"broker\":\"b\"},\"lst\":[1]}");
  parse(staged, "{\"mqtt\":{\"broker\":\"b\",\"password\":\"(set)\",\"token\":\"(empty)\"},\"web\":{\"password\":\"(set)\"},"
                "\"lst\":[{\"pwd\":\"(set)\"},{\"pwd\":\"(set)\"}],\"n\":{\"password\":\"(set)\"}}");
  stored["n"] = 5;
  TEST_ASSERT_EQUAL(6, configKeepMasked(staged.as<JsonVariant>(), stored.as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL_STRING("{\"mqtt\":{\"broker\":\"b\"},\"web\":{},\"lst\":[{},{}],\"n\":{}}", json(staged).c_str());
}

// A real new value and a non-secret key holding a marker string are written as given.
void test_keep_masked_real_values_win(void) {
  JsonDocument stored, staged;
  parse(stored, kFull);
  parse(staged, "{\"mqtt\":{\"password\":\"new-one\"},\"device\":{\"name\":\"(set)\"},\"wifi\":{\"networks\":[{\"password\":\"(set)\"}]}}");
  TEST_ASSERT_EQUAL(1, configKeepMasked(staged.as<JsonVariant>(), stored.as<JsonVariantConst>()));
  TEST_ASSERT_EQUAL_STRING("{\"mqtt\":{\"password\":\"new-one\"},\"device\":{\"name\":\"(set)\"},\"wifi\":{\"networks\":[{\"password\":\"S-wifi0\"}]}}",
                           json(staged).c_str());
  JsonDocument s2;
  parse(s2, "{\"a\":[[\"(set)\"]]}");
  TEST_ASSERT_EQUAL(0, configKeepMasked(s2.as<JsonVariant>(), stored.as<JsonVariantConst>()));
  TEST_ASSERT_FALSE(configHasMaskMarker(s2.as<JsonVariantConst>()));
}

// config.set and secret.set never store the marker as a value.
void test_set_refuses_marker(void) {
  TEST_ASSERT_TRUE(configSetKeepsSecret("mqtt.password", "(set)"));
  TEST_ASSERT_TRUE(configSetKeepsSecret("wifi.networks.0.password", "(empty)"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("mqtt", "{\"password\":\"(set)\"}"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("mqtt.password", "real"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("device.name", "(set)"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("mqtt.password", nullptr));
  TEST_ASSERT_FALSE(configSetKeepsSecret(nullptr, "(set)"));
}

// The config.set reply never echoes a secret value.
void test_saved_reply_masks_secret(void) {
  char buf[200];
  configSavedReply(buf, sizeof(buf), "mqtt.password", "S-mqtt");
  TEST_ASSERT_NULL(strstr(buf, "S-mqtt"));
  TEST_ASSERT_NOT_NULL(strstr(buf, "mqtt.password = (set)"));
  configSavedReply(buf, sizeof(buf), "mqtt", "{\"password\":\"S-mqtt\"}", true);
  TEST_ASSERT_NULL(strstr(buf, "S-mqtt"));
  configSavedReply(buf, sizeof(buf), "web.password", "");
  TEST_ASSERT_NOT_NULL(strstr(buf, "web.password = (empty)"));
  configSavedReply(buf, sizeof(buf), "device.name", "n1");
  TEST_ASSERT_NOT_NULL(strstr(buf, "device.name = n1"));
}

void test_config_family_paths(void) {
  const char* fam[] = {"/config.json", "config.json", "/./config.json", "//config.json", "/CONFIG.JSON",
                       "/config.json.bak", "/config.json.tmp", "/config.json.bak.tmp", "/config_backup.json",
                       "/sd/config_backup.json", "/config.json/", "/config.json.", "/config.json ", "/Config.Json.BAK",
                       "/sd/old/config.json"};
  const char* other[] = {"/ca.crt", "/scripts/main.lua", "/sd/log.csv", "/configs/x.lua", "/myconfig.json",
                         "/config", "", "/"};
  for (const char* p : fam) TEST_ASSERT_TRUE_MESSAGE(configFileIsFamily(p), p);
  for (const char* p : other) TEST_ASSERT_FALSE_MESSAGE(configFileIsFamily(p), p);
  TEST_ASSERT_FALSE(configFileIsFamily(nullptr));
}

// File reads: masked when it holds a secret, raw when clean, refused when a
// config-named file does not parse.
void test_file_read_policy(void) {
  std::string out;
  TEST_ASSERT_EQUAL(CFG_READ_MASKED, configFileReadMasked("/./config.json", kFull, strlen(kFull), out));
  assertNoSecret(out);
  TEST_ASSERT_TRUE(out.find("(set)") != std::string::npos);
  TEST_ASSERT_EQUAL(CFG_READ_MASKED, configFileReadMasked("/sd/notes.json", kFull, strlen(kFull), out));
  assertNoSecret(out);
  const char clean[] = "{\"device\":{\"name\":\"n1\"}}";
  TEST_ASSERT_EQUAL(CFG_READ_RAW, configFileReadMasked("/config.json", clean, strlen(clean), out));
  const char broken[] = "{\"mqtt\":{\"password\":\"S-mqtt\"";
  TEST_ASSERT_EQUAL(CFG_READ_REFUSE, configFileReadMasked("/config.json.tmp", broken, strlen(broken), out));
  TEST_ASSERT_EQUAL(CFG_READ_REFUSE, configFileReadMasked("/config.json", "not json", 8, out));
  TEST_ASSERT_EQUAL(CFG_READ_RAW, configFileReadMasked("/config.json", "", 0, out));
  TEST_ASSERT_EQUAL(CFG_READ_RAW, configFileReadMasked("/a.json", broken, strlen(broken), out));
  TEST_ASSERT_EQUAL(CFG_READ_RAW, configFileReadMasked("/main.lua", "print(1)", 8, out));
  TEST_ASSERT_EQUAL(CFG_READ_RAW, configFileReadMasked("/a.json", "[1]", 3, out));
  TEST_ASSERT_EQUAL(CFG_READ_REFUSE, configFileReadMasked("/config.json", "[1]", 3, out));
  TEST_ASSERT_EQUAL(CFG_READ_REFUSE, configFileReadMasked("/config.json", nullptr, 5, out));
}

// Which files a reader must buffer and check, by name and first byte.
void test_file_needs_check(void) {
  TEST_ASSERT_TRUE(configFileNeedsMask("/config.json", 'x'));
  TEST_ASSERT_TRUE(configFileNeedsMask("/a.txt", '{'));
  TEST_ASSERT_FALSE(configFileNeedsMask("/a.txt", 'p'));
  TEST_ASSERT_FALSE(configFileNeedsMask("/a.txt", -1));
}

// A key configKeyValid refuses is never kept: config.set refuses it as invalid.
void test_set_invalid_key_not_kept(void) {
  TEST_ASSERT_FALSE(configSetKeepsSecret("mqtt..password", "(set)"));
  TEST_ASSERT_FALSE(configSetKeepsSecret(".password", "(set)"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("mqtt.password.", "(empty)"));
  TEST_ASSERT_FALSE(configSetKeepsSecret("", "(set)"));
  TEST_ASSERT_TRUE(configSetKeepsSecret("mqtt.password", "(set)"));
}

// The stored side of a marker save: the file, else the live doc.
struct FakeKeepSource {
  const char* file;
  const char* live;
  bool copyFails;
  bool parse(JsonDocument& doc, DeserializationError& err) {
    if (!file) return false;
    err = deserializeJson(doc, file);
    return true;
  }
  bool copyLive(JsonDocument& doc) {
    if (!live || copyFails) return false;
    return !deserializeJson(doc, live);
  }
  bool hasLive() { return live != nullptr; }
};

static const char kStored[] = "{\"mqtt\":{\"user\":\"u1\",\"password\":\"S-mqtt\"}}";
static const char kMarked[] = "{\"mqtt\":{\"user\":\"u2\",\"password\":\"(set)\"}}";

// A marker save keeps the secret from the file, or from the live doc when the file is gone.
void test_keep_stored_from_file_or_live(void) {
  JsonDocument staged, stored;
  FakeKeepSource fromFile{kStored, nullptr, false};
  deserializeJson(staged, kMarked);
  TEST_ASSERT_EQUAL(CONFIG_KEEP_OK, configKeepStoredSecrets(fromFile, staged, stored));
  TEST_ASSERT_EQUAL_STRING("S-mqtt", staged["mqtt"]["password"].as<const char*>());
  FakeKeepSource fromLive{nullptr, kStored, false};
  deserializeJson(staged, kMarked);
  TEST_ASSERT_EQUAL(CONFIG_KEEP_OK, configKeepStoredSecrets(fromLive, staged, stored));
  TEST_ASSERT_EQUAL_STRING("S-mqtt", staged["mqtt"]["password"].as<const char*>());
  FakeKeepSource blank{nullptr, nullptr, false};
  deserializeJson(staged, kMarked);
  TEST_ASSERT_EQUAL(CONFIG_KEEP_OK, configKeepStoredSecrets(blank, staged, stored));
  TEST_ASSERT_TRUE(staged["mqtt"]["password"].isNull());
  JsonDocument clean;
  deserializeJson(clean, "{\"mqtt\":{\"user\":\"u2\"}}");
  FakeKeepSource none{nullptr, kStored, true};
  TEST_ASSERT_EQUAL(CONFIG_KEEP_NONE, configKeepStoredSecrets(none, clean, stored));
}

// A live doc that cannot be copied refuses the marker save, never drops the secret.
void test_keep_stored_copy_failed_refuses(void) {
  JsonDocument staged, stored;
  FakeKeepSource missing{nullptr, kStored, true};
  deserializeJson(staged, kMarked);
  TEST_ASSERT_EQUAL(CONFIG_KEEP_COPY_FAILED, configKeepStoredSecrets(missing, staged, stored));
  FakeKeepSource corrupt{"{\"mqtt\":", kStored, true};
  deserializeJson(staged, kMarked);
  TEST_ASSERT_EQUAL(CONFIG_KEEP_COPY_FAILED, configKeepStoredSecrets(corrupt, staged, stored));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_keymap_fields_are_secret);
  RUN_TEST(test_key_name_table);
  RUN_TEST(test_path_secret_any_segment);
  RUN_TEST(test_mask_tree_full_doc);
  RUN_TEST(test_mask_non_string_and_null);
  RUN_TEST(test_mask_clean_doc_untouched);
  RUN_TEST(test_mask_node_for_path);
  RUN_TEST(test_mask_node_clean_is_plain);
  RUN_TEST(test_marker_detection);
  RUN_TEST(test_keep_masked_round_trip);
  RUN_TEST(test_keep_masked_wifi_by_ssid);
  RUN_TEST(test_keep_masked_no_stored_drops_marker);
  RUN_TEST(test_keep_masked_real_values_win);
  RUN_TEST(test_set_refuses_marker);
  RUN_TEST(test_saved_reply_masks_secret);
  RUN_TEST(test_config_family_paths);
  RUN_TEST(test_file_read_policy);
  RUN_TEST(test_file_needs_check);
  RUN_TEST(test_set_invalid_key_not_kept);
  RUN_TEST(test_keep_stored_from_file_or_live);
  RUN_TEST(test_keep_stored_copy_failed_refuses);
  return UNITY_END();
}
