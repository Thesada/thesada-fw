// thesada-fw - config_load_policy.h
// Which document stays live when /config.json is read, and what a config file
// must be before it replaces the one on disk. Pure, host-tested.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <utility>

enum ConfigLoad {
  CONFIG_LOAD_OK = 0,
  CONFIG_LOAD_MOUNT_FAILED,
  CONFIG_LOAD_FILE_MISSING,
  CONFIG_LOAD_PARSE_FAILED,  // malformed, or valid JSON that is not one object
};

// Parse the config file into a fresh doc and move it into live only when it is
// one JSON object. Every failure leaves live as it was: at boot that is the
// empty doc, on a reload the last good one.
// Fs: mount() -> bool, parse(JsonDocument&, DeserializationError&) -> bool,
// false when the file cannot be opened.
// in: fs, live doc, error out. out: result, err set on a parse failure.
template <typename Fs>
inline ConfigLoad configLoadInto(Fs& fs, JsonDocument& live, DeserializationError& err) {
  err = DeserializationError::Ok;
  if (!fs.mount()) return CONFIG_LOAD_MOUNT_FAILED;
  JsonDocument fresh;
  if (!fs.parse(fresh, err)) return CONFIG_LOAD_FILE_MISSING;
  if (err || !fresh.is<JsonObject>()) return CONFIG_LOAD_PARSE_FAILED;
  live = std::move(fresh);
  return CONFIG_LOAD_OK;
}

// in: result. out: the log event name for it.
inline const char* configLoadEvent(ConfigLoad r) {
  switch (r) {
    case CONFIG_LOAD_MOUNT_FAILED: return "config.mount_failed";
    case CONFIG_LOAD_FILE_MISSING: return "config.file_missing";
    case CONFIG_LOAD_PARSE_FAILED: return "config.parse_failed";
    default:                       return "config.loaded";
  }
}

// A config file handed over as bytes (fs.write) must parse as one JSON object.
// in: bytes, length. out: true when it does.
inline bool configBytesAreObject(const char* data, size_t len) {
  if (!data || len == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return false;
  return doc.is<JsonObject>();
}
