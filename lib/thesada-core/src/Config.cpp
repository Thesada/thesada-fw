// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include "log_kv_policy.h"
#include <LittleFS.h>
#include <cmath>
#include <climits>

static const char* TAG = "Config";

JsonDocument Config::_doc;

// Prefix already written by replace(). save() writes this, not the boot
// value holdTopicPrefix puts back into the live doc.
static char _diskTopicPrefix[Config::TOPIC_PREFIX_CAP];
static bool _diskTopicPrefixPresent = false;
static bool _topicPrefixHeld = false;

// Drop the boot-prefix overlay.
// in: none. out: none.
static void clearHeldTopicPrefix() {
  _topicPrefixHeld = false;
  _diskTopicPrefixPresent = false;
  _diskTopicPrefix[0] = '\0';
}

void Config::load() {
  clearHeldTopicPrefix();
  if (!LittleFS.begin()) return;
  File f = LittleFS.open("/config.json", "r");
  if (!f) return;
  DeserializationError err = deserializeJson(_doc, f);
  f.close();
  // A malformed file would otherwise leave _doc half-parsed; clear to a
  // clean empty doc so get() returns defaults, not garbage.
  if (err) {
    _doc.clear();
    Log::kvfe(TAG, "config.parse_failed err=%s action=use_defaults", err.c_str());
  }
}

// Persist the in-memory doc to /config.json. out: true on success;
// false if the file cannot be opened or the write came up short (e.g.
// LittleFS full) so callers never report a save that did not happen.
// Writes go to a temp file first, then rename over config.json - lfs
// rename atomically replaces the destination, so a short write never
// destroys the last good config and the load() rollback in set()/
// replace() genuinely restores it.
bool Config::save() {
  char livePrefix[Config::TOPIC_PREFIX_CAP];
  bool swapped = false;
  if (_topicPrefixHeld) {
    const char* live = _doc["mqtt"]["topic_prefix"] | "";
    size_t n = strlen(live);
    // The hold only stores a prefix that fits. A longer live value means
    // something else wrote it; writing the doc now would drop the on-disk prefix.
    if (n >= sizeof(livePrefix)) {
      Log::error(TAG, "config.save_failed reason=prefix_length");
      return false;
    }
    memcpy(livePrefix, live, n + 1);
    if (_diskTopicPrefixPresent) {
      _doc["mqtt"]["topic_prefix"] = _diskTopicPrefix;
    } else {
      // The pushed document left the key out. Writing "" would make the
      // next boot read an empty prefix instead of the default.
      JsonObject mqtt = _doc["mqtt"].as<JsonObject>();
      if (!mqtt.isNull()) mqtt.remove("topic_prefix");
    }
    swapped = true;
  }
  auto restoreLivePrefix = [&]() {
    if (swapped) _doc["mqtt"]["topic_prefix"] = livePrefix;
  };
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) {
    Log::error(TAG, "config.save_failed reason=tmp_open");
    restoreLivePrefix();
    return false;
  }
  size_t written  = serializeJson(_doc, f);
  size_t expected = measureJson(_doc);
  f.close();
  if (written < expected) {
    Log::kvfe(TAG, "config.save_failed reason=short_write written=%u expected=%u",
              (unsigned)written, (unsigned)expected);
    LittleFS.remove("/config.json.tmp");
    restoreLivePrefix();
    return false;
  }
  if (!LittleFS.rename("/config.json.tmp", "/config.json")) {
    Log::error(TAG, "config.save_failed reason=rename");
    LittleFS.remove("/config.json.tmp");
    restoreLivePrefix();
    return false;
  }
  Log::kvf(TAG, "config.saved path=/config.json bytes=%u", (unsigned)written);
  restoreLivePrefix();
  return true;
}

// Replace whole config with new JSON. On parse or persist failure the
// cleared doc is rolled back to the on-disk file so a bad MQTT payload
// cannot wipe live config. in: JSON string. out: false if nothing was written.
bool Config::replace(const char* json) {
  char bootPrefix[Config::TOPIC_PREFIX_CAP];
  bool keepBoot = false;
  if (_topicPrefixHeld) {
    if (!Config::copyBootTopicPrefix(bootPrefix, sizeof(bootPrefix))) {
      Log::error(TAG, "config.replace_failed reason=prefix_length");
      return false;
    }
    keepBoot = true;
  }
  // save() must write this document, not a prefix held from an earlier push.
  clearHeldTopicPrefix();
  _doc.clear();
  DeserializationError err = deserializeJson(_doc, json);
  if (err) {
    Log::kvfe(TAG, "config.replace_failed err=%s action=rollback", err.c_str());
    load();
    if (keepBoot) Config::holdTopicPrefix(bootPrefix);
    return false;
  }
  if (!save()) {
    Log::error(TAG, "config.replace_failed reason=persist");
    load();
    if (keepBoot) Config::holdTopicPrefix(bootPrefix);
    return false;
  }
  Log::info(TAG, "config.replaced source=mqtt");
  return true;
}

// Keep `prefix` as the live topic prefix. The value already in the doc
// stays the one save() writes.
// in: boot prefix. out: false if prefix is null, the disk value is not a
// string, or either string does not fit.
bool Config::holdTopicPrefix(const char* prefix) {
  if (!prefix) {
    Log::warn(TAG, "config.topic_prefix_hold_skipped reason=null");
    return false;
  }
  JsonObjectConst root = _doc.as<JsonObjectConst>();
  JsonObjectConst mqtt = root["mqtt"];
  bool present = false;
  const char* disk = "";
  JsonVariantConst prefixVar = mqtt["topic_prefix"];
  // Missing stays missing. null, numbers, and objects are not strings.
  if (!prefixVar.isUnbound() && !prefixVar.is<const char*>()) {
    Log::warn(TAG, "config.topic_prefix_hold_skipped reason=type");
    return false;
  }
  if (prefixVar.is<const char*>()) {
    disk = prefixVar.as<const char*>();
    if (!disk) {
      Log::warn(TAG, "config.topic_prefix_hold_skipped reason=null");
      return false;
    }
    present = true;
  }
  size_t diskLen = strlen(disk);
  size_t liveLen = strlen(prefix);
  if (diskLen >= sizeof(_diskTopicPrefix) || liveLen >= sizeof(_diskTopicPrefix)) {
    Log::warn(TAG, "config.topic_prefix_hold_skipped reason=length");
    return false;
  }
  if (present) memcpy(_diskTopicPrefix, disk, diskLen + 1);
  else _diskTopicPrefix[0] = '\0';
  _diskTopicPrefixPresent = present;
  _topicPrefixHeld = true;
  _doc["mqtt"]["topic_prefix"] = prefix;
  return true;
}

bool Config::topicPrefixHeld() {
  return _topicPrefixHeld;
}

bool Config::copyBootTopicPrefix(char* out, size_t cap) {
  if (!out || cap == 0 || !_topicPrefixHeld) return false;
  const char* live = _doc["mqtt"]["topic_prefix"] | "";
  size_t n = strlen(live);
  if (n >= cap) {
    Log::error(TAG, "config.boot_prefix_unavailable reason=length");
    return false;
  }
  memcpy(out, live, n + 1);
  return true;
}

bool Config::copyDiskDoc(JsonDocument& dst) {
  dst.clear();
  if (!dst.set(_doc.as<JsonObjectConst>())) return false;
  if (_topicPrefixHeld) {
    if (_diskTopicPrefixPresent) dst["mqtt"]["topic_prefix"] = _diskTopicPrefix;
    else dst["mqtt"].as<JsonObject>().remove("topic_prefix");
  }
  return true;
}

// Set one value by dot-path (e.g. "telegram.cooldown_s"), preserving
// JSON type (bool/int/double/string). in: dot-path, value string.
// out: false if a parent key is missing or not an object, value is
// null, or the persist fails (in which case _doc is rolled back).
bool Config::set(const char* path, const char* value) {
  if (path && strcmp(path, "mqtt.topic_prefix") == 0) clearHeldTopicPrefix();
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
  if (logPathIsSensitive(path)) {
    Log::kvf(TAG, "config.set path=%s value=<redacted> value_len=%u",
             path, (unsigned)strlen(value));
  } else {
    Log::kvf(TAG, "config.set path=%s value=%s", path, value);
  }
  return true;
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
