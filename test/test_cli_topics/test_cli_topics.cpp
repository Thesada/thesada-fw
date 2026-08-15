// Host-native unit tests for cli_topics.h (CLI topic construction + the
// non-match property the topic split exists to create).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "cli_topics.h"

void setUp(void) {}
void tearDown(void) {}

static const char* kPrefix = "thesada/acme/owb";

// The whole reason for the split: a device subscribed to the input wildcard
// must not receive its own responses.
void test_response_topic_does_not_match_input_subscription(void) {
  char sub[64], resp[64];
  TEST_ASSERT_TRUE(cliInputSubscription(sub, sizeof(sub), kPrefix));
  TEST_ASSERT_TRUE(cliResponseTopic(resp, sizeof(resp), kPrefix));
  TEST_ASSERT_FALSE(cliTopicMatches(sub, resp));
}

// The pre-split topic DID match, which is why it had to move. Kept as a
// literal: if this stops matching, the test above stops proving anything.
void test_legacy_response_topic_did_match(void) {
  char sub[CLI_TOPIC_CAP];
  TEST_ASSERT_TRUE(cliInputSubscription(sub, sizeof(sub), kPrefix));
  TEST_ASSERT_TRUE(cliTopicMatches(sub, "thesada/acme/owb/cli/response"));
}

// Commands must stay inside the wildcard, or they stop arriving - the
// failure mode opposite to the echo.
void test_command_topics_match_input_subscription(void) {
  char sub[64], cmd[64];
  TEST_ASSERT_TRUE(cliInputSubscription(sub, sizeof(sub), kPrefix));
  const char* cmds[] = {"fs.cat", "restart", "ota.check", "config.dump"};
  for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    TEST_ASSERT_TRUE(cliCommandTopic(cmd, sizeof(cmd), kPrefix, cmds[i]));
    TEST_ASSERT_TRUE(cliTopicMatches(sub, cmd));
  }
}

void test_exact_topic_strings(void) {
  char buf[64];
  TEST_ASSERT_TRUE(cliInputSubscription(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/#", buf);
  TEST_ASSERT_TRUE(cliCommandTopic(buf, sizeof(buf), kPrefix, "fs.cat"));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/fs.cat", buf);
  TEST_ASSERT_TRUE(cliResponseTopic(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli_response", buf);
  TEST_ASSERT_TRUE(cliInputPrefix(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/", buf);
}

// A truncated topic would publish to the wrong place, so truncation must be
// reported rather than silently clipped.
void test_truncation_is_reported(void) {
  char tiny[8];
  TEST_ASSERT_FALSE(cliResponseTopic(tiny, sizeof(tiny), kPrefix));
  TEST_ASSERT_FALSE(cliCommandTopic(tiny, sizeof(tiny), kPrefix, "fs.cat"));
  TEST_ASSERT_FALSE(cliInputSubscription(tiny, sizeof(tiny), kPrefix));
  TEST_ASSERT_FALSE(cliInputPrefix(tiny, sizeof(tiny), kPrefix));
  // Exact fit is not truncation: prefix + suffix + NUL.
  char exact[sizeof("thesada/acme/owb/cli_response")];
  TEST_ASSERT_TRUE(cliResponseTopic(exact, sizeof(exact), kPrefix));
  // One byte short is.
  char shortBuf[sizeof("thesada/acme/owb/cli_response") - 1];
  TEST_ASSERT_FALSE(cliResponseTopic(shortBuf, sizeof(shortBuf), kPrefix));
}

void test_null_guards(void) {
  char buf[64];
  TEST_ASSERT_FALSE(cliResponseTopic(NULL, sizeof(buf), kPrefix));
  TEST_ASSERT_FALSE(cliResponseTopic(buf, 0, kPrefix));
  TEST_ASSERT_FALSE(cliResponseTopic(buf, sizeof(buf), NULL));
  TEST_ASSERT_FALSE(cliCommandTopic(buf, sizeof(buf), kPrefix, NULL));
  TEST_ASSERT_FALSE(cliTopicMatches(NULL, "a"));
  TEST_ASSERT_FALSE(cliTopicMatches("a", NULL));
}

// Guard the matcher itself - a matcher that says "no" to everything would
// make the non-match test pass for the wrong reason.
void test_matcher_sanity(void) {
  TEST_ASSERT_TRUE(cliTopicMatches("a/b/#", "a/b/c"));
  TEST_ASSERT_TRUE(cliTopicMatches("a/b/#", "a/b/c/d"));
  TEST_ASSERT_TRUE(cliTopicMatches("a/+/c", "a/b/c"));
  TEST_ASSERT_FALSE(cliTopicMatches("a/+/c", "a/b/d"));
  TEST_ASSERT_FALSE(cliTopicMatches("a/b/c", "a/b"));
  TEST_ASSERT_TRUE(cliTopicMatches("a/b/c", "a/b/c"));
  TEST_ASSERT_FALSE(cliTopicMatches("a/b", "a/b/c"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_response_topic_does_not_match_input_subscription);
  RUN_TEST(test_legacy_response_topic_did_match);
  RUN_TEST(test_command_topics_match_input_subscription);
  RUN_TEST(test_exact_topic_strings);
  RUN_TEST(test_truncation_is_reported);
  RUN_TEST(test_null_guards);
  RUN_TEST(test_matcher_sanity);
  return UNITY_END();
}
