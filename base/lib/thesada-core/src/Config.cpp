// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include <LittleFS.h>

static const char* TAG = "Config";

JsonDocument Config::_doc;

// Load config from LittleFS into memory
void Config::load() {
  if (!LittleFS.begin()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  deserializeJson(_doc, f);
  f.close();
}

// Persist in-memory config to LittleFS
void Config::save() {
  File f = LittleFS.open("/config.json", "w");
  if (!f) { Log::error(TAG, "Failed to open config.json for writing"); return; }
  size_t written = serializeJson(_doc, f);
  f.close();
  char msg[48];
  snprintf(msg, sizeof(msg), "Saved %u bytes to /config.json", (unsigned)written);
  Log::info(TAG, msg);
}

// Replace entire config with new JSON, rolling back on parse failure
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
  save();
  Log::info(TAG, "Config replaced via MQTT");
}

// Set a single config value by dot-separated path and save to flash
bool Config::set(const char* path, const char* value) {
  // Walk dot-separated path (e.g. "telegram.cooldown_s").
  JsonVariant node = _doc.as<JsonVariant>();
  char buf[128];
  strncpy(buf, path, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = '\0';

  // Find parent and final key.
  char* lastDot = strrchr(buf, '.');
  const char* finalKey = buf;
  if (lastDot) {
    *lastDot = '\0';
    finalKey = lastDot + 1;
    // Navigate to parent.
    char* tok = strtok(buf, ".");
    while (tok) {
      if (node[tok].isNull()) return false;
      node = node[tok];
      tok = strtok(nullptr, ".");
    }
  }

  // Try to preserve type: number, bool, or string.
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

  save();
  char msg[128];
  snprintf(msg, sizeof(msg), "Set %s = %s", path, value);
  Log::info(TAG, msg);
  return true;
}

// Return the in-memory config as a JsonObject
JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
