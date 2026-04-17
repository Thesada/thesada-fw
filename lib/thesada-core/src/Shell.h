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

  // Process a command line string. Called by serial handler and WebSocket handler.
  // Output is sent via the provided callback.
  static void execute(const char* line, ShellOutput out);

  // Register a command. Called internally by begin() and by modules.
  static void registerCommand(const char* name, const char* help, ShellCommand handler);

  // Tab completion / help for a partial command. Returns matching commands.
  static void listCommands(ShellOutput out);

  // Max commands
  static constexpr int MAX_COMMANDS = 40;

private:
  static void registerBuiltins();
  static int parse(const char* line, char** argv, int maxArgs);

  static ShellEntry _commands[MAX_COMMANDS];
  static int _commandCount;
  static char _parseBuf[256];  // mutable copy for tokenization
};
