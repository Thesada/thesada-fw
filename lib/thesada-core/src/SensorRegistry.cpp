// thesada-fw - SensorRegistry.cpp
// Backing store for the sensor callback table. Static storage, no heap.
//
// SPDX-License-Identifier: GPL-3.0-only
#include "SensorRegistry.h"
#include "Log.h"
#include <string.h>

static const char* TAG = "SENSORS";

SensorEntry SensorRegistry::_entries[SensorRegistry::MAX_SENSORS] = {};
int         SensorRegistry::_count = 0;

// Append a sensor entry. Name is assumed to be a literal / static string -
// we store the pointer, not a copy. Duplicate names are rejected (only the
// first wins) so double-registration from a module retry is not silently
// shadowing state.
bool SensorRegistry::add(const char* name, const char* desc,
                         SensorReadFn read, void* ctx, bool enabled) {
  if (!name || !read) return false;
  if (find(name) != nullptr) {
    Log::warn(TAG, "duplicate sensor registration ignored");
    return false;
  }
  if (_count >= MAX_SENSORS) {
    Log::warn(TAG, "sensor table full - bump MAX_SENSORS");
    return false;
  }
  _entries[_count++] = { name, desc, read, ctx, enabled };
  return true;
}

void SensorRegistry::setEnabled(const char* name, bool enabled) {
  for (int i = 0; i < _count; i++) {
    if (strcmp(_entries[i].name, name) == 0) {
      _entries[i].enabled = enabled;
      return;
    }
  }
}

const SensorEntry* SensorRegistry::find(const char* name) {
  for (int i = 0; i < _count; i++) {
    if (strcmp(_entries[i].name, name) == 0) return &_entries[i];
  }
  return nullptr;
}

void SensorRegistry::forEach(void (*visit)(const SensorEntry&, ShellOutput),
                             ShellOutput out) {
  for (int i = 0; i < _count; i++) {
    visit(_entries[i], out);
  }
}

int SensorRegistry::count() { return _count; }
