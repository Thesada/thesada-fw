// thesada-fw - shell_line_policy.h
// Splits a shell command line into argv. Refuses a line or a token it cannot
// hold instead of clipping it: a clipped line still runs, as a different
// command. Also the nesting limit and the result a transport reports as ok.
// Pure, so the limits are host-testable.
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

// Copy line into buf and split it on spaces and tabs into argv. restFrom > 0
// makes argv[restFrom] the rest of the line, inner whitespace kept, trailing cut.
// in: line, scratch buf + its size, argv + its slots, restFrom (0 = all words).
// out: Ok with *argc set; TooLong / TooManyArgs leave *argc at 0.
inline ShellLine shellSplitLine(const char* line, char* buf, size_t bufSize,
                                char** argv, int maxArgs, int* argc, int restFrom = 0) {
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
    if (n - 1 == restFrom && restFrom > 0) {
      char* end = p + strlen(p);
      while (end[-1] == ' ' || end[-1] == '\t') end--;
      *end = '\0';
      break;
    }
    while (*p != '\0' && *p != ' ' && *p != '\t') p++;
    if (*p != '\0') *p++ = '\0';
  }
  *argc = n;
  return ShellLine::Ok;
}

// Parse buffers: the outer command and one console line run while it blocks
// (OTA, net.http, cellular waits pump the console). Deeper nesting is refused.
static constexpr int SHELL_NEST_MAX = 2;

// in: commands already running. out: parse buffer index, -1 = refuse.
inline int shellParseSlot(int depth) {
  return (depth >= 0 && depth < SHELL_NEST_MAX) ? depth : -1;
}

// The console line buffer and the nesting depth belong to one task.
// in: calling task, owner (nullptr before Shell::begin). out: may pump.
inline bool shellPumpAllowed(const void* task, const void* owner) {
  return owner == nullptr || task == owner;
}

enum class ShellResult { Ok, Failed };

// Outcome of one execute(); transports report it as ok.
// in: split verdict, argc, command found, handler reported failure. out: result.
inline ShellResult shellDispatchResult(ShellLine split, int argc, bool found,
                                       bool handlerFailed) {
  if (split != ShellLine::Ok) return ShellResult::Failed;
  if (argc == 0) return ShellResult::Ok;
  if (!found || handlerFailed) return ShellResult::Failed;
  return ShellResult::Ok;
}
