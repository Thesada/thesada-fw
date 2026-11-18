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

inline bool configIsJsonSpace(char c) {
  return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// ArduinoJson stops at the end of the first value and ignores what follows.
// in: bytes that start with an object, length. out: only JSON whitespace after its closing brace.
inline bool configNothingAfterObject(const char* data, size_t len) {
  size_t i = 0;
  while (i < len && configIsJsonSpace(data[i])) i++;
  int depth = 0;
  char quote = 0;  // ArduinoJson also takes single-quoted strings
  for (; i < len; i++) {
    const char c = data[i];
    if (quote) {
      if (c == '\\') i++;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == '{' || c == '[') {
      depth++;
    } else if ((c == '}' || c == ']') && --depth == 0) {
      break;
    }
  }
  if (i >= len) return false;
  for (i++; i < len; i++) {
    if (!configIsJsonSpace(data[i])) return false;
  }
  return true;
}

// A config file handed over as bytes (fs.write) must be one JSON object and nothing more.
// in: bytes, length. out: true when it is.
inline bool configBytesAreObject(const char* data, size_t len) {
  if (!data || len == 0) return false;
  JsonDocument doc;
  if (deserializeJson(doc, data, len)) return false;
  return doc.is<JsonObject>() && configNothingAfterObject(data, len);
}

enum ConfigReplace {
  CONFIG_REPLACE_OK = 0,
  CONFIG_REPLACE_PARSE_FAILED,
  CONFIG_REPLACE_NOT_OBJECT,
  CONFIG_REPLACE_PERSIST_FAILED,
};

// Install a pushed document when it is one object and save() persists it. The
// prefix hold is cleared for save(), and on a failed save the previous doc and
// hold come back as they were, with no filesystem read.
// in: live doc, hold (value type, Hold() is no hold), JSON text, error out,
// save() -> bool. out: result, err set on a parse failure.
template <typename Hold, typename Save>
inline ConfigReplace configReplaceLive(JsonDocument& live, Hold& hold, const char* json,
                                       DeserializationError& err, Save save) {
  JsonDocument fresh;
  err = deserializeJson(fresh, json);
  if (err) return CONFIG_REPLACE_PARSE_FAILED;
  if (!fresh.is<JsonObject>()) return CONFIG_REPLACE_NOT_OBJECT;
  JsonDocument prev = std::move(live);
  const Hold prevHold = hold;
  hold = Hold();
  live = std::move(fresh);
  if (save()) return CONFIG_REPLACE_OK;
  live = std::move(prev);
  hold = prevHold;
  return CONFIG_REPLACE_PERSIST_FAILED;
}
