// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include <LittleFS.h>

static const char* TAG = "Config";

JsonDocument Config::_doc;

void Config::load() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  deserializeJson(_doc, f);
  f.close();
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
    snprintf(msg, sizeof(msg), "Short write to config.json: %u/%u bytes",
             (unsigned)written, (unsigned)expected);
    Log::error(TAG, msg);
    return false;
  }
  char msg[48];
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
// out: false if a parent key on the path is missing.
bool Config::set(const char* path, const char* value) {
  char buf[128];
  // Reject empty or over-long paths; truncation would rewrite a
  // different key than the caller asked for.
  if (!path || !*path || strlen(path) >= sizeof(buf)) return false;
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
      if (node[tok].isNull()) return false;
      node = node[tok];
      tok = strtok(nullptr, ".");
    }
  }

  if (strcmp(value, "true") == 0)       node[finalKey] = true;
  else if (strcmp(value, "false") == 0) node[finalKey] = false;
  else {
    char* end;
    double num = strtod(value, &end);
    if (*end == '\0' && end != value) {
      if (num == (int)num && !strchr(value, '.')) node[finalKey] = (int)num;
      else node[finalKey] = num;
    } else {
      node[finalKey] = value;
    }
  }

  if (!save()) return false;   // persist failed - do not report success
  char msg[128];
  snprintf(msg, sizeof(msg), "Set %s = %s", path, value);
  Log::info(TAG, msg);
  return true;
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
