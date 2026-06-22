// thesada-fw - Shell.h
// Interactive CLI shell shared by the serial and WebSocket transports.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <functional>

namespace fs { class FS; }

// Sink for command output; one call per line. Each transport supplies its own.
using ShellOutput = std::function<void(const char* line)>;

// Deferred main-loop work for transports whose payload the line-based parser
// would mangle (binary frames, multi-step responses). Captures own their state.
using DeferredFn = std::function<void()>;

using ShellCommand = std::function<void(int argc, char** argv, ShellOutput out)>;

// Disk-usage probe a module supplies for its mount; fs::FS has no byte counts
// (subclass-specific). Returns bytes, 0 = empty / not mounted.
using FSDfFn = uint64_t(*)();

struct ShellEntry {
  const char* name;
  const char* help;
  ShellCommand handler;
};

class Shell {
public:
  // Call once at boot, after all core classes are initialized.
  static void begin();

  // Run a command line synchronously on the caller's task. Callers in a
  // non-main context (AsyncTCP, PubSubClient callback) MUST use enqueue()
  // instead: running here from those task stacks risks overflow on commands
  // that touch fs / MQTT / TLS.
  // in: command line, output callback. out: none.
  static void execute(const char* line, ShellOutput out);

  // Stage a command for the main-loop drain. false if the ring is full (caller
  // surfaces "shell busy"). sink fires once per output line during execute().
  // Safe from any task; copies happen under a brief critical section.
  // in: command, output sink. out: true if accepted, false if ring full.
  static bool enqueue(const char* line, ShellOutput sink);

  // Stage an arbitrary callable for the main-loop drain - for transports whose
  // payload is not a parseable command line (binary frames, multi-field
  // protocol). The callable owns its captured state. Backpressure as enqueue().
  // in: callable. out: true if accepted, false if ring full.
  static bool enqueueDeferred(DeferredFn fn);

  // Drain at most one staged command on the caller's (main-loop) task, so it
  // runs with full stack rather than an async/network callback stack. One per
  // call so a long command does not starve other periodic work.
  // in: none. out: none.
  static void loop();

  // Register a command (name, one-line help, handler).
  static void registerCommand(const char* name, const char* help, ShellCommand handler);

  // Map a mount prefix to a filesystem; modules with their own storage call
  // this after a successful mount. fs.* routes by leading path segment, with
  // LittleFS the default for unmatched paths. Prefix must start with '/' and
  // not end with '/' - e.g. "/sd" routes "/sd/log" to that FS as "/log".
  // in: prefix, fs pointer. out: true if registered, false if table full.
  static bool registerFS(const char* prefix, fs::FS* fs);

  // registerFS for mounts that can report disk usage. fs::FS exposes no
  // totalBytes/usedBytes (subclass-specific), so the module passes wrappers;
  // nullptr behaves like the 2-arg form. fs.df prints a line per mount that
  // advertises both.
  // in: prefix, fs pointer, used-bytes fn, total-bytes fn. out: true if registered.
  static bool registerFS(const char* prefix, fs::FS* fs, FSDfFn dfUsed, FSDfFn dfTotal);

  // Print one `[MOUNT] <prefix>` line per registered mount; bare `fs.ls`
  // appends these so an operator sees volumes without knowing prefixes.
  // in: output callback. out: none.
  static void listMounts(ShellOutput out);

  // Print a df line per mount carrying dfUsed/dfTotal. Member fn so it can
  // reach the private FSMount registry a free command handler cannot.
  // in: output callback. out: none.
  static void printRegisteredDf(ShellOutput out);

  // Resolve a path to its backing filesystem. LittleFS for any unmatched path;
  // never nullptr.
  // in: absolute path. out: filesystem pointer.
  static fs::FS* resolveFS(const char* path);

  // Strip a registered mount prefix so the underlying FS sees a root-relative
  // path; unmatched paths are returned unchanged. Exact "/sd" maps to "/".
  // The non-root case returns a pointer into the input buffer; "/" is a
  // literal, so the caller must NOT modify the returned pointer.
  // in: absolute path. out: path relative to the resolved FS root.
  static const char* stripPrefix(const char* path);

  // Validate an externally-supplied FS path before any LittleFS call. Every
  // transport that accepts a path (HTTP, Shell over serial / WS / MQTT CLI,
  // future BLE) MUST run it through this - single-source so tightening the
  // policy touches one place.
  // Policy: reject empty; require leading '/' (all callers emit absolute
  // paths); reject ".." (traversal); reject "//" (normaliser-escape).
  // in: null-terminated path. out: true if safe to pass to FS.
  static bool pathSafe(const char* path);

  // Print every registered command name. Backs tab-completion / help listing.
  // in: output callback. out: none.
  static void listCommands(ShellOutput out);

  // Help output: empty / nullptr filter prints category buckets plus ungrouped
  // commands; a filter (e.g. "cell") prints every matching command with its
  // help. Groups on the first '.' of dotted names.
  // in: filter, output callback. out: none.
  static void printHelp(const char* filter, ShellOutput out);

  // Read Serial and execute newline-terminated lines. The console buffer is a
  // shared singleton (USB-CDC / UART0), so this is safe to call from
  // main.loop() and from blocking inner loops that must keep the shell live
  // (e.g. cellular connect polling).
  static void pumpConsole();

  static constexpr int MAX_COMMANDS = 60;

  // Deferred-ring depth. 4 covers concurrent serial + WS + MQTT CLI bursts;
  // bump if "shell busy" shows up in normal use.
  static constexpr int DEFERRED_RING_SIZE = 4;
  static constexpr int DEFERRED_LINE_LEN  = 256;

private:
  static void registerBuiltins();
  static int parse(const char* line, char** argv, int maxArgs);

  static ShellEntry _commands[MAX_COMMANDS];
  static int _commandCount;
  static char _parseBuf[256];  // mutable copy for tokenization

  // FS mount registry, walked in registration order by resolveFS.
  static constexpr int FS_MOUNTS_MAX = 4;
  struct FSMount {
    const char* prefix;
    size_t      prefixLen;
    fs::FS*     fs;
    FSDfFn      dfUsed;   // nullptr = no df support for this mount
    FSDfFn      dfTotal;  // nullptr = no df support for this mount
  };
  static FSMount _fsMounts[FS_MOUNTS_MAX];
  static int     _fsMountCount;

  // Deferred-execution ring. `active` marks a slot in use (set on enqueue,
  // cleared after drain). Guarded by a brief portMUX so the indices and flags
  // stay consistent against AsyncTCP / PubSubClient-thread enqueues.
  enum class SlotMode : uint8_t { Empty, Shell, Handler };

  struct DeferredSlot {
    SlotMode    mode;
    char        line[DEFERRED_LINE_LEN];   // mode=Shell: command line
    ShellOutput sink;                       // mode=Shell: per-line output
    DeferredFn  fn;                         // mode=Handler: callable
    bool        active;
  };
  static DeferredSlot _ring[DEFERRED_RING_SIZE];
  static uint8_t      _ringHead;
  static uint8_t      _ringTail;
};
