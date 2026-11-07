// Host-native unit tests for secret_heap.h (key-bearing heap copies).
// secretWipe is a recording double: it keeps what each wipe covered.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include <utility>
#include "secret_heap.h"

static const size_t SEEN_CAP = 256;
static unsigned char g_seen[SEEN_CAP];
static size_t g_seenLen = 0;
static size_t g_wipes = 0;
static size_t g_wipedBytes = 0;

void secretWipe(void* p, size_t n) noexcept {
  g_seenLen = n < SEEN_CAP ? n : SEEN_CAP;
  memcpy(g_seen, p, g_seenLen);
  memset(p, 0, n);
  g_wipes++;
  g_wipedBytes += n;
}

void setUp(void) {
  g_seenLen = 0;
  g_wipes = 0;
  g_wipedBytes = 0;
}
void tearDown(void) {}

static const char KEY[] = "-----BEGIN EC PRIVATE KEY-----MHcCAQEE";
static const size_t KEY_LEN = sizeof(KEY) - 1;

static bool seenEndsWith(const char* s, size_t n) {
  return g_seenLen >= n && memcmp(g_seen + g_seenLen - n, s, n) == 0;
}

// The freed block, capacity and header, was wiped while it still held the key.
void test_free_wipes_the_whole_block(void) {
  char* p = static_cast<char*>(secretAlloc(KEY_LEN));
  TEST_ASSERT_NOT_NULL(p);
  memcpy(p, KEY, KEY_LEN);
  secretFree(p);
  TEST_ASSERT_EQUAL_UINT(1, g_wipes);
  TEST_ASSERT_EQUAL_UINT(SECRET_BLOCK_HDR + KEY_LEN, g_wipedBytes);
  TEST_ASSERT_TRUE(seenEndsWith(KEY, KEY_LEN));
}

void test_free_null_is_a_no_op(void) {
  secretFree(nullptr);
  TEST_ASSERT_EQUAL_UINT(0, g_wipes);
}

// A grow leaves no unwiped copy behind: the old block is wiped, the data moves.
void test_grow_wipes_the_old_block(void) {
  char* p = static_cast<char*>(secretAlloc(KEY_LEN));
  TEST_ASSERT_NOT_NULL(p);
  memcpy(p, KEY, KEY_LEN);
  char* q = static_cast<char*>(secretRealloc(p, 4 * KEY_LEN));
  TEST_ASSERT_NOT_NULL(q);
  TEST_ASSERT_EQUAL_UINT(1, g_wipes);
  TEST_ASSERT_TRUE(seenEndsWith(KEY, KEY_LEN));
  TEST_ASSERT_EQUAL_MEMORY(KEY, q, KEY_LEN);
  TEST_ASSERT_EQUAL_UINT(4 * KEY_LEN, secretCapacity(q));
  secretFree(q);
}

// A shrink stays in place, keeps the head, wipes the tail, and cannot fail.
void test_shrink_stays_in_place_and_wipes_the_tail(void) {
  char* p = static_cast<char*>(secretAlloc(KEY_LEN));
  TEST_ASSERT_NOT_NULL(p);
  memcpy(p, KEY, KEY_LEN);
  char* q = static_cast<char*>(secretRealloc(p, 8));
  TEST_ASSERT_EQUAL_PTR(p, q);
  TEST_ASSERT_EQUAL_UINT(KEY_LEN - 8, g_wipedBytes);
  TEST_ASSERT_TRUE(seenEndsWith(KEY + 8, KEY_LEN - 8));
  TEST_ASSERT_EQUAL_MEMORY(KEY, q, 8);
  TEST_ASSERT_EACH_EQUAL_UINT8(0, q + 8, KEY_LEN - 8);
  TEST_ASSERT_EQUAL_UINT(KEY_LEN, secretCapacity(q));  // the free wipes it all
  secretFree(q);
}

void test_realloc_of_null_allocates(void) {
  void* p = secretRealloc(nullptr, 16);
  TEST_ASSERT_NOT_NULL(p);
  TEST_ASSERT_EQUAL_UINT(16, secretCapacity(p));
  secretFree(p);
}

// A size the header cannot fit is refused, and a refused grow keeps p as is.
void test_oversize_refused_and_grow_failure_keeps_block(void) {
  TEST_ASSERT_NULL(secretAlloc(SIZE_MAX));
  char* p = static_cast<char*>(secretAlloc(KEY_LEN));
  TEST_ASSERT_NOT_NULL(p);
  memcpy(p, KEY, KEY_LEN);
  TEST_ASSERT_NULL(secretRealloc(p, SIZE_MAX));
  TEST_ASSERT_EQUAL_UINT(0, g_wipes);
  TEST_ASSERT_EQUAL_MEMORY(KEY, p, KEY_LEN);
  secretFree(p);
}

// A CLI payload copy holding a cert.set key is wiped when it goes away.
void test_secret_string_wipes_on_destruction(void) {
  {
    SecretString s(KEY, KEY_LEN);
    TEST_ASSERT_EQUAL_UINT(KEY_LEN, s.size());
    TEST_ASSERT_FALSE(s.empty());
    TEST_ASSERT_EQUAL_MEMORY(KEY, s.c_str(), KEY_LEN);
  }
  TEST_ASSERT_EQUAL_UINT(1, g_wipes);
  TEST_ASSERT_EQUAL_UINT(KEY_LEN, g_seenLen);
  TEST_ASSERT_EQUAL_MEMORY(KEY, g_seen, KEY_LEN);
}

// std::function copies its target: each copy wipes, a moved-from one holds nothing.
void test_secret_string_copies_and_moves_all_wipe(void) {
  {
    SecretString a(KEY, KEY_LEN);
    SecretString b(a);
    SecretString c(std::move(a));
    TEST_ASSERT_EQUAL_MEMORY(KEY, b.c_str(), KEY_LEN);
    TEST_ASSERT_EQUAL_MEMORY(KEY, c.c_str(), KEY_LEN);
  }
  TEST_ASSERT_TRUE(g_wipedBytes >= 2 * KEY_LEN);
}

void test_secret_string_null_or_empty_wipes_nothing(void) {
  {
    SecretString a(nullptr, 5);
    SecretString b("", 0);
    TEST_ASSERT_TRUE(a.empty());
    TEST_ASSERT_TRUE(b.empty());
    TEST_ASSERT_EQUAL_STRING("", a.c_str());
  }
  TEST_ASSERT_EQUAL_UINT(0, g_wipes);
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_free_wipes_the_whole_block);
  RUN_TEST(test_free_null_is_a_no_op);
  RUN_TEST(test_grow_wipes_the_old_block);
  RUN_TEST(test_shrink_stays_in_place_and_wipes_the_tail);
  RUN_TEST(test_realloc_of_null_allocates);
  RUN_TEST(test_oversize_refused_and_grow_failure_keeps_block);
  RUN_TEST(test_secret_string_wipes_on_destruction);
  RUN_TEST(test_secret_string_copies_and_moves_all_wipe);
  RUN_TEST(test_secret_string_null_or_empty_wipes_nothing);
  return UNITY_END();
}
