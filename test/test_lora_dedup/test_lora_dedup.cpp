// Host-native unit tests for lora_dedup_policy.h (RX packetId dedup ring).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include "lora_dedup_policy.h"

void setUp(void) {}
void tearDown(void) {}

void test_first_seen_passes(void) {
  mesh::DedupRing<4> r;
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 100));
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 101));
}

void test_repeat_drops(void) {
  mesh::DedupRing<4> r;
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 100));
  TEST_ASSERT_TRUE(r.seenAndRecord(0xA1, 100));
  TEST_ASSERT_TRUE(r.seenAndRecord(0xA1, 100));  // every retry, not just first
}

void test_same_id_different_sender_passes(void) {
  mesh::DedupRing<4> r;
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 100));
  TEST_ASSERT_FALSE(r.seenAndRecord(0xB2, 100));
  TEST_ASSERT_TRUE(r.seenAndRecord(0xB2, 100));
}

void test_fifo_eviction_reopens_old_id(void) {
  mesh::DedupRing<4> r;
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 1));
  for (uint32_t pid = 2; pid <= 5; pid++) {
    TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, pid));  // 4 new entries evict pid 1
  }
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 1));  // evicted = no longer a dup
  TEST_ASSERT_TRUE(r.seenAndRecord(0xA1, 5));   // recent one still tracked
}

void test_pid_zero_never_drops(void) {
  mesh::DedupRing<4> r;
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 0));
  TEST_ASSERT_FALSE(r.seenAndRecord(0xA1, 0));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_first_seen_passes);
  RUN_TEST(test_repeat_drops);
  RUN_TEST(test_same_id_different_sender_passes);
  RUN_TEST(test_fifo_eviction_reopens_old_id);
  RUN_TEST(test_pid_zero_never_drops);
  return UNITY_END();
}
