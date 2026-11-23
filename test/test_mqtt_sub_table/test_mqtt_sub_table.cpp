// Host-native unit tests for mqtt_sub_table.h - the slot/count invariant and
// the topic match lifted out of MQTTClient::matchAndDispatch.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include <string.h>
#include "mqtt_sub_table.h"
#include <functional>
#include <memory>

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

// A topic longer than the slot is refused, not clipped: the clipped filter
// would be resubscribed on every reconnect as a different topic.
void test_long_topic_is_refused_not_clipped(void) {
  MqttSubTable<Cb, 2, 8> t;
  TEST_ASSERT_FALSE(t.add("abcdefgh", hitA));
  TEST_ASSERT_FALSE(t.isRegistered("abcdefg"));
  TEST_ASSERT_EQUAL_UINT8(0, t.count());
  TEST_ASSERT_TRUE(t.add("abcdefg", hitA));
  TEST_ASSERT_TRUE(t.isRegistered("abcdefg"));
}

void test_empty_topic_is_refused(void) {
  Table t;
  TEST_ASSERT_FALSE(t.add("", hitA));
  TEST_ASSERT_FALSE(t.add(nullptr, hitA));
  TEST_ASSERT_EQUAL_UINT8(0, t.count());
}

// --- owners: a Lua reload frees its slots --------------------------------

static int g_unsubs;
static char g_unsubTopic[64];
static void onUnsub(const char* topic) {
  g_unsubs++;
  (void)snprintf(g_unsubTopic, sizeof(g_unsubTopic), "%s", topic);
}

// fw #2: every Lua reload added its subscriptions again, so a few reloads
// filled the table and the script's MQTT subscribe was refused.
void test_reloads_do_not_leak_lua_slots(void) {
  Table t;
  TEST_ASSERT_TRUE(t.add("core/cli/#", hitA));
  for (int reload = 0; reload < 10; reload++) {
    t.removeOwner(MQTT_SUB_OWNER_LUA);
    TEST_ASSERT_TRUE(t.add("lua/a", hitB, MQTT_SUB_OWNER_LUA));
    TEST_ASSERT_TRUE(t.add("lua/b", hitB, MQTT_SUB_OWNER_LUA));
  }
  TEST_ASSERT_EQUAL_UINT8(3, t.count());
}

void test_remove_owner_keeps_other_owners(void) {
  Table t;
  t.add("core/cli/#", hitA);
  t.add("lua/a", hitB, MQTT_SUB_OWNER_LUA);
  t.add("core/ota", hitA);
  TEST_ASSERT_EQUAL_UINT8(1, t.removeOwner(MQTT_SUB_OWNER_LUA));
  t.dispatch("lua/a", "x");
  TEST_ASSERT_EQUAL_INT(0, g_hitsB);
  t.dispatch("core/ota", "x");
  TEST_ASSERT_EQUAL_INT(1, g_hitsA);
  TEST_ASSERT_TRUE(t.isRegistered("core/cli/#"));
  TEST_ASSERT_FALSE(t.isRegistered("lua/a"));
}

// fw #3: reinitSubscriptions emptied the whole table, so Lua subscriptions
// were dead until a reboot. A reinit frees core slots only.
void test_core_reinit_keeps_lua_slots(void) {
  Table t;
  t.add("old/cli/#", hitA);
  t.add("lua/a", hitB, MQTT_SUB_OWNER_LUA);
  t.removeOwner(MQTT_SUB_OWNER_CORE);
  t.add("new/cli/#", hitA);
  t.dispatch("lua/a", "x");
  TEST_ASSERT_EQUAL_INT(1, g_hitsB);
  TEST_ASSERT_FALSE(t.isRegistered("old/cli/#"));
  TEST_ASSERT_TRUE(t.isRegistered("new/cli/#"));
}

// A freed slot in the middle is reused, so a full table takes a new
// subscription after a reload.
void test_freed_slot_is_reused(void) {
  Table t;
  t.add("a", hitA);
  t.add("lua/1", hitB, MQTT_SUB_OWNER_LUA);
  t.add("b", hitA);
  t.add("c", hitA);
  TEST_ASSERT_FALSE(t.add("d", hitA));
  t.removeOwner(MQTT_SUB_OWNER_LUA);
  TEST_ASSERT_TRUE(t.add("d", hitA));
  TEST_ASSERT_EQUAL_UINT8(4, t.count());
  TEST_ASSERT_TRUE(t.isRegistered("d"));
}

// The broker unsubscribe runs once per topic and only when no other slot
// still wants it.
void test_unsubscribe_only_topics_nobody_holds(void) {
  Table t;
  g_unsubs = 0;
  t.add("shared", hitA);
  t.add("shared", hitB, MQTT_SUB_OWNER_LUA);
  t.add("only/lua", hitB, MQTT_SUB_OWNER_LUA);
  t.add("only/lua", hitB, MQTT_SUB_OWNER_LUA);
  TEST_ASSERT_EQUAL_UINT8(3, t.removeOwner(MQTT_SUB_OWNER_LUA, onUnsub));
  TEST_ASSERT_EQUAL_INT(1, g_unsubs);
  TEST_ASSERT_EQUAL_STRING("only/lua", g_unsubTopic);
}

// Trailing freed slots give the count back; reset still empties everything.
void test_trailing_free_slots_shrink_the_count(void) {
  Table t;
  t.add("a", hitA);
  t.add("lua/1", hitB, MQTT_SUB_OWNER_LUA);
  t.add("lua/2", hitB, MQTT_SUB_OWNER_LUA);
  t.removeOwner(MQTT_SUB_OWNER_LUA);
  TEST_ASSERT_EQUAL_UINT8(1, t.count());
  t.reset();
  TEST_ASSERT_EQUAL_UINT8(0, t.count());
}

// A callback that frees its owner's slots and subscribes again (a Lua reload
// inside an MQTT callback) runs to the end on its own captures. The new
// registration takes a different slot, and the old one is swept after the call.
using FnTable = MqttSubTable<std::function<void(const char*, const char*)>, 4>;
static FnTable*           g_fnTable;
static std::weak_ptr<int> g_capture;
static bool               g_captureAliveAfterReAdd;
static uint8_t            g_countInCall;

void test_callback_that_resubscribes_keeps_its_captures(void) {
  FnTable t;
  g_fnTable = &t;
  g_captureAliveAfterReAdd = false;
  auto owned = std::make_shared<int>(7);
  g_capture  = owned;
  t.add("lua/reload", [owned](const char*, const char*) {
    g_fnTable->removeOwner(MQTT_SUB_OWNER_LUA);
    g_fnTable->add("lua/reload", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
    g_captureAliveAfterReAdd = !g_capture.expired();
    g_countInCall            = g_fnTable->count();
  }, MQTT_SUB_OWNER_LUA);
  owned.reset();
  TEST_ASSERT_FALSE(g_capture.expired());
  t.dispatch("lua/reload", "");
  TEST_ASSERT_TRUE(g_captureAliveAfterReAdd);
  TEST_ASSERT_EQUAL_UINT8(2, g_countInCall);
  TEST_ASSERT_TRUE(g_capture.expired());
  TEST_ASSERT_TRUE(t.isRegistered("lua/reload"));
  // Slot 0 is free again: the next add takes it and the count stays.
  TEST_ASSERT_TRUE(t.add("next", [](const char*, const char*) {}));
  TEST_ASSERT_EQUAL_UINT8(2, t.count());
}

// A mutable callback keeps its state from one delivery to the next.
void test_callback_keeps_its_state_between_deliveries(void) {
  FnTable t;
  int seen = 0;
  t.add("state", [n = 0, &seen](const char*, const char*) mutable { seen = ++n; });
  t.dispatch("state", "");
  TEST_ASSERT_EQUAL_INT(1, seen);
  t.dispatch("state", "");
  TEST_ASSERT_EQUAL_INT(2, seen);
}

// Counts live copies of itself, so a test sees when the table destroys one.
struct Counted {
  static int live;
  int*       hits;
  explicit Counted(int* h) : hits(h) { live++; }
  Counted(const Counted& o) : hits(o.hits) { live++; }
  ~Counted() { live--; }
  Counted& operator=(const Counted&) = delete;
  void     operator()(const char*, const char*) const { (*hits)++; }
};
int Counted::live = 0;

static FnTable* g_t;
static int      g_hitsC;

// A remove inside dispatch leaves the slot a zombie until the dispatch returns.
void test_remove_inside_dispatch_is_swept_after_it(void) {
  FnTable t;
  g_t     = &t;
  g_hitsC = 0;
  uint8_t countInCall = 0;
  bool    stillHeld   = true;
  t.add("lua/a", [](const char*, const char*) { g_hitsC++; }, MQTT_SUB_OWNER_LUA);
  t.add("core/x", [&](const char*, const char*) {
    g_t->removeOwner(MQTT_SUB_OWNER_LUA);
    countInCall = g_t->count();
    stillHeld   = g_t->isRegistered("lua/a");
  });
  t.dispatch("core/x", "");
  TEST_ASSERT_EQUAL_UINT8(2, countInCall);
  TEST_ASSERT_FALSE(stillHeld);
  TEST_ASSERT_EQUAL_UINT8(2, t.count());
  t.dispatch("lua/a", "");
  TEST_ASSERT_EQUAL_INT(0, g_hitsC);
  // The swept zombie is free: a new add lands in slot 0, the count stays.
  TEST_ASSERT_TRUE(t.add("lua/b", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA));
  TEST_ASSERT_EQUAL_UINT8(2, t.count());
}

// A dispatch from inside a callback does not sweep: only the outer one does.
void test_nested_dispatch_sweeps_once_at_depth_zero(void) {
  FnTable t;
  g_t = &t;
  int     liveAfterInner = -1;
  int     hits           = 0;
  Counted::live          = 0;
  t.add("lua/a", Counted(&hits), MQTT_SUB_OWNER_LUA);
  t.add("inner", [](const char*, const char*) { g_t->removeOwner(MQTT_SUB_OWNER_LUA); });
  t.add("outer", [&](const char*, const char*) {
    g_t->dispatch("inner", "");
    liveAfterInner = Counted::live;
  });
  TEST_ASSERT_EQUAL_INT(1, Counted::live);
  t.dispatch("outer", "");
  TEST_ASSERT_EQUAL_INT(1, liveAfterInner);
  TEST_ASSERT_EQUAL_INT(0, Counted::live);
}

// add() inside a dispatch takes a free slot below the count, never a zombie:
// a zombie can hold the callback still running.
void test_add_inside_dispatch_takes_a_free_slot_not_a_zombie(void) {
  FnTable t;
  g_t     = &t;
  g_hitsC = 0;
  t.add("lua/a", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
  t.add("free/me", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
  t.add("core/x", [](const char*, const char*) {
    g_t->removeOwner(MQTT_SUB_OWNER_LUA);
    g_t->add("lua/new", [](const char*, const char*) { g_hitsC++; }, MQTT_SUB_OWNER_LUA);
  });
  t.add("hold", [](const char*, const char*) {});
  // Free slot 1, keep lua/a in slot 0: the dispatch turns slot 0 into a zombie.
  t.removeOwner(MQTT_SUB_OWNER_LUA);
  t.add("lua/a", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
  TEST_ASSERT_EQUAL_UINT8(4, t.count());
  t.dispatch("core/x", "");
  TEST_ASSERT_TRUE(t.isRegistered("lua/new"));
  TEST_ASSERT_FALSE(t.isRegistered("lua/a"));
  t.dispatch("lua/new", "");
  TEST_ASSERT_EQUAL_INT(1, g_hitsC);
  // Slot 0 is free after the sweep, and the table is full once it is taken.
  TEST_ASSERT_TRUE(t.add("last", [](const char*, const char*) {}));
  TEST_ASSERT_FALSE(t.add("over", [](const char*, const char*) {}));
}

// The count covers zombies until the outermost dispatch returns.
void test_count_holds_until_the_outer_dispatch_returns(void) {
  FnTable t;
  g_t = &t;
  uint8_t inner = 0;
  t.add("core/x", [&](const char*, const char*) {
    g_t->removeOwner(MQTT_SUB_OWNER_LUA);
    inner = g_t->count();
  });
  t.add("lua/a", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
  t.add("lua/b", [](const char*, const char*) {}, MQTT_SUB_OWNER_LUA);
  t.dispatch("core/x", "");
  TEST_ASSERT_EQUAL_UINT8(3, inner);
  TEST_ASSERT_EQUAL_UINT8(1, t.count());
}

// A removed callback is destroyed at the sweep, not later and not before.
void test_removed_callback_is_destroyed_at_sweep(void) {
  Counted::live = 0;
  int hits      = 0;
  {
    FnTable t;
    t.add("lua/a", Counted(&hits), MQTT_SUB_OWNER_LUA);
    TEST_ASSERT_EQUAL_INT(1, Counted::live);
    t.dispatch("lua/a", "");
    TEST_ASSERT_EQUAL_INT(1, hits);
    t.removeOwner(MQTT_SUB_OWNER_LUA);
    TEST_ASSERT_EQUAL_INT(0, Counted::live);
    t.add("lua/b", Counted(&hits), MQTT_SUB_OWNER_LUA);
    t.reset();
    TEST_ASSERT_EQUAL_INT(0, Counted::live);
    t.add("lua/c", Counted(&hits), MQTT_SUB_OWNER_LUA);
  }
  TEST_ASSERT_EQUAL_INT(0, Counted::live);
}

// The function-pointer table takes the same path.
static Table* g_fpTable;
static void reAddSelf(const char*, const char*) {
  g_fpTable->removeOwner(MQTT_SUB_OWNER_LUA);
  g_fpTable->add("self", hitA, MQTT_SUB_OWNER_LUA);
}
void test_fn_pointer_callback_replaces_itself(void) {
  Table t;
  g_fpTable = &t;
  t.add("self", reAddSelf, MQTT_SUB_OWNER_LUA);
  t.dispatch("self", "");
  TEST_ASSERT_EQUAL_INT(0, g_hitsA);
  t.dispatch("self", "");
  TEST_ASSERT_EQUAL_INT(1, g_hitsA);
  // The new registration sits in slot 1, the swept slot 0 is free below it.
  TEST_ASSERT_EQUAL_UINT8(2, t.count());
  TEST_ASSERT_TRUE(t.add("next", hitB));
  TEST_ASSERT_EQUAL_UINT8(2, t.count());
}

// The std::function table the client uses unsubscribes the same way.
void test_fn_table_unsubscribes_only_unheld_topics(void) {
  FnTable t;
  g_unsubs  = 0;
  auto noop = [](const char*, const char*) {};
  t.add("shared", noop);
  t.add("shared", noop, MQTT_SUB_OWNER_LUA);
  t.add("only/lua", noop, MQTT_SUB_OWNER_LUA);
  TEST_ASSERT_EQUAL_UINT8(2, t.removeOwner(MQTT_SUB_OWNER_LUA, onUnsub));
  TEST_ASSERT_EQUAL_INT(1, g_unsubs);
  TEST_ASSERT_EQUAL_STRING("only/lua", g_unsubTopic);
  TEST_ASSERT_TRUE(t.isRegistered("shared"));
  TEST_ASSERT_FALSE(t.isRegistered("only/lua"));
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
  RUN_TEST(test_long_topic_is_refused_not_clipped);
  RUN_TEST(test_empty_topic_is_refused);
  RUN_TEST(test_reloads_do_not_leak_lua_slots);
  RUN_TEST(test_remove_owner_keeps_other_owners);
  RUN_TEST(test_core_reinit_keeps_lua_slots);
  RUN_TEST(test_freed_slot_is_reused);
  RUN_TEST(test_unsubscribe_only_topics_nobody_holds);
  RUN_TEST(test_trailing_free_slots_shrink_the_count);
  RUN_TEST(test_callback_that_resubscribes_keeps_its_captures);
  RUN_TEST(test_callback_keeps_its_state_between_deliveries);
  RUN_TEST(test_fn_table_unsubscribes_only_unheld_topics);
  RUN_TEST(test_remove_inside_dispatch_is_swept_after_it);
  RUN_TEST(test_nested_dispatch_sweeps_once_at_depth_zero);
  RUN_TEST(test_add_inside_dispatch_takes_a_free_slot_not_a_zombie);
  RUN_TEST(test_count_holds_until_the_outer_dispatch_returns);
  RUN_TEST(test_removed_callback_is_destroyed_at_sweep);
  RUN_TEST(test_fn_pointer_callback_replaces_itself);
  return UNITY_END();
}
