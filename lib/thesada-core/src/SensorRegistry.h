// thesada-fw - SensorRegistry.h
// Uniform registry of sensor-reading callbacks. Modules call add() during
// begin() to expose a one-shot read to the shell `sensors` command.
// Plain function pointer + void* ctx - no heap, no std::function.
//
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Shell.h"

// Read callback signature: receives the caller-supplied shell output sink
// and the opaque ctx pointer the module registered with.
using SensorReadFn = void (*)(ShellOutput out, void* ctx);

struct SensorEntry {
  const char* name;
  const char* desc;
  SensorReadFn read;
  void*        ctx;           // module instance ptr or nullptr
  bool         enabled;       // false = listed as (disabled)
};

class SensorRegistry {
public:
  static constexpr int MAX_SENSORS = 16;

  // Register a sensor read callback. Safe to call from module begin().
  // Silently drops if the table is full (boot-time warning logged).
  // in: name (literal, not copied), desc, read fn, ctx, initial enabled state.
  // out: true if stored, false if full.
  static bool add(const char* name, const char* desc,
                  SensorReadFn read, void* ctx, bool enabled = true);

  // in: name, enabled flag. out: none. No-op if name not found.
  static void setEnabled(const char* name, bool enabled);

  // in: name. out: pointer to entry, or nullptr if not found.
  static const SensorEntry* find(const char* name);

  // Iterate all registered entries in registration order.
  // in: callback(entry, out). out: none.
  static void forEach(void (*visit)(const SensorEntry& e, ShellOutput out),
                      ShellOutput out);

  static int count();

private:
  static SensorEntry _entries[MAX_SENSORS];
  static int         _count;
};
