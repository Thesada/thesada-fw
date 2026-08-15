// Host-native unit tests for cli_topics.h (CLI topic construction).
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "cli_topics.h"

void setUp(void) {}
void tearDown(void) {}

static const char* kPrefix = "thesada/acme/owb";

void test_exact_topic_strings(void) {
  char buf[CLI_TOPIC_CAP];
  TEST_ASSERT_TRUE(cliInputSubscription(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/#", buf);
  TEST_ASSERT_TRUE(cliCommandTopic(buf, sizeof(buf), kPrefix, "fs.cat"));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/fs.cat", buf);
  TEST_ASSERT_TRUE(cliResponseTopic(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/response", buf);
  TEST_ASSERT_TRUE(cliInputPrefix(buf, sizeof(buf), kPrefix));
  TEST_ASSERT_EQUAL_STRING("thesada/acme/owb/cli/", buf);
}

// Commands must sit inside the wildcard, or they stop arriving.
void test_command_topics_match_input_subscription(void) {
  char sub[CLI_TOPIC_CAP], cmd[CLI_TOPIC_CAP];
  TEST_ASSERT_TRUE(cliInputSubscription(sub, sizeof(sub), kPrefix));
  const char* cmds[] = {"fs.cat", "restart", "ota.check", "config.dump"};
  for (unsigned i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
    TEST_ASSERT_TRUE(cliCommandTopic(cmd, sizeof(cmd), kPrefix, cmds[i]));
    TEST_ASSERT_TRUE(cliTopicMatches(sub, cmd));
  }
}

// Documents the bug this migration is groundwork for: the response topic sits
// INSIDE the device's own command subscription, so every response echoes back.
void test_response_topic_currently_matches_input_subscription(void) {
  char sub[CLI_TOPIC_CAP], resp[CLI_TOPIC_CAP];
  TEST_ASSERT_TRUE(cliInputSubscription(sub, sizeof(sub), kPrefix));
  TEST_ASSERT_TRUE(cliResponseTopic(resp, sizeof(resp), kPrefix));
  TEST_ASSERT_TRUE(cliTopicMatches(sub, resp));
}

// A truncated topic would publish to, or match against, the wrong topic.
void test_truncation_is_reported(void) {
  char tiny[8];
  TEST_ASSERT_FALSE(cliResponseTopic(tiny, sizeof(tiny), kPrefix));
  TEST_ASSERT_FALSE(cliCommandTopic(tiny, sizeof(tiny), kPrefix, "fs.cat"));
  TEST_ASSERT_FALSE(cliInputSubscription(tiny, sizeof(tiny), kPrefix));
  TEST_ASSERT_FALSE(cliInputPrefix(tiny, sizeof(tiny), kPrefix));
  // Exact fit is not truncation: prefix + suffix + NUL.
  char exact[sizeof("thesada/acme/owb/cli/response")];
  TEST_ASSERT_TRUE(cliResponseTopic(exact, sizeof(exact), kPrefix));
  // One byte short is.
  char shortBuf[sizeof("thesada/acme/owb/cli/response") - 1];
  TEST_ASSERT_FALSE(cliResponseTopic(shortBuf, sizeof(shortBuf), kPrefix));
}

void test_null_guards(void) {
  char buf[CLI_TOPIC_CAP];
  TEST_ASSERT_FALSE(cliResponseTopic(NULL, sizeof(buf), kPrefix));
  TEST_ASSERT_FALSE(cliResponseTopic(buf, 0, kPrefix));
  TEST_ASSERT_FALSE(cliResponseTopic(buf, sizeof(buf), NULL));
  TEST_ASSERT_FALSE(cliCommandTopic(buf, sizeof(buf), kPrefix, NULL));
  TEST_ASSERT_FALSE(cliTopicMatches(NULL, "a"));
  TEST_ASSERT_FALSE(cliTopicMatches("a", NULL));
}

// Guard the matcher itself - one that says "no" to everything would make the
// match assertions pass for the wrong reason.
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
  RUN_TEST(test_exact_topic_strings);
  RUN_TEST(test_command_topics_match_input_subscription);
  RUN_TEST(test_response_topic_currently_matches_input_subscription);
  RUN_TEST(test_truncation_is_reported);
  RUN_TEST(test_null_guards);
  RUN_TEST(test_matcher_sanity);
  return UNITY_END();
}
