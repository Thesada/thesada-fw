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
