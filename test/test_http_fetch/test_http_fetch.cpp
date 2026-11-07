// Host-native unit tests for http_fetch_policy.h (net.http preview, read
// budget, bounds, job hand-off, modem HTTPS slot claim).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "http_fetch_policy.h"

void setUp(void) {}
void tearDown(void) {}

static uint8_t chunk[2048];

static void fillChunk(void) {
  for (size_t i = 0; i < sizeof(chunk); ++i) chunk[i] = (uint8_t)('a' + i % 26);
}

// --- preview ------------------------------------------------------------------

void test_keep_counts_every_byte_and_stops_copying_at_the_cap(void) {
  fillChunk();
  HttpPreview p = {};
  httpPreviewKeep(p, chunk, 1000);
  TEST_ASSERT_EQUAL_UINT32(1000, p.len);
  httpPreviewKeep(p, chunk + 1000, 1000);
  TEST_ASSERT_EQUAL_UINT32(HTTP_PREVIEW_CAP, p.len);
  TEST_ASSERT_EQUAL_UINT32(2000, p.total);
  TEST_ASSERT_EQUAL_MEMORY(chunk, p.data, HTTP_PREVIEW_CAP);
  httpPreviewKeep(p, chunk, 10);
  TEST_ASSERT_EQUAL_UINT32(HTTP_PREVIEW_CAP, p.len);
  TEST_ASSERT_EQUAL_UINT32(2010, p.total);
  TEST_ASSERT_FALSE(p.cut);
}

void test_budget_holds_the_read_open_inside_it(void) {
  TEST_ASSERT_FALSE(httpReadBudgetSpent(0, HTTP_CELL_READ_MS));
  TEST_ASSERT_FALSE(httpReadBudgetSpent(HTTP_CELL_READ_MS - 1, HTTP_CELL_READ_MS));
}

// #69: a budget in the body sink never ran for a stalled body, which then
// read on for the 15 s idle timeout. The GET loop checks it, bytes or not.
void test_budget_stops_the_read_once_spent(void) {
  TEST_ASSERT_TRUE(httpReadBudgetSpent(HTTP_CELL_READ_MS, HTTP_CELL_READ_MS));
  TEST_ASSERT_TRUE(httpReadBudgetSpent(HTTP_CELL_READ_MS + 15000, HTTP_CELL_READ_MS));
}

// OTA passes no budget: its chunks run to httpsGet's own 5 min cap.
void test_no_budget_never_stops_the_read(void) {
  TEST_ASSERT_FALSE(httpReadBudgetSpent(0, 0));
  TEST_ASSERT_FALSE(httpReadBudgetSpent(5UL * 60UL * 1000UL, 0));
  TEST_ASSERT_FALSE(httpReadBudgetSpent(0xFFFFFFFFUL, 0));
}

// --- bounds -------------------------------------------------------------------

void test_each_read_ends_inside_its_wait(void) {
  TEST_ASSERT_TRUE(HTTP_CELL_READ_MS < HTTP_CELL_WAIT_MS);
  TEST_ASSERT_TRUE(HTTP_WIFI_READ_MS < HTTP_WIFI_WAIT_MS);
}

// --- MQTT tick in the wait ----------------------------------------------------

// Cellular keepalive, the broker's 1.5x grace, the slot-0 write's AT-bus guard.
static const uint32_t CELL_KEEPALIVE_MS = 30000;
static const uint32_t BROKER_GRACE_MS   = CELL_KEEPALIVE_MS * 3 / 2;
static const uint32_t SLOT0_WRITE_MS    = 10000;

// Walks cmd_http's wait in its 20 ms steps. out: ticks, longest gap between them.
static uint32_t walkWait(uint32_t t0, uint32_t waitMs, uint32_t* maxGap) {
  uint32_t ticks = 0, lastTick = t0;
  *maxGap = 0;
  for (uint32_t now = t0; now - t0 < waitMs; now += 20) {
    if (!httpWaitTickDue(lastTick, now)) continue;
    if (now - lastTick > *maxGap) *maxGap = now - lastTick;
    lastTick = now;
    ++ticks;
  }
  return ticks;
}

// #69: the wait never ticked MQTT. The last packet the broker heard can be a
// keepalive old when the command starts, so a held loop passed the grace.
void test_untouched_wait_outlasts_the_broker_grace(void) {
  TEST_ASSERT_TRUE(CELL_KEEPALIVE_MS + HTTP_CELL_WAIT_MS > BROKER_GRACE_MS);
}

void test_wait_ticks_mqtt_once_a_second(void) {
  uint32_t gap   = 0;
  uint32_t ticks = walkWait(0, HTTP_CELL_WAIT_MS, &gap);
  TEST_ASSERT_EQUAL_UINT32(HTTP_CELL_WAIT_MS / HTTP_WAIT_TICK_MS - 1, ticks);
  TEST_ASSERT_EQUAL_UINT32(HTTP_WAIT_TICK_MS, gap);
}

void test_wait_ticks_across_the_millis_wrap(void) {
  uint32_t gap   = 0;
  uint32_t ticks = walkWait(0xFFFFFFFFUL - 4990UL, HTTP_CELL_WAIT_MS, &gap);
  TEST_ASSERT_EQUAL_UINT32(HTTP_CELL_WAIT_MS / HTTP_WAIT_TICK_MS - 1, ticks);
  TEST_ASSERT_EQUAL_UINT32(HTTP_WAIT_TICK_MS, gap);
  TEST_ASSERT_TRUE(httpWaitTickDue(0xFFFFFC18UL, 0));
  TEST_ASSERT_FALSE(httpWaitTickDue(0xFFFFFC19UL, 0));
}

// A ping falls due at the keepalive, goes out by the next tick, and its write
// waits on the bus at most the slot-0 guard: still inside the broker grace.
void test_ticked_ping_lands_inside_the_broker_grace(void) {
  uint32_t gap = 0;
  walkWait(0, HTTP_CELL_WAIT_MS, &gap);
  TEST_ASSERT_TRUE(CELL_KEEPALIVE_MS + gap + SLOT0_WRITE_MS < BROKER_GRACE_MS);
}

// --- job hand-off -------------------------------------------------------------

void test_task_done_first_leaves_the_job_to_the_shell(void) {
  std::atomic<uint32_t> s{HTTP_JOB_RUNNING};
  TEST_ASSERT_FALSE(httpJobFinish(s));
  TEST_ASSERT_EQUAL_UINT32(HTTP_JOB_DONE, s.load());
}

void test_shell_gives_up_first_hands_the_job_to_the_task(void) {
  std::atomic<uint32_t> s{HTTP_JOB_RUNNING};
  TEST_ASSERT_FALSE(httpJobAbandon(s));
  TEST_ASSERT_TRUE(httpJobFinish(s));
}

// The task finished between the shell's last poll and its give-up: the shell
// still owns the job and the task does not free it.
void test_finish_racing_the_give_up_keeps_one_owner(void) {
  std::atomic<uint32_t> s{HTTP_JOB_RUNNING};
  TEST_ASSERT_FALSE(httpJobFinish(s));
  TEST_ASSERT_TRUE(httpJobAbandon(s));
}

// --- slot claim ---------------------------------------------------------------

void test_second_claim_is_refused_while_the_first_holds(void) {
  std::atomic<uint32_t> busy{0};
  HttpSlotClaim first(busy);
  TEST_ASSERT_TRUE(first.held());
  HttpSlotClaim second(busy);
  TEST_ASSERT_FALSE(second.held());
}

void test_claim_frees_the_slot_when_it_ends(void) {
  std::atomic<uint32_t> busy{0};
  {
    HttpSlotClaim first(busy);
    TEST_ASSERT_TRUE(first.held());
    {
      HttpSlotClaim refused(busy);
      TEST_ASSERT_FALSE(refused.held());
    }
    // A refused claim does not free the slot it never held.
    TEST_ASSERT_EQUAL_UINT32(1, busy.load());
  }
  TEST_ASSERT_EQUAL_UINT32(0, busy.load());
  HttpSlotClaim next(busy);
  TEST_ASSERT_TRUE(next.held());
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_keep_counts_every_byte_and_stops_copying_at_the_cap);
  RUN_TEST(test_budget_holds_the_read_open_inside_it);
  RUN_TEST(test_budget_stops_the_read_once_spent);
  RUN_TEST(test_no_budget_never_stops_the_read);
  RUN_TEST(test_each_read_ends_inside_its_wait);
  RUN_TEST(test_untouched_wait_outlasts_the_broker_grace);
  RUN_TEST(test_wait_ticks_mqtt_once_a_second);
  RUN_TEST(test_wait_ticks_across_the_millis_wrap);
  RUN_TEST(test_ticked_ping_lands_inside_the_broker_grace);
  RUN_TEST(test_task_done_first_leaves_the_job_to_the_shell);
  RUN_TEST(test_shell_gives_up_first_hands_the_job_to_the_task);
  RUN_TEST(test_finish_racing_the_give_up_keeps_one_owner);
  RUN_TEST(test_second_claim_is_refused_while_the_first_holds);
  RUN_TEST(test_claim_frees_the_slot_when_it_ends);
  return UNITY_END();
}
