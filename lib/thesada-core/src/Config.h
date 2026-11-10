// thesada-fw - Config.h
// Loads config.json from LittleFS and exposes values
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>

// Single-task invariant: every method reads or writes the one _doc with no
// locking, and ArduinoJson is not thread-safe. Every caller must run on the
// main loop task: module begin()/loop(), Shell handlers, MQTT callbacks all
// do today. Do NOT call from an ISR or a secondary FreeRTOS task. A second
// task reads a copy the main loop refreshes from onChange(), as HttpServer's
// web snapshot does for the async_tcp handlers.
class Config {
public:
  static void load();
  static bool save();   // false if the on-disk write failed (open or short write)
  // tmp + rename into /config.json, live doc untouched. Main loop only: every
  // writer shares /config.json.tmp. out: false if the old file stayed, bytes on disk.
  static bool writeFile(JsonVariantConst src, bool pretty, size_t* bytes = nullptr);
  // false on parse or persist failure. Either failure reloads the on-disk file
  // and puts a held boot prefix back.
  static bool replace(const char* json);
  // Largest topic prefix the hold can keep without truncating it.
  static constexpr size_t TOPIC_PREFIX_CAP = 128;
  // Put the boot prefix back in the live doc after a replace. Later saves
  // still write the prefix that is already on disk.
  // in: boot prefix, full string. out: false if prefix is null, the disk
  // value is not a string, or either string does not fit. Unchanged on failure.
  static bool holdTopicPrefix(const char* prefix);
  // in: none. out: true while a boot prefix is overlaid on the live doc.
  static bool topicPrefixHeld();
  // in: buffer. out: false if no prefix is held or it does not fit.
  static bool copyBootTopicPrefix(char* out, size_t cap);
  // Copy the doc a save should write. A held boot prefix stays out of the copy.
  // in: destination document. out: false if the copy did not land.
  static bool copyDiskDoc(JsonDocument& dst);
  // Changes after every load, replace or set that rebuilt or edited the doc.
  static uint32_t revision();
  // Run fn on the main loop after each such change, before the caller resumes.
  // in: hook. out: false when all 4 slots are taken.
  static bool onChange(void (*fn)());
  static JsonObject get();
  // in: none. out: why the boot schema migration did not run, or nullptr.
  static const char* schemaAlert();
private:
  static JsonDocument _doc;
};
