// Host-native unit tests for mqtt_sub_table.h - the slot/count invariant and
// the topic match lifted out of MQTTClient::matchAndDispatch.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_sub_table.h"

using Cb    = void (*)(const char*, const char*);
using Table = MqttSubTable<Cb, 4>;

static const char* g_lastTopic;
static int         g_hitsA;
static int         g_hitsB;

static void hitA(const char* topic, const char*) {
  g_lastTopic = topic;
  g_hitsA++;
}
static void hitB(const char* topic, const char*) {
  g_lastTopic = topic;
  g_hitsB++;
}

void setUp(void) {
  g_lastTopic = nullptr;
  g_hitsA     = 0;
  g_hitsB     = 0;
}
void tearDown(void) {}

// --- the count/active desync regression ----------------------------------

// The bug: begin() cleared every slot's `active` but left the count. Slot 0
// stayed dead and the next add() landed in slot 1. Reproduce the shape a
// reset must NOT have.
void test_reset_clears_count_not_just_active(void) {
  Table t;
  TEST_ASSERT_TRUE(t.add("thesada/node/ota/cmd", hitA));
  TEST_ASSERT_EQUAL_UINT8(1, t.count());

  t.reset();
  TEST_ASSERT_EQUAL_UINT8(0, t.count());

  // Re-registration must reuse slot 0, not skip past it.
  TEST_ASSERT_TRUE(t.add("thesada/node/cli/#", hitB));
  TEST_ASSERT_EQUAL_UINT8(1, t.count());
  t.dispatch("thesada/node/cli/restart", "");
  TEST_ASSERT_EQUAL_INT(1, g_hitsB);
}

// A topic registered before a reset and again after it still dispatches.
// This is exactly what the OTA command topic does across begin().
void test_topic_survives_reregistration_after_reset(void) {
  Table t;
  t.add("thesada/node/ota/cmd", hitA);
  t.reset();
  t.add("thesada/node/cli/#", hitB);
  t.add("thesada/node/ota/cmd", hitA);

  TEST_ASSERT_EQUAL_UINT8(2, t.count());
  TEST_ASSERT_TRUE(t.isRegistered("thesada/node/ota/cmd"));

  t.dispatch("thesada/node/ota/cmd", "");
  TEST_ASSERT_EQUAL_INT(1, g_hitsA);
  TEST_ASSERT_EQUAL_INT(0, g_hitsB);
}

// Slots leaked by a count-only reset would exhaust the table. Registering
// capacity() entries after any number of resets must always fit.
void test_resets_do_not_leak_slots(void) {
  Table t;
  for (int round = 0; round < 5; round++) {
    t.reset();
    for (uint8_t i = 0; i < Table::capacity(); i++) {
      char topic[32];
      (void)snprintf(topic, sizeof(topic), "t/%u", (unsigned)i);
      TEST_ASSERT_TRUE(t.add(topic, hitA));
    }
    TEST_ASSERT_EQUAL_UINT8(Table::capacity(), t.count());
  }
}

void test_add_rejects_when_full(void) {
  Table t;
  for (uint8_t i = 0; i < Table::capacity(); i++) {
    char topic[32];
    (void)snprintf(topic, sizeof(topic), "t/%u", (unsigned)i);
    TEST_ASSERT_TRUE(t.add(topic, hitA));
  }
  TEST_ASSERT_FALSE(t.add("t/overflow", hitA));
  TEST_ASSERT_EQUAL_UINT8(Table::capacity(), t.count());
}

// --- topic matching ------------------------------------------------------

void test_exact_match_only(void) {
  TEST_ASSERT_TRUE(mqttSubMatches("a/b/c", "a/b/c"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/c", "a/b/c/d"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/c", "a/b"));
}

void test_hash_matches_every_level_below(void) {
  TEST_ASSERT_TRUE(mqttSubMatches("a/b/#", "a/b/c"));
  TEST_ASSERT_TRUE(mqttSubMatches("a/b/#", "a/b/c/d/e"));
  TEST_ASSERT_TRUE(mqttSubMatches("a/b/#", "a/b/"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/#", "a/x/c"));
}

void test_plus_matches_exactly_one_level(void) {
  TEST_ASSERT_TRUE(mqttSubMatches("a/b/+", "a/b/c"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/+", "a/b/c/d"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/+", "a/b/"));
  TEST_ASSERT_FALSE(mqttSubMatches("a/b/+", "a/x/c"));
}

// Every matching subscription fires, not just the first.
void test_dispatch_fires_all_matching_slots(void) {
  Table t;
  t.add("a/b/#", hitA);
  t.add("a/b/c", hitB);
  t.dispatch("a/b/c", "payload");
  TEST_ASSERT_EQUAL_INT(1, g_hitsA);
  TEST_ASSERT_EQUAL_INT(1, g_hitsB);
  TEST_ASSERT_EQUAL_STRING("a/b/c", g_lastTopic);
}

void test_dispatch_skips_non_matching(void) {
  Table t;
  t.add("a/b/#", hitA);
  t.dispatch("x/y/z", "");
  TEST_ASSERT_EQUAL_INT(0, g_hitsA);
}

// forEachActive is what replays subscriptions onto the cellular session.
void test_for_each_active_walks_registration_order(void) {
  Table t;
  t.add("one", hitA);
  t.add("two", hitA);
  char seen[64] = {0};
  t.forEachActive([&](const char* topic) {
    strncat(seen, topic, sizeof(seen) - strlen(seen) - 1);
    strncat(seen, ",", sizeof(seen) - strlen(seen) - 1);
  });
  TEST_ASSERT_EQUAL_STRING("one,two,", seen);
}

// A topic longer than the slot is truncated, not overflowed.
void test_long_topic_is_truncated(void) {
  MqttSubTable<Cb, 2, 8> t;
  TEST_ASSERT_TRUE(t.add("abcdefghijkl", hitA));
  TEST_ASSERT_TRUE(t.isRegistered("abcdefg"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_reset_clears_count_not_just_active);
  RUN_TEST(test_topic_survives_reregistration_after_reset);
  RUN_TEST(test_resets_do_not_leak_slots);
  RUN_TEST(test_add_rejects_when_full);
  RUN_TEST(test_exact_match_only);
  RUN_TEST(test_hash_matches_every_level_below);
  RUN_TEST(test_plus_matches_exactly_one_level);
  RUN_TEST(test_dispatch_fires_all_matching_slots);
  RUN_TEST(test_dispatch_skips_non_matching);
  RUN_TEST(test_for_each_active_walks_registration_order);
  RUN_TEST(test_long_topic_is_truncated);
  return UNITY_END();
}
