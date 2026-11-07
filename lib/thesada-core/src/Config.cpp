// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include "config_write_policy.h"
#include <LittleFS.h>

static const char* TAG = "Config";

JsonDocument Config::_doc;

// Prefix already written by replace(). save() writes this, not the boot
// value holdTopicPrefix puts back into the live doc.
static char _diskTopicPrefix[Config::TOPIC_PREFIX_CAP];
static bool _diskTopicPrefixPresent = false;
static bool _topicPrefixHeld = false;

// Bumped, and the hooks run, after every call that rebuilt or edited _doc.
static uint32_t _revision = 0;
static void (*_changeHooks[4])() = {};
uint32_t Config::revision() { return _revision; }

bool Config::onChange(void (*fn)()) {
  for (auto& h : _changeHooks) {
    if (!h) { h = fn; return true; }
  }
  return false;
}

// Main loop only, like every mutator that calls it. in: none. out: none.
static void configChanged() {
  _revision++;
  for (auto h : _changeHooks) if (h) h();
}

// Drop the boot-prefix overlay.
// in: none. out: none.
static void clearHeldTopicPrefix() {
  _topicPrefixHeld = false;
  _diskTopicPrefixPresent = false;
  _diskTopicPrefix[0] = '\0';
}

void Config::load() {
  auto read = [&]() {
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
  };
  read();
  configChanged();
}

// Persist the in-memory doc to /config.json. out: true on success;
// false if the file cannot be opened or the write came up short (e.g.
// LittleFS full) so callers never report a save that did not happen.
// Writes go to a temp file first, then rename over config.json - lfs
// rename atomically replaces the destination, so a short write never
// destroys the last good config and the load() rollback in replace()
// genuinely restores it.
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
  bool ok = writeFile(_doc.as<JsonVariantConst>(), false);
  restoreLivePrefix();
  return ok;
}

// The one tmp + rename writer for /config.json. lfs rename replaces the
// destination atomically, so a failure leaves the last good file in place.
bool Config::writeFile(JsonVariantConst src, bool pretty, size_t* bytes) {
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) {
    Log::error(TAG, "config.save_failed reason=tmp_open");
    return false;
  }
  size_t written  = pretty ? serializeJsonPretty(src, f) : serializeJson(src, f);
  size_t expected = pretty ? measureJsonPretty(src) : measureJson(src);
  f.close();
  // The real lfs write is inside fclose and its result is dropped. Read back.
  size_t onDisk = 0;
  File chk = LittleFS.open("/config.json.tmp", "r");
  if (chk) { onDisk = chk.size(); chk.close(); }
  if (!configWriteLanded(written, onDisk, expected)) {
    Log::kvfe(TAG, "config.save_failed reason=short_write written=%u on_disk=%u expected=%u",
              (unsigned)written, (unsigned)onDisk, (unsigned)expected);
    LittleFS.remove("/config.json.tmp");
    return false;
  }
  if (!LittleFS.rename("/config.json.tmp", "/config.json")) {
    Log::error(TAG, "config.save_failed reason=rename");
    LittleFS.remove("/config.json.tmp");
    return false;
  }
  Log::kvf(TAG, "config.saved path=/config.json bytes=%u", (unsigned)onDisk);
  if (bytes) *bytes = onDisk;
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
  configChanged();
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
  // Through text: dst.set() deep-copies recursively at about 1 KB of stack
  // per nesting level and overflowed the 8 KB loop task on config.save.
  String text;
  if (serializeJson(_doc, text) == 0) return false;
  if (deserializeJson(dst, text) != DeserializationError::Ok) return false;
  if (!dst.is<JsonObject>()) return false;
  if (_topicPrefixHeld) {
    if (_diskTopicPrefixPresent) dst["mqtt"]["topic_prefix"] = _diskTopicPrefix;
    else dst["mqtt"].as<JsonObject>().remove("topic_prefix");
  }
  return true;
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
