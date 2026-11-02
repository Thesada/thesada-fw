// thesada-fw - shell_line_policy.h
// Splits a shell command line into argv. Refuses a line or a token it cannot
// hold instead of clipping it: a clipped line still runs, as a different
// command. Pure, so the limits are host-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// Longest line the parser holds, NUL included. MQTTClient::runCli builds its
// line in a buffer of the same size, so nothing it accepts is cut here.
static constexpr size_t SHELL_LINE_MAX = 1024;

// argv slots, the command name included.
static constexpr int SHELL_ARGS_MAX = 16;

enum class ShellLine { Ok, TooLong, TooManyArgs };

// in: string, buffer capacity including the NUL. out: true if s fits.
inline bool shellFits(const char* s, size_t cap) {
  return s != nullptr && strlen(s) < cap;
}

// Copy line into buf and split it on spaces and tabs into argv.
// in: line, scratch buf + its size, argv + its slots.
// out: Ok with *argc set; TooLong / TooManyArgs leave *argc at 0.
inline ShellLine shellSplitLine(const char* line, char* buf, size_t bufSize,
                                char** argv, int maxArgs, int* argc) {
  *argc = 0;
  if (!shellFits(line, bufSize)) return ShellLine::TooLong;
  memcpy(buf, line, strlen(line) + 1);
  int n = 0;
  char* p = buf;
  while (true) {
    while (*p == ' ' || *p == '\t') p++;
    if (*p == '\0') break;
    if (n >= maxArgs) return ShellLine::TooManyArgs;
    argv[n++] = p;
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') *p++ = '\0';
  }
  *argc = n;
  return ShellLine::Ok;
}
