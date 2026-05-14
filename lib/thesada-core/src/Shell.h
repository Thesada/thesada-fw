// thesada-fw - Shell.h
// Interactive CLI shell for serial and WebSocket terminals.
// Commands are registered at boot. Both transports share the same parser.
//
// Command groups:
//   fs:     ls, cat, rm, write, df, mv
//   config: get, set, save, reload
//   net:    ifconfig, ping, ntp, mqtt
//   module: list, status
//   lua:    exec, load, reload
//   sys:    restart, heap, uptime, version, help, selftest
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <Arduino.h>
#include <functional>

namespace fs { class FS; }

// Output callback - shell commands write through this.
// Serial handler prints to Serial. WebSocket handler sends to client.
using ShellOutput = std::function<void(const char* line)>;

// Generic deferred handler - any callable with no args. Used by transports
// that need to defer arbitrary work to main-loop context (binary protocol
// payloads, multi-step responses) where the line-based shell parser would
// destroy meaning. Captures hold their own sinks/state.
using DeferredFn = std::function<void()>;

// Command handler signature: argc, argv, output callback.
using ShellCommand = std::function<void(int argc, char** argv, ShellOutput out)>;

// Disk-usage probe for a registered filesystem. Modules whose fs::FS
// subclass exposes byte counts (SD_MMC, SD) supply a pair of these so
// fs.df can report them without core knowing the concrete subclass.
// Returns bytes; 0 is a valid "empty / not mounted" answer.
using FSDfFn = uint64_t(*)();

struct ShellEntry {
  const char* name;         // e.g. "ls", "config.get"
  const char* help;         // one-line description
  ShellCommand handler;
};

class Shell {
public:
  // Call once at boot after all core classes are initialized.
  static void begin();

  // Process a command line string synchronously on the caller's task. Used
  // by the serial reader (always main loop context) and the deferred-drain
  // path. Callers running from non-main contexts (AsyncTCP, PubSubClient
  // callback) MUST use enqueue() instead - executing here from those
  // task stacks risks overflow on commands that touch fs / MQTT / TLS.
  // in: command line, output callback. out: none.
  static void execute(const char* line, ShellOutput out);

  // Stage a command for execution on the main loop task.
  // Copies the command into a ring slot and returns true. Returns false
  // if the ring is full - the caller is expected to surface "shell busy"
  // to the user. The sink callback is invoked once per output line during
  // the eventual execute() drain. Safe to call from any task context;
  // copies happen under a brief critical section.
  // in: command, output sink. out: true if accepted, false if ring full.
  static bool enqueue(const char* line, ShellOutput sink);

  // Stage an arbitrary callable for execution on the main loop task.
  // Used by transports (MQTT CLI, future BLE/Telegram) whose payload is
  // not parseable as a shell command line - binary blobs, multi-field
  // protocol frames, response shapes that don't map to per-line sink. The
  // callable owns its own state (captured by value); drain just invokes
  // it. Same backpressure contract as enqueue() - false on ring full.
  // in: callable. out: true if accepted, false if ring full.
  static bool enqueueDeferred(DeferredFn fn);

  // Drain one staged command (if any) on the caller's task. Wired into the
  // main loop so every enqueued command runs with full main-loop stack and
  // not from inside an async/network callback. Drains at most one slot per
  // call so a long-running command does not starve other periodic work.
  // in: none. out: none.
  static void loop();

  // Register a command. Called internally by begin() and by modules.
  static void registerCommand(const char* name, const char* help, ShellCommand handler);

  // Register a mount prefix -> filesystem mapping. Modules with their own
  // filesystem (SD card, future external storage) call this in begin()
  // after a successful mount. fs.* commands route by matching the path's
  // leading segment against registered prefixes. LittleFS is the default
  // for any path that does not match a registered prefix.
  //
  // Prefix MUST start with '/' and MUST NOT end with '/'. Example: "/sd"
  // routes "/sd/log042.csv" to the SD FS with the prefix stripped so the
  // underlying FS sees "/log042.csv".
  // in: prefix, fs pointer. out: true if registered, false if table full.
  static bool registerFS(const char* prefix, fs::FS* fs);

  // registerFS overload for filesystems that can report disk usage. The
  // fs::FS base class has no totalBytes()/usedBytes() - those are subclass-
  // specific - so a module that wants its volume to show up in fs.df passes
  // wrappers around its concrete subclass calls here. dfUsed/dfTotal may be
  // nullptr (equivalent to the 2-arg overload). cmd_df walks the registry
  // and prints a line for every mount that advertises both pointers.
  // in: prefix, fs pointer, used-bytes fn, total-bytes fn. out: true if registered.
  static bool registerFS(const char* prefix, fs::FS* fs, FSDfFn dfUsed, FSDfFn dfTotal);

  // Print one `[MOUNT] <prefix>` line per registered FS mount prefix.
  // Backs the discovery line that bare `fs.ls` appends after the LittleFS
  // root listing so an operator sees mounted volumes without knowing the
  // prefix in advance.
  // in: output callback. out: none.
  static void listMounts(ShellOutput out);

  // Print a df line for every registered mount whose FSMount entry carries
  // dfUsed + dfTotal pointers. cmd_df handles LittleFS inline and calls
  // this for the rest - member function so it can reach the private
  // FSMount registry that a free command handler cannot.
  // in: output callback. out: none.
  static void printRegisteredDf(ShellOutput out);

  // Resolve a path to the filesystem that backs it. Returns &LittleFS for
  // any path not matching a registered prefix. Never returns nullptr.
  // in: absolute path. out: filesystem pointer.
  static fs::FS* resolveFS(const char* path);

  // Strip a registered mount prefix from a path so the underlying fs::FS
  // sees the path relative to its own root. Paths that do not match a
  // registered prefix are returned unchanged. The exact prefix "/sd" maps
  // to "/" (root listing). Returns a pointer into the input buffer for
  // the non-root case; "/" is returned as a literal for the root case so
  // the caller must NOT modify the returned pointer.
  // in: absolute path. out: path relative to the resolved FS root.
  static const char* stripPrefix(const char* path);

  // Validate a filesystem path before passing it to LittleFS.open() / similar.
  // Every transport that accepts a path from an external source (HTTP, Shell
  // over serial / WS / MQTT CLI, future BLE) MUST run the argument through
  // pathSafe() before any FS call. Centralized here so the policy is single-
  // source: any new caller that forgets the check is the only thing that has
  // to change when the policy tightens further.
  //
  // Policy:
  //   - reject empty
  //   - require leading '/' (every legitimate caller emits absolute paths;
  //     the dashboard JS already does this)
  //   - reject ".." anywhere (parent-dir traversal)
  //   - reject "//" anywhere (normaliser-escape variants)
  // in:  null-terminated path. out: true if safe to pass to FS.
  static bool pathSafe(const char* path);

  // Tab completion / help for a partial command. Returns matching commands.
  static void listCommands(ShellOutput out);

  // Categorised help: nullptr / empty filter shows category buckets +
  // ungrouped commands; filter "cell" shows every cell.* command with
  // its description. Backs the `help` shell command. Splits dotted
  // names on the first '.' for grouping.
  static void printHelp(const char* filter, ShellOutput out);

  // Drain `Serial` for any bytes the host has typed and execute newline-
  // terminated lines via `execute()`. Same buffer state is shared across
  // every caller (it is the singleton USB-CDC / UART0 console), so this
  // can be called from main.loop() and from any blocking inner loop that
  // wants the shell to stay live (e.g. cellular network-connect polling).
  static void pumpConsole();

  // Max commands
  static constexpr int MAX_COMMANDS = 56;

  // Deferred-execution ring depth. 4 slots covers concurrent serial + WS +
  // MQTT CLI bursts without runaway memory. Bump if "shell busy" starts
  // surfacing in normal use.
  static constexpr int DEFERRED_RING_SIZE = 4;
  static constexpr int DEFERRED_LINE_LEN  = 256;

private:
  static void registerBuiltins();
  static int parse(const char* line, char** argv, int maxArgs);

  static ShellEntry _commands[MAX_COMMANDS];
  static int _commandCount;
  static char _parseBuf[256];  // mutable copy for tokenization

  // FS mount registry. Slots large enough for LittleFS + SD + future
  // external storage. resolveFS walks this in registration order.
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

  // Deferred-execution ring. Each slot holds a buffered command line
  // plus the per-call output sink. `active` flips true on enqueue and back
  // to false at the end of the drain so the slot is reusable. `head` is
  // the next slot loop() will drain; `tail` is where enqueue() writes.
  // Wraps modulo DEFERRED_RING_SIZE. Guarded by a brief portMUX during
  // enqueue/drain to keep the indices and active flags consistent against
  // AsyncTCP / PubSubClient-thread enqueues.
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
