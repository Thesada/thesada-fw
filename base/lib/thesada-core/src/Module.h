// thesada-fw - Module.h
// Base class for all modules.
// Constructor must be trivial - no hardware init, no Serial,
// no heap allocation. All init goes in begin().
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "Shell.h"

class Module {
public:
  virtual void begin() = 0;
  virtual void loop() = 0;
  virtual const char* name() = 0;
  virtual void status(ShellOutput out) { out("ok"); }
  virtual void selftest(ShellOutput out) {}
  virtual ~Module() {}
};

// Priority levels for module boot ordering.
// Modules with Lua bindings must init before ScriptEngine (PRIORITY_SCRIPT)
// so their bindings are registered when the Lua state is created.
enum ModulePriority : uint8_t {
    PRIORITY_POWER    = 10,   // PowerManager
    PRIORITY_NETWORK  = 20,   // Cellular
    PRIORITY_SERVICE  = 30,   // HttpServer/LiteServer, Display, TFT, Telegram
    PRIORITY_SCRIPT   = 40,   // ScriptEngine (creates Lua state, calls all registrars)
    PRIORITY_SENSOR   = 50,   // Temperature, ADS1115, Battery
    PRIORITY_OUTPUT   = 60,   // SD, PWM
    PRIORITY_LAST     = 100,  // SleepManager
};
