// thesada-fw - ModuleRegistry.h
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "Module.h"

class ModuleRegistry {
public:
  static void add(Module* module, uint8_t priority = PRIORITY_SENSOR);
  static void beginAll();
  static void loopAll();
  static uint8_t count();
  static Module* get(uint8_t index);

  static constexpr uint8_t MAX_MODULES = 24;
};

// Self-registration macro for modules.
// Place at bottom of each module .cpp file, inside the #ifdef ENABLE_* guard.
// Creates a static instance and registers it before setup() runs.
// __attribute__((used)) prevents the linker from stripping these symbols
// when -gc-sections is active and nothing in src/ references them directly.
#define MODULE_REGISTER(CLASS, PRIO) \
    __attribute__((used)) static CLASS _inst_##CLASS; \
    __attribute__((used)) static struct _AutoReg_##CLASS { \
        _AutoReg_##CLASS() { ModuleRegistry::add(&_inst_##CLASS, PRIO); } \
    } _autoreg_##CLASS;
