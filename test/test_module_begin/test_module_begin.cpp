// Host-native unit tests for module_begin_policy.h (watchdog feeds between module begins).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <vector>
#include "module_begin_policy.h"

static const uint32_t WDT_MS = 30000;

// Simulated task watchdog: trips when an unfed span passes WDT_MS.
struct Sim {
  uint32_t now = 0, lastFeed = 0;
  bool tripped = false;
  int feeds = 0;
  std::vector<int> trace;  // begin index, or -1 for a feed
  void check() { if (now - lastFeed > WDT_MS) tripped = true; }
  void feed() { check(); lastFeed = now; feeds++; trace.push_back(-1); }
  void run(uint32_t ms) { now += ms; check(); }
};

static void runAll(Sim& s, const std::vector<uint32_t>& costs) {
  beginModulesFed((uint8_t)costs.size(),
                  [&](uint8_t i) { s.trace.push_back(i); s.run(costs[i]); },
                  [&]() { s.feed(); });
}

void setUp(void) {}
void tearDown(void) {}

// Three slow modules, each inside the period: no trip.
void test_slow_neighbours_do_not_trip(void) {
  Sim s;
  runAll(s, {20000, 20000, 20000});
  TEST_ASSERT_FALSE(s.tripped);
}

// The boot work before beginAll does not count against the first module.
void test_feed_before_first_module(void) {
  Sim s; s.now = 25000;
  runAll(s, {10000});
  TEST_ASSERT_FALSE(s.tripped);
}

// One feed before the first module and one after each, none inside.
void test_feed_between_every_module(void) {
  Sim s;
  runAll(s, {1, 1, 1});
  TEST_ASSERT_EQUAL_INT(4, s.feeds);
  const std::vector<int> want = {-1, 0, -1, 1, -1, 2, -1};
  TEST_ASSERT_TRUE(want == s.trace);
}

// A module that hangs past the period still trips: no feed rescues it.
void test_hung_module_still_trips(void) {
  Sim s;
  runAll(s, {1000, 40000, 1000});
  TEST_ASSERT_TRUE(s.tripped);
}

void test_no_modules_feeds_once(void) {
  Sim s;
  runAll(s, {});
  TEST_ASSERT_EQUAL_INT(1, s.feeds);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_slow_neighbours_do_not_trip);
  RUN_TEST(test_feed_before_first_module);
  RUN_TEST(test_feed_between_every_module);
  RUN_TEST(test_hung_module_still_trips);
  RUN_TEST(test_no_modules_feeds_once);
  return UNITY_END();
}
