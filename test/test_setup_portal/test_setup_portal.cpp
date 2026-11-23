// Host-native unit tests for setup_portal_policy.h.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "setup_portal_policy.h"
#include <stdlib.h>
#include <string>

void setUp(void) {}
void tearDown(void) {}

void test_portal_opens_only_on_the_ap_without_a_station(void) {
  TEST_ASSERT_TRUE(setupPortalOpen(true, 0, false));
  TEST_ASSERT_FALSE(setupPortalOpen(false, 0, false));
  TEST_ASSERT_FALSE(setupPortalOpen(true, 1, false));
  TEST_ASSERT_TRUE(setupPortalOpen(true, 1, true));
  TEST_ASSERT_FALSE(setupPortalOpen(false, 0, true));
}

void test_ssid_is_one_to_32_bytes_without_controls(void) {
  TEST_ASSERT_TRUE(setupSsidUsable("HomeNet"));
  TEST_ASSERT_TRUE(setupSsidUsable("a b"));
  TEST_ASSERT_TRUE(setupSsidUsable("bad<ssid>"));
  TEST_ASSERT_TRUE(setupSsidUsable("say \"hi\""));
  TEST_ASSERT_TRUE(setupSsidUsable("caf\xc3\xa9"));
  TEST_ASSERT_FALSE(setupSsidUsable(""));
  TEST_ASSERT_FALSE(setupSsidUsable(nullptr));
  TEST_ASSERT_FALSE(setupSsidUsable("has\nline"));
  char longSsid[34];
  memset(longSsid, 'a', 33);
  longSsid[33] = '\0';
  TEST_ASSERT_FALSE(setupSsidUsable(longSsid));
  longSsid[32] = '\0';
  TEST_ASSERT_TRUE(setupSsidUsable(longSsid));
}

void test_passphrase_is_8_to_63_printable(void) {
  TEST_ASSERT_TRUE(setupPassUsable("12345678"));
  TEST_ASSERT_FALSE(setupPassUsable("short"));
  TEST_ASSERT_FALSE(setupPassUsable(nullptr));
  char pass63[64];
  memset(pass63, 'p', 63);
  pass63[63] = '\0';
  TEST_ASSERT_TRUE(setupPassUsable(pass63));
  char pass64[65];
  memset(pass64, 'p', 64);
  pass64[64] = '\0';
  TEST_ASSERT_FALSE(setupPassUsable(pass64));
}

void test_form_needs_an_https_url(void) {
  TEST_ASSERT_TRUE(setupFormUsable("HomeNet", "12345678", "https://thesada.app"));
  TEST_ASSERT_FALSE(setupFormUsable("HomeNet", "12345678", "http://thesada.app"));
  TEST_ASSERT_FALSE(setupFormUsable("HomeNet", "short", "https://thesada.app"));
}

// An allocator that refuses every allocation while fail is set.
struct NoHeap : ArduinoJson::Allocator {
  bool fail = true;
  void* allocate(size_t n) override { return fail ? nullptr : malloc(n); }
  void deallocate(void* p) override { free(p); }
  void* reallocate(void* p, size_t n) override { return fail ? nullptr : realloc(p, n); }
};

static std::string json(JsonDocument& doc) {
  std::string s;
  serializeJson(doc, s);
  return s;
}

// The form replaces the network list and sets the enroll URL.
void test_form_writes_the_network_and_url(void) {
  JsonDocument doc;
  deserializeJson(doc, "{\"wifi\":{\"networks\":[{\"ssid\":\"old\"}],\"enabled\":true},\"mqtt\":{}}");
  TEST_ASSERT_TRUE(setupFormToConfig(doc.as<JsonObject>(), "home", "https://thesada.app"));
  TEST_ASSERT_EQUAL_STRING(
      "{\"wifi\":{\"networks\":[{\"ssid\":\"home\"}],\"enabled\":true},\"mqtt\":{},"
      "\"enroll\":{\"url\":\"https://thesada.app\"}}",
      json(doc).c_str());
}

// wifi or enroll that is not an object is replaced, never a silent no-op.
void test_form_replaces_a_scalar_wifi(void) {
  const char* texts[] = {"{\"wifi\":false}", "{\"wifi\":{\"networks\":5}}", "{\"enroll\":\"x\"}", "{}"};
  for (const char* text : texts) {
    JsonDocument doc;
    deserializeJson(doc, text);
    TEST_ASSERT_TRUE_MESSAGE(setupFormToConfig(doc.as<JsonObject>(), "home", "https://thesada.app"), text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("home", doc["wifi"]["networks"][0]["ssid"] | "", text);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("https://thesada.app", doc["enroll"]["url"] | "", text);
  }
}

// No heap for the network: false, so the portal reports the save error.
void test_form_out_of_heap_is_false(void) {
  NoHeap heap;
  heap.fail = false;
  JsonDocument doc(&heap);
  deserializeJson(doc, "{}");
  heap.fail = true;
  TEST_ASSERT_FALSE(setupFormToConfig(doc.as<JsonObject>(), "home-network", "https://thesada.app"));
  TEST_ASSERT_FALSE(setupFormToConfig(JsonObject(), "home", "https://thesada.app"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_portal_opens_only_on_the_ap_without_a_station);
  RUN_TEST(test_ssid_is_one_to_32_bytes_without_controls);
  RUN_TEST(test_passphrase_is_8_to_63_printable);
  RUN_TEST(test_form_needs_an_https_url);
  RUN_TEST(test_form_writes_the_network_and_url);
  RUN_TEST(test_form_replaces_a_scalar_wifi);
  RUN_TEST(test_form_out_of_heap_is_false);
  return UNITY_END();
}
