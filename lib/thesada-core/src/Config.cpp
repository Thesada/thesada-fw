// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include "config_load_policy.h"
#include "config_write_policy.h"
#include "config_schema_policy.h"
#include <LittleFS.h>

static const char* TAG = "Config";

JsonDocument Config::_doc;

// Prefix already written by replace(). save() writes this, not the boot
// value holdTopicPrefix puts back into the live doc.
struct TopicPrefixHold {
  char disk[Config::TOPIC_PREFIX_CAP];
  bool diskPresent;
  bool held;
};
static TopicPrefixHold _hold = {};

// Bumped, and the hooks run, after every call that rebuilt or edited _doc.
static uint32_t _revision = 0;

// Set by the boot migration when it could not run, empty otherwise.
static char _schemaAlert[128];
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
  _hold = TopicPrefixHold();
}

// Copy /config.json to /config.json.bak through a tmp, read back by size.
static bool backupConfigFile() {
  File in = LittleFS.open("/config.json", "r");
  if (!in) return false;
  const size_t want = in.size();
  File out = LittleFS.open("/config.json.bak.tmp", "w");
  if (!out) { in.close(); return false; }
  uint8_t buf[256];
  size_t copied = 0;
  while (in.available()) {
    const size_t n = in.read(buf, sizeof(buf));
    if (n == 0 || out.write(buf, n) != n) break;
    copied += n;
  }
  in.close();
  out.close();
  size_t onDisk = 0;
  File chk = LittleFS.open("/config.json.bak.tmp", "r");
  if (chk) { onDisk = chk.size(); chk.close(); }
  if (copied != want || onDisk != want || !LittleFS.rename("/config.json.bak.tmp", "/config.json.bak")) {
    LittleFS.remove("/config.json.bak.tmp");
    return false;
  }
  return true;
}

static void logSchemaStep(int from, const char* name, size_t changed) {
  Log::kvf(TAG, "config.schema_step from=%d name=%s changed=%u", from, name, (unsigned)changed);
}

// configMigrateLive's view of LittleFS.
struct LittleFsMigrateFs {
  bool parse(JsonDocument& doc) {
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;
    const DeserializationError err = deserializeJson(doc, f);
    f.close();
    return !err;
  }
  bool backup() { return backupConfigFile(); }
  bool write(JsonVariantConst src) { return Config::writeFile(src, false); }
};

// Bring a parsed older config up to CONFIG_SCHEMA_CURRENT on disk, old file kept
// as .bak. Logs the outcome and sets the schema alert when it could not run.
static void migrateSchema(JsonDocument& live) {
  _schemaAlert[0] = '\0';
  const int from = configSchemaVersion(live.as<JsonVariantConst>());
  LittleFsMigrateFs fs;
  const char* why = nullptr;
  switch (configMigrateLive(fs, live, logSchemaStep)) {
    case CFG_MIGRATE_NONE:
      return;
    case CFG_MIGRATE_DONE:
      Log::kvf(TAG, "config.schema_migrated from=%d to=%d backup=/config.json.bak", from, CONFIG_SCHEMA_CURRENT);
      return;
    case CFG_MIGRATE_REREAD_FAILED:
      Log::kvfe(TAG, "config.schema_migrate_failed from=%d reason=reread action=run_unmigrated", from);
      snprintf(_schemaAlert, sizeof(_schemaAlert), "config schema %d could not be re-read, running unmigrated", from);
      return;
    case CFG_MIGRATE_NEWER:
      Log::kvfw(TAG, "config.schema_newer have=%d know=%d action=run_as_is", from, CONFIG_SCHEMA_CURRENT);
      snprintf(_schemaAlert, sizeof(_schemaAlert), "config schema %d is newer than this firmware (%d), running as is",
               from, CONFIG_SCHEMA_CURRENT);
      return;
    case CFG_MIGRATE_INVALID:
      Log::kvfe(TAG, "config.schema_invalid action=run_unmigrated");
      snprintf(_schemaAlert, sizeof(_schemaAlert), "config schema field is not a whole number, running unmigrated");
      return;
    case CFG_MIGRATE_BACKUP_FAILED: why = "backup"; break;
    case CFG_MIGRATE_WRITE_FAILED:  why = "write";  break;
  }
  Log::kvfe(TAG, "config.schema_migrate_failed from=%d to=%d reason=%s action=run_unmigrated",
            from, CONFIG_SCHEMA_CURRENT, why);
  snprintf(_schemaAlert, sizeof(_schemaAlert), "config schema %d to %d failed (%s), running unmigrated",
           from, CONFIG_SCHEMA_CURRENT, why);
}

const char* Config::schemaAlert() { return _schemaAlert[0] ? _schemaAlert : nullptr; }

// configLoadInto's view of LittleFS.
struct LittleFsConfigSource {
  bool mount() { return LittleFS.begin(); }
  bool parse(JsonDocument& doc, DeserializationError& err) {
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;
    configParseWhole(f, doc, err);
    f.close();
    return true;
  }
};

bool Config::load() {
  _schemaAlert[0] = '\0';
  const bool hadDoc = !_doc.isNull();
  LittleFsConfigSource src;
  DeserializationError err;
  const ConfigLoad r = configLoadInto(src, _doc, err);
  if (r != CONFIG_LOAD_OK) {
    // The doc and its held prefix stay as they were, on boot and reload alike.
    Log::kvfe(TAG, "%s err=%s action=%s", configLoadEvent(r),
              err ? err.c_str() : (r == CONFIG_LOAD_PARSE_FAILED ? "not_object" : "none"),
              hadDoc ? "keep_previous" : "use_defaults");
    return false;
  }
  clearHeldTopicPrefix();
  migrateSchema(_doc);
  configChanged();
  return true;
}

// Persist the in-memory doc to /config.json. out: true on success;
// false if the file cannot be opened or the write came up short (e.g.
// LittleFS full) so callers never report a save that did not happen.
// Writes go to a temp file first, then rename over config.json - lfs
// rename atomically replaces the destination, so a short write never
// destroys the last good config.
bool Config::save() {
  char livePrefix[Config::TOPIC_PREFIX_CAP];
  bool swapped = false;
  if (_hold.held) {
    const char* live = _doc["mqtt"]["topic_prefix"] | "";
    size_t n = strlen(live);
    // The hold only stores a prefix that fits. A longer live value means
    // something else wrote it; writing the doc now would drop the on-disk prefix.
    if (n >= sizeof(livePrefix)) {
      Log::error(TAG, "config.save_failed reason=prefix_length");
      return false;
    }
    memcpy(livePrefix, live, n + 1);
    if (_hold.diskPresent) {
      _doc["mqtt"]["topic_prefix"] = _hold.disk;
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

static bool commitTmp(size_t written, size_t expected, size_t* bytes);

// The one tmp + rename path for /config.json. lfs rename replaces the
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
  return commitTmp(written, expected, bytes);
}

bool Config::writeRaw(const char* data, size_t len, size_t* bytes) {
  if (!configBytesAreObject(data, len)) {
    Log::error(TAG, "config.save_failed reason=not_object");
    return false;
  }
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) {
    Log::error(TAG, "config.save_failed reason=tmp_open");
    return false;
  }
  size_t written = f.write((const uint8_t*)data, len);
  f.close();
  return commitTmp(written, len, bytes);
}

// Rename a closed /config.json.tmp over /config.json once its size read back
// matches. in: bytes handed to the File, intended size. out: false if the old file stayed.
static bool commitTmp(size_t written, size_t expected, size_t* bytes) {
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

// Replace whole config with new JSON. A push that is not one object, or does
// not persist, leaves the live doc and its prefix hold as they were.
// in: JSON string. out: false if nothing was written.
bool Config::replace(const char* json) {
  DeserializationError err;
  switch (configReplaceLive(_doc, _hold, json, err, [] { return save(); })) {
    case CONFIG_REPLACE_OK:
      break;
    case CONFIG_REPLACE_PARSE_FAILED:
      Log::kvfe(TAG, "config.replace_failed err=%s action=keep_live", err.c_str());
      return false;
    case CONFIG_REPLACE_NOT_OBJECT:
      Log::error(TAG, "config.replace_failed reason=not_object action=keep_live");
      return false;
    case CONFIG_REPLACE_PERSIST_FAILED:
      Log::error(TAG, "config.replace_failed reason=persist action=keep_live");
      return false;
  }
  Log::info(TAG, "config.replaced source=mqtt");
  configChanged();
  return true;
}

bool Config::edit(const std::function<void(JsonObject)>& fn) {
  switch (configEditLive(_doc, [&](JsonDocument& d) { fn(d.as<JsonObject>()); }, [] { return save(); })) {
    case CONFIG_EDIT_OK:
      configChanged();
      return true;
    case CONFIG_EDIT_NO_COPY:
      Log::error(TAG, "config.edit_failed reason=no_heap action=keep_live");
      return false;
    case CONFIG_EDIT_PERSIST_FAILED:
      Log::error(TAG, "config.edit_failed reason=persist action=restore_live");
      return false;
  }
  return false;
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
  if (diskLen >= sizeof(_hold.disk) || liveLen >= sizeof(_hold.disk)) {
    Log::warn(TAG, "config.topic_prefix_hold_skipped reason=length");
    return false;
  }
  if (present) memcpy(_hold.disk, disk, diskLen + 1);
  else _hold.disk[0] = '\0';
  _hold.diskPresent = present;
  _hold.held = true;
  _doc["mqtt"]["topic_prefix"] = prefix;
  return true;
}

bool Config::topicPrefixHeld() {
  return _hold.held;
}

bool Config::copyBootTopicPrefix(char* out, size_t cap) {
  if (!out || cap == 0 || !_hold.held) return false;
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
  if (_hold.held) {
    if (_hold.diskPresent) dst["mqtt"]["topic_prefix"] = _hold.disk;
    else dst["mqtt"].as<JsonObject>().remove("topic_prefix");
  }
  return true;
}

JsonObject Config::get() {
  return _doc.as<JsonObject>();
}
