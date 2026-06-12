// thesada-fw - ModuleRegistry.cpp
// Priority-sorted module registry. Modules self-register via MODULE_REGISTER.
// SPDX-License-Identifier: GPL-3.0-only
#include "ModuleRegistry.h"
#include "MQTTClient.h"
#include "Config.h"
#include "Log.h"


struct ModuleEntry { Module* mod; uint8_t priority; bool enabled; };
static ModuleEntry _entries[ModuleRegistry::MAX_MODULES];
static uint8_t _count = 0;

// Register a module with a priority level
void ModuleRegistry::add(Module* module, uint8_t priority) {
  if (_count >= MAX_MODULES) return;
  _entries[_count++] = { module, priority, true };
}

// Sort by priority (insertion sort) and call begin() on each module
void ModuleRegistry::beginAll() {
  // Insertion sort - small array, no need for std::sort
  for (uint8_t i = 1; i < _count; i++) {
    ModuleEntry tmp = _entries[i];
    int j = i - 1;
    while (j >= 0 && _entries[j].priority > tmp.priority) {
      _entries[j + 1] = _entries[j];
      j--;
    }
    _entries[j + 1] = tmp;
  }

  JsonObject cfg = Config::get();
  for (uint8_t i = 0; i < _count; i++) {
    Module* m = _entries[i].mod;
    // Gate, see Module.h: optional default off, core default on.
    bool en = cfg[m->configKey()]["enabled"] | m->coreModule();
    _entries[i].enabled = en;
    if (!en) {
      char msg[48];
      snprintf(msg, sizeof(msg), "Skip [%d]: %s (disabled)", _entries[i].priority, m->name());
      Log::info("Registry", msg);
      continue;
    }
    char msg[48];
    snprintf(msg, sizeof(msg), "Init [%d]: %s", _entries[i].priority, m->name());
    Log::info("Registry", msg);
    m->begin();
    // Feed MQTT keepalive between module inits. Module begin() can take
    // seconds (TFT init, Lua script load, SD mount) and the total init
    // time can exceed the MQTT keepalive window, dropping the connection.
    MQTTClient::tick();
  }
}

// Disabled modules never ran begin(); skip their loop() too.
void ModuleRegistry::loopAll() {
  for (uint8_t i = 0; i < _count; i++) {
    if (!_entries[i].enabled) continue;
    _entries[i].mod->loop();
  }
}

bool ModuleRegistry::enabled(uint8_t index) {
  if (index >= _count) return false;
  return _entries[index].enabled;
}

// Return the number of registered modules
uint8_t ModuleRegistry::count() { return _count; }

// Return module at index, or nullptr
Module* ModuleRegistry::get(uint8_t index) {
  if (index >= _count) return nullptr;
  return _entries[index].mod;
}
