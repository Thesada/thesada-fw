// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include <LittleFS.h>
#include <cmath>
#include <climits>

static const char* TAG = "Config";

JsonDocument Config::_doc;

void Config::load() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  DeserializationError err = deserializeJson(_doc, f);
  f.close();
  // A malformed file would otherwise leave _doc half-parsed; clear to a
  // clean empty doc so get() returns defaults, not garbage.
  if (err) {
    _doc.clear();
    char msg[64];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "config.json parse failed (%s) - using defaults", err.c_str());
    Log::error(TAG, msg);
  }
}

// Persist the in-memory doc to /config.json. out: true on success;
// false if the file cannot be opened or the write came up short (e.g.
// LittleFS full) so callers never report a save that did not happen.
bool Config::save() {
  File f = LittleFS.open("/config.json", "w");
  if (!f) { Log::error(TAG, "Failed to open config.json for writing"); return false; }
  size_t written  = serializeJson(_doc, f);
  size_t expected = measureJson(_doc);
  f.close();
  if (written < expected) {
    char msg[64];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "Short write to config.json: %u/%u bytes",
             (unsigned)written, (unsigned)expected);
    Log::error(TAG, msg);
    return false;
  }
  char msg[48];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Saved %u bytes to /config.json", (unsigned)written);
  Log::info(TAG, msg);
  return true;
}

// Replace whole config with new JSON. On parse failure the cleared doc
// is rolled back to the on-disk file so a bad MQTT payload cannot wipe
// live config. in: JSON string.
void Config::replace(const char* json) {
  _doc.clear();
  DeserializationError err = deserializeJson(_doc, json);
  if (err) {
    char msg[64];
    // TODO: migrate to structured logging
    snprintf(msg, sizeof(msg), "Replace failed: %s", err.c_str());
    Log::error(TAG, msg);
    load();  // rollback to file on disk
    return;
  }
  if (!save()) {
    Log::error(TAG, "Config replace parsed but failed to persist");
    return;
  }
  Log::info(TAG, "Config replaced via MQTT");
}

// Set one value by dot-path (e.g. "telegram.cooldown_s"), preserving
// JSON type (bool/int/double/string). in: dot-path, value string.
// out: false if a parent key is missing or not an object, value is
// null, or the persist fails (in which case _doc is rolled back).
bool Config::set(const char* path, const char* value) {
  char buf[128];
  // Reject empty/over-long paths (truncation would rewrite a different
  // key) and a null value (the strcmp/strtod below would deref it).
  if (!path || !*path || !value || strlen(path) >= sizeof(buf)) return false;
  JsonVariant node = _doc.as<JsonVariant>();
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  char* lastDot = strrchr(buf, '.');
  const char* finalKey = buf;
  if (lastDot) {
    *lastDot = '\0';
    finalKey = lastDot + 1;
    char* tok = strtok(buf, ".");
    while (tok) {
      // Parent must exist AND be an object - traversing into a scalar
      // (or absent) key would silently write nowhere.
      if (!node[tok].is<JsonObject>()) return false;
      node = node[tok];
      tok = strtok(nullptr, ".");
    }
  }

  if (strcmp(value, "true") == 0)       node[finalKey] = true;
  else if (strcmp(value, "false") == 0) node[finalKey] = false;
  else {
    char* end;
    double num = strtod(value, &end);
    // Whole string parsed as a finite number stores numerically; the int
    // cast is gated by an explicit range check so inf/nan/overflow (where
    // (int)num is UB) fall through to string storage instead.
    if (*end == '\0' && end != value && isfinite(num)) {
      if (!strchr(value, '.') &&
          num >= (double)INT_MIN && num <= (double)INT_MAX && num == (int)num)
        node[finalKey] = (int)num;
      else
        node[finalKey] = num;
    } else {
      node[finalKey] = value;
    }
  }

  if (!save()) {
    // Persist failed - roll _doc back to the on-disk state so get()
    // never returns a value that was never written.
    load();
    return false;
  }
  char msg[128];
  // TODO: migrate to structured logging
  snprintf(msg, sizeof(msg), "Set %s = %s", path, value);
  Log::info(TAG, msg);
  return true;
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
