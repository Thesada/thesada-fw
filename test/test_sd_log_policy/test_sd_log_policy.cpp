// Host-native unit tests for sd_log_policy.h (SD log slot pick from one listing).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <stdio.h>
#include "sd_log_policy.h"

void setUp(void) {}
void tearDown(void) {}

static void addRange(SdLogSlots& s, int from, int to) {
  char name[20];
  for (int i = from; i <= to; i++) {
    TEST_ASSERT_EQUAL_INT(11, snprintf(name, sizeof(name), "/log%03d.csv", i));
    sdLogSlotsAdd(s, name);
  }
}

// --- sdLogIndex ---------------------------------------------------------------

void test_index_parses_log_names(void) {
  TEST_ASSERT_EQUAL_INT(1,   sdLogIndex("/log001.csv"));
  TEST_ASSERT_EQUAL_INT(694, sdLogIndex("/log694.csv"));
  TEST_ASSERT_EQUAL_INT(999, sdLogIndex("log999.csv"));
}

// FAT matches names case-insensitively, so exists() saw these too.
void test_index_is_case_insensitive(void) {
  TEST_ASSERT_EQUAL_INT(42, sdLogIndex("/LOG042.CSV"));
  TEST_ASSERT_EQUAL_INT(42, sdLogIndex("/Log042.Csv"));
}

void test_index_rejects_garbage(void) {
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex(nullptr));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex(""));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log000.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log1.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log0001.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log12a.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log123.txt"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log123.csv.bak"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/xlog123.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/lag123.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log123xcsv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/config.json"));
}

// The listing returns full paths; only the basename counts.
void test_index_uses_basename(void) {
  TEST_ASSERT_EQUAL_INT(7, sdLogIndex("/sdcard/log007.csv"));
  TEST_ASSERT_EQUAL_INT(0, sdLogIndex("/log007.csv/"));
}

// --- SdLogSlots ---------------------------------------------------------------

void test_empty_card_has_no_highest_and_slot_one_free(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(1, sdLogSlotsNextFree(s, 1));
}

// The bench card: log001..log694 present.
void test_694_files_picks_log694(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  addRange(s, 1, 694);
  TEST_ASSERT_EQUAL_INT(694, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(695, sdLogSlotsNextFree(s, 1));
}

// Highest wins for resume; the lowest gap is the next fresh slot, as the
// old ascending exists() probe picked.
void test_gaps_resume_highest_open_lowest_gap(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  addRange(s, 1, 3);
  addRange(s, 10, 12);
  sdLogSlotsAdd(s, "/log500.csv");
  TEST_ASSERT_EQUAL_INT(500, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(4, sdLogSlotsNextFree(s, 1));
  TEST_ASSERT_EQUAL_INT(5, sdLogSlotsNextFree(s, 5));
  TEST_ASSERT_EQUAL_INT(13, sdLogSlotsNextFree(s, 10));
  TEST_ASSERT_EQUAL_INT(501, sdLogSlotsNextFree(s, 500));
}

void test_garbage_names_do_not_take_slots(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  sdLogSlotsAdd(s, "/config.json");
  sdLogSlotsAdd(s, "/log000.csv");
  sdLogSlotsAdd(s, "/log1000.csv");
  sdLogSlotsAdd(s, "/System Volume Information");
  sdLogSlotsAdd(s, nullptr);
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(1, sdLogSlotsNextFree(s, 1));
  sdLogSlotsAdd(s, "/log002.csv");
  TEST_ASSERT_EQUAL_INT(2, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(1, sdLogSlotsNextFree(s, 1));
}

// A full set: nothing free, no wrap back to log001.
void test_full_set_has_no_free_slot(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  addRange(s, 1, 999);
  TEST_ASSERT_EQUAL_INT(999, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsNextFree(s, 1));
}

void test_only_last_slot_and_bounds(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  sdLogSlotsAdd(s, "/log999.csv");
  TEST_ASSERT_EQUAL_INT(999, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(1, sdLogSlotsNextFree(s, 0));
  TEST_ASSERT_EQUAL_INT(998, sdLogSlotsNextFree(s, 998));
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsNextFree(s, 999));
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsNextFree(s, 1000));
  sdLogSlotsAdd(s, "/log999.csv");
  TEST_ASSERT_EQUAL_INT(999, sdLogSlotsHighest(s));
}

// A directory named like a log holds its slot but is never resumed.
void test_dir_holds_slot_but_is_not_resumed(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  sdLogSlotsAdd(s, "/log005.csv");
  sdLogSlotsAdd(s, "/log999.csv", true);
  TEST_ASSERT_EQUAL_INT(5, sdLogSlotsHighest(s));
  TEST_ASSERT_TRUE(sdLogSlotsHas(s, 999));
  TEST_ASSERT_EQUAL_INT(6, sdLogSlotsNextFree(s, 5));
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsNextFree(s, 999));
}

// Only directories: nothing to resume, the first free slot opens.
void test_only_dirs_resume_nothing(void) {
  SdLogSlots s;
  sdLogSlotsClear(s);
  sdLogSlotsAdd(s, "/log001.csv", true);
  sdLogSlotsAdd(s, "/log002.csv", true);
  TEST_ASSERT_EQUAL_INT(0, sdLogSlotsHighest(s));
  TEST_ASSERT_EQUAL_INT(3, sdLogSlotsNextFree(s, 1));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_index_parses_log_names);
  RUN_TEST(test_index_is_case_insensitive);
  RUN_TEST(test_index_rejects_garbage);
  RUN_TEST(test_index_uses_basename);
  RUN_TEST(test_empty_card_has_no_highest_and_slot_one_free);
  RUN_TEST(test_694_files_picks_log694);
  RUN_TEST(test_gaps_resume_highest_open_lowest_gap);
  RUN_TEST(test_garbage_names_do_not_take_slots);
  RUN_TEST(test_full_set_has_no_free_slot);
  RUN_TEST(test_only_last_slot_and_bounds);
  RUN_TEST(test_dir_holds_slot_but_is_not_resumed);
  RUN_TEST(test_only_dirs_resume_nothing);
  return UNITY_END();
}
