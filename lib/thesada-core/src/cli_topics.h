// thesada-fw - cli_topics.h
// Pure construction of the CLI MQTT topics, in one place so the protocol can
// be changed in one edit rather than nine.
//
// Command input and command output live in separate topic spaces. The device
// subscribes to the input wildcard; when responses shared that space the
// broker forwarded every response the device had just published straight back
// to it. Over cellular that echo arrives as a URC larger than the modem line
// buffer, which overflows and is dropped with a warning - pure waste, since
// the device has no handler for its own responses either way.
//
// Every builder truncates rather than overflowing, and reports whether the
// topic fitted. A truncated topic must never be published: it would land on a
// different topic than intended. Host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdio.h>

// Topic suffixes. The response suffix deliberately does not begin with the
// input segment, so it cannot match the input wildcard.
#define CLI_TOPIC_INPUT_WILDCARD "/cli/#"
#define CLI_TOPIC_INPUT_SEGMENT  "/cli/"
#define CLI_TOPIC_RESPONSE       "/cli_response"

// One capacity for every CLI topic buffer. A single value keeps a prefix from
// truncating on one path while fitting on another.
#define CLI_TOPIC_CAP 96

// Join prefix + suffix into out. out: true when the whole topic fitted.
inline bool cliTopicJoin(char* out, size_t cap, const char* prefix,
                         const char* suffix) {
  if (!out || cap == 0 || !prefix || !suffix) return false;
  int n = snprintf(out, cap, "%s%s", prefix, suffix);
  return n > 0 && (size_t)n < cap;
}

// <prefix>/cli/# - what the device subscribes to for commands.
inline bool cliInputSubscription(char* out, size_t cap, const char* prefix) {
  return cliTopicJoin(out, cap, prefix, CLI_TOPIC_INPUT_WILDCARD);
}

// <prefix>/cli/<command> - where a single command arrives.
inline bool cliCommandTopic(char* out, size_t cap, const char* prefix,
                            const char* command) {
  if (!out || cap == 0 || !prefix || !command) return false;
  int n = snprintf(out, cap, "%s%s%s", prefix, CLI_TOPIC_INPUT_SEGMENT, command);
  return n > 0 && (size_t)n < cap;
}

// <prefix>/cli_response - where responses go. Cannot match the input
// wildcard, which is the entire point of the split.
inline bool cliResponseTopic(char* out, size_t cap, const char* prefix) {
  return cliTopicJoin(out, cap, prefix, CLI_TOPIC_RESPONSE);
}

// <prefix>/cli/ - the literal command topics are matched against, to slice the
// command name off an inbound topic. Truncation here would match a short
// prefix and slice the wrong command, so callers must check the result.
inline bool cliInputPrefix(char* out, size_t cap, const char* prefix) {
  return cliTopicJoin(out, cap, prefix, CLI_TOPIC_INPUT_SEGMENT);
}

// Does topic match filter? Only the wildcards this protocol uses are modelled:
// '#' as a trailing multi-level wildcard, '+' as a single level. Exists so the
// non-match property between response and input topics can be asserted as a
// test rather than argued in a comment.
inline bool cliTopicMatches(const char* filter, const char* topic) {
  if (!filter || !topic) return false;
  const char* f = filter;
  const char* t = topic;
  while (*f) {
    if (*f == '#') return true;
    if (*f == '+') {
      while (*t && *t != '/') t++;
      f++;
      if (*f == '/' && *t == '/') { f++; t++; }
      continue;
    }
    if (*f != *t) return false;
    f++;
    t++;
  }
  return *t == '\0';
}
