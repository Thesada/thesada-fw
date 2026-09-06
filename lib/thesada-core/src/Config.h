// thesada-fw - Config.h
// Loads config.json from LittleFS and exposes values
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>

// Single-task invariant: every method reads or writes the one _doc with no
// locking, and ArduinoJson is not thread-safe. Every caller must run on the
// main loop task: module begin()/loop(), Shell handlers, MQTT callbacks all
// do today. Do NOT call from an ISR or a secondary FreeRTOS task. If a second
// task ever needs config, add a recursive mutex here first, as EventBus.h
// says for the bus.
class Config {
public:
  static void load();
  static bool save();   // false if the on-disk write failed (open or short write)
  static void replace(const char* json);
  static bool set(const char* path, const char* value);
  static JsonObject get();
private:
  static JsonDocument _doc;
};
