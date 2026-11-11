// Host-native unit tests for event_bus_table.h (owner-tagged EventBus entries).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <functional>
#include "event_bus_table.h"

using Cb    = std::function<void(int)>;
using Table = EventBusTable<Cb>;

static int g_core;
static int g_lua;

void setUp(void) {
  g_core = 0;
  g_lua  = 0;
}
void tearDown(void) {}

void test_publish_reaches_every_subscriber(void) {
  Table t;
  t.subscribe("temperature", [](int v) { g_core += v; });
  t.subscribe("temperature", [](int v) { g_lua += v; }, EVENT_OWNER_LUA);
  t.subscribe("other", [](int) { g_core += 100; });
  t.publish("temperature", 2);
  TEST_ASSERT_EQUAL_INT(2, g_core);
  TEST_ASSERT_EQUAL_INT(2, g_lua);
  t.publish("missing", 1);
}

// fw #3: every Lua reload left its EventBus callbacks behind, so the list
// grew by the script's subscriptions on each reload.
void test_reloads_do_not_accumulate_lua_entries(void) {
  Table t;
  t.subscribe("temperature", [](int) { g_core++; });
  for (int reload = 0; reload < 10; reload++) {
    t.removeOwner(EVENT_OWNER_LUA);
    t.subscribe("temperature", [](int) { g_lua++; }, EVENT_OWNER_LUA);
    t.subscribe("alert", [](int) { g_lua++; }, EVENT_OWNER_LUA);
  }
  TEST_ASSERT_EQUAL_UINT32(3, t.size());
  t.publish("temperature", 0);
  TEST_ASSERT_EQUAL_INT(1, g_core);
  TEST_ASSERT_EQUAL_INT(1, g_lua);
}

void test_remove_owner_keeps_core(void) {
  Table t;
  t.subscribe("e", [](int) { g_core++; });
  t.subscribe("e", [](int) { g_lua++; }, EVENT_OWNER_LUA);
  TEST_ASSERT_EQUAL_UINT32(1, t.removeOwner(EVENT_OWNER_LUA));
  t.publish("e", 0);
  TEST_ASSERT_EQUAL_INT(1, g_core);
  TEST_ASSERT_EQUAL_INT(0, g_lua);
  TEST_ASSERT_EQUAL_UINT32(0, t.removeOwner(EVENT_OWNER_LUA));
}

// A reload that runs inside a callback drops the Lua entries still to come in
// that same publish, and the sweep waits for the publish to return.
static Table* g_table;
void test_remove_inside_publish_skips_dropped_entries(void) {
  Table t;
  g_table = &t;
  t.subscribe("e", [](int) { g_core++; g_table->removeOwner(EVENT_OWNER_LUA); });
  t.subscribe("e", [](int) { g_lua++; }, EVENT_OWNER_LUA);
  t.publish("e", 0);
  TEST_ASSERT_EQUAL_INT(1, g_core);
  TEST_ASSERT_EQUAL_INT(0, g_lua);
  TEST_ASSERT_EQUAL_UINT32(1, t.size());
}

// The reload re-subscribes inside the same callback: the vector may grow
// under the loop without a dangling call.
void test_subscribe_inside_publish_is_safe(void) {
  Table t;
  g_table = &t;
  t.subscribe("e", [](int) {
    g_core++;
    g_table->removeOwner(EVENT_OWNER_LUA);
    for (int i = 0; i < 32; i++) g_table->subscribe("e", [](int) { g_lua++; }, EVENT_OWNER_LUA);
  });
  t.subscribe("e", [](int) { g_lua += 1000; }, EVENT_OWNER_LUA);
  t.publish("e", 0);
  TEST_ASSERT_EQUAL_INT(1, g_core);
  TEST_ASSERT_EQUAL_INT(32, g_lua);
  TEST_ASSERT_EQUAL_UINT32(33, t.size());
}

// CR #126: a callback that subscribes itself on every call made publish run
// until memory ran out. One publish now stops at its start count plus a
// margin, and the event stops taking callbacks at its cap.
static int g_self_calls;
static void selfSub(int) {
  g_self_calls++;
  if (g_self_calls > 100000) return;
  g_table->subscribe("e", selfSub, EVENT_OWNER_LUA);
}
void test_self_subscribing_callback_is_bounded(void) {
  Table t;
  g_table      = &t;
  g_self_calls = 0;
  t.subscribe("e", selfSub, EVENT_OWNER_LUA);
  t.publish("e", 0);
  TEST_ASSERT_EQUAL_INT(1 + EVENT_PUBLISH_MARGIN, g_self_calls);
  // Later publishes stop growing at the per-event cap.
  for (int i = 0; i < 10; i++) t.publish("e", 0);
  TEST_ASSERT_EQUAL_UINT32(EVENT_SUBS_MAX, t.size());
  g_self_calls = 0;
  t.publish("e", 0);
  TEST_ASSERT_EQUAL_INT((int)EVENT_SUBS_MAX, g_self_calls);
}

// The cap counts live entries per event: dropped ones free their room, and
// another event has its own.
void test_subscribe_cap_counts_live_entries(void) {
  Table t;
  for (size_t i = 0; i < EVENT_SUBS_MAX; i++) {
    TEST_ASSERT_TRUE(t.subscribe("e", [](int) {}, EVENT_OWNER_LUA));
  }
  TEST_ASSERT_FALSE(t.subscribe("e", [](int) {}));
  TEST_ASSERT_TRUE(t.subscribe("other", [](int) {}));
  t.removeOwner(EVENT_OWNER_LUA);
  TEST_ASSERT_TRUE(t.subscribe("e", [](int) {}));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_publish_reaches_every_subscriber);
  RUN_TEST(test_reloads_do_not_accumulate_lua_entries);
  RUN_TEST(test_remove_owner_keeps_core);
  RUN_TEST(test_remove_inside_publish_skips_dropped_entries);
  RUN_TEST(test_subscribe_inside_publish_is_safe);
  RUN_TEST(test_self_subscribing_callback_is_bounded);
  RUN_TEST(test_subscribe_cap_counts_live_entries);
  return UNITY_END();
}
