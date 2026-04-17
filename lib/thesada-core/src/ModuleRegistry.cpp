// thesada-fw - ModuleRegistry.cpp
// Priority-sorted module registry. Modules self-register via MODULE_REGISTER.
// SPDX-License-Identifier: GPL-3.0-only
#include "ModuleRegistry.h"
#include "Log.h"


struct ModuleEntry { Module* mod; uint8_t priority; };
static ModuleEntry _entries[ModuleRegistry::MAX_MODULES];
static uint8_t _count = 0;

// Register a module with a priority level
void ModuleRegistry::add(Module* module, uint8_t priority) {
  if (_count >= MAX_MODULES) return;
  _entries[_count++] = { module, priority };
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

  for (uint8_t i = 0; i < _count; i++) {
    char msg[48];
    snprintf(msg, sizeof(msg), "Init [%d]: %s", _entries[i].priority, _entries[i].mod->name());
    Log::info("Registry", msg);
    _entries[i].mod->begin();
  }
}

// Call loop() on all registered modules
void ModuleRegistry::loopAll() {
  for (uint8_t i = 0; i < _count; i++) {
    _entries[i].mod->loop();
  }
}

// Return the number of registered modules
uint8_t ModuleRegistry::count() { return _count; }

// Return module at index, or nullptr
Module* ModuleRegistry::get(uint8_t index) {
  if (index >= _count) return nullptr;
  return _entries[index].mod;
}
