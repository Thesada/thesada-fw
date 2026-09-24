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
  // false on parse or persist failure. Either failure reloads the on-disk file
  // and puts a held boot prefix back.
  static bool replace(const char* json);
  // Largest topic prefix the hold can keep without truncating it.
  static constexpr size_t TOPIC_PREFIX_CAP = 128;
  // Put the boot prefix back in the live doc after a replace. Later saves
  // still write the prefix that is already on disk.
  // in: boot prefix, full string. out: false if prefix is null or either
  // string does not fit. The live doc is unchanged on failure.
  static bool holdTopicPrefix(const char* prefix);
  // in: none. out: true while a boot prefix is overlaid on the live doc.
  static bool topicPrefixHeld();
  // in: buffer. out: false if no prefix is held or it does not fit.
  static bool copyBootTopicPrefix(char* out, size_t cap);
  // Copy the doc a save should write. A held boot prefix stays out of the copy.
  // in: destination document. out: false if the copy did not land.
  static bool copyDiskDoc(JsonDocument& dst);
  static bool set(const char* path, const char* value);
  static JsonObject get();
private:
  static JsonDocument _doc;
};
