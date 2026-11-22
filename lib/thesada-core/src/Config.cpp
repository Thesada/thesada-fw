// thesada-fw - Config.cpp
// SPDX-License-Identifier: GPL-3.0-only
#include "Config.h"
#include "Log.h"
#include "config_load_policy.h"
#include "config_write_policy.h"
#include "config_schema_policy.h"
#include "SecretJson.h"
#include "secret_heap.h"
#include <LittleFS.h>

static const char* TAG = "Config";

JsonDocument Config::_doc;

// The prefix on disk while the live doc shows the boot one.
static_assert(Config::TOPIC_PREFIX_CAP == CONFIG_PREFIX_CAP, "one prefix cap");
static ConfigPrefixHold _hold = {};

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
  _hold = ConfigPrefixHold();
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
    DeserializationError err;
    configParseWhole(f, doc, err);
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

// in: none. out: /config.json size in bytes, -1 when it is missing,
// CONFIG_SIZE_UNREADABLE when it exists and does not open.
static long configFileSize() {
  File f = LittleFS.open("/config.json", "r");
  if (!f) return LittleFS.exists("/config.json") ? CONFIG_SIZE_UNREADABLE : -1;
  const long n = (long)f.size();
  f.close();
  return n;
}

// configLoadInto's view of LittleFS.
struct LittleFsConfigSource {
  bool mount() { return LittleFS.begin(); }
  long size() { return configFileSize(); }
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

static bool commitTmp(size_t written, size_t expected, size_t* bytes);

// The one tmp + rename path for /config.json. lfs rename replaces the
// destination atomically, so a failure leaves the last good file in place.
bool Config::writeFile(JsonVariantConst src, bool pretty, size_t* bytes) {
  if (!configDepthOk(src)) {
    Log::kvfe(TAG, "config.save_failed reason=too_deep max=%d", CONFIG_MAX_DEPTH);
    return false;
  }
  size_t expected = pretty ? measureJsonPretty(src) : measureJson(src);
  if (expected > CONFIG_FILE_MAX) {
    Log::kvfe(TAG, "config.save_failed reason=too_large bytes=%u max=%u", (unsigned)expected, (unsigned)CONFIG_FILE_MAX);
    return false;
  }
  File f = LittleFS.open("/config.json.tmp", "w");
  if (!f) {
    Log::error(TAG, "config.save_failed reason=tmp_open");
    return false;
  }
  size_t written = pretty ? serializeJsonPretty(src, f) : serializeJson(src, f);
  f.close();
  return commitTmp(written, expected, bytes);
}

bool Config::writeRaw(const char* data, size_t len, size_t* bytes) {
  if (!configBytesAreObject(data, len)) {
    Log::error(TAG, "config.save_failed reason=not_object");
    return false;
  }
  // A read mask in the bytes is resolved first, so the file is rewritten whole.
  if (configTextHasMarker(data, len)) {
    JsonDocument doc(secretJsonAllocator());
    if (!configParseObjectBytes(doc, data, len)) {
      Log::error(TAG, "config.save_failed reason=no_heap");
      return false;
    }
    if (!keepStoredSecrets(doc)) return false;
    return writeFile(doc.as<JsonVariantConst>(), false, bytes);
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

// configStage's view of LittleFS: the file, else the live doc through text.
struct LittleFsStageSource {
  long size() { return configFileSize(); }
  bool parse(JsonDocument& doc, DeserializationError& err) {
    File f = LittleFS.open("/config.json", "r");
    if (!f) return false;
    configParseWhole(f, doc, err);
    f.close();
    return true;
  }
  bool copyLive(JsonDocument& doc) { return Config::copyDiskDoc(doc); }
  bool hasLive() { return Config::hasLiveDoc(); }
};

// Write staged and move it into _doc. in: staged doc, log event. out: false if live stayed.
bool Config::commit(JsonDocument& staged, const char* event, bool deferPrefix) {
  if (!keepStoredSecrets(staged)) {
    Log::kvfe(TAG, "%s reason=copy_failed action=keep_live", event);
    return false;
  }
  const bool wasHeld = _hold.held;
  auto write = [](JsonVariantConst src) { return Config::writeFile(src, false); };
  const ConfigCommit r = deferPrefix ? configCommitDeferPrefix(_doc, staged, write, _hold)
                                     : configCommit(_doc, staged, write, &_hold);
  switch (r) {
    case CONFIG_COMMIT_OK:
      break;
    case CONFIG_COMMIT_NOT_OBJECT:
      Log::kvfe(TAG, "%s reason=not_object action=keep_live", event);
      return false;
    case CONFIG_COMMIT_NO_HEAP:
      Log::kvfe(TAG, "%s reason=no_heap action=keep_live", event);
      return false;
    case CONFIG_COMMIT_PERSIST_FAILED:
      Log::kvfe(TAG, "%s reason=persist action=keep_live", event);
      return false;
    case CONFIG_COMMIT_TOO_LARGE:
      Log::kvfe(TAG, "%s reason=too_large action=keep_live", event);
      return false;
  }
  if (wasHeld && !_hold.held) Log::error(TAG, "config.topic_prefix_hold_lost");
  if (!wasHeld && _hold.held) Log::info(TAG, "config.topic_prefix_deferred");
  configChanged();
  return true;
}

// Replace the whole config. A push that is not one object, or does not
// persist, leaves the live doc and its prefix hold as they were.
bool Config::replace(const char* json) {
  if (!json) return false;
  JsonDocument staged;
  DeserializationError err;
  ConfigTextReader in{json};
  configParseWhole(in, staged, err);
  if (err) {
    Log::kvfe(TAG, "config.replace_failed err=%s action=keep_live", err.c_str());
    return false;
  }
  configMigratePushed(staged, logSchemaStep);
  if (!commit(staged, "config.replace_failed")) return false;
  Log::info(TAG, "config.replaced source=mqtt");
  return true;
}

bool Config::rescueEdit(const std::function<bool(JsonObject)>& fn) {
  JsonDocument staged;
  DeserializationError err;
  LittleFsStageSource src;
  const long size = src.size();
  const ConfigStage from = configStageRescue(src, staged, err, CONFIG_SCHEMA_CURRENT);
  if (from != CONFIG_STAGE_FILE && from != CONFIG_STAGE_LIVE && from != CONFIG_STAGE_RESCUED) {
    const char* why = from == CONFIG_STAGE_NO_HEAP       ? "no_heap"
                      : from == CONFIG_STAGE_COPY_FAILED ? "copy_failed"
                      : from == CONFIG_STAGE_READ_FAILED ? "file_unreadable"
                                                         : "no_source";
    Log::kvfe(TAG, "config.edit_failed reason=%s err=%s action=keep_live", why, err ? err.c_str() : "none");
    return false;
  }
  if (!fn(staged.as<JsonObject>())) return false;
  if (!commit(staged, "config.edit_failed")) return false;
  if (from == CONFIG_STAGE_RESCUED) {
    const char* why = size < 0 ? "file_missing" : size > (long)CONFIG_FILE_MAX ? "too_large" : "file_unreadable";
    Log::kvfw(TAG, "config.rescued reason=%s err=%s action=replaced_file source=setup", why, err ? err.c_str() : "none");
  }
  return true;
}

bool Config::edit(const std::function<bool(JsonObject)>& fn, bool deferPrefix) {
  JsonDocument staged;
  DeserializationError err;
  LittleFsStageSource src;
  const ConfigStage from = configStage(src, staged, err);
  if (from != CONFIG_STAGE_FILE && from != CONFIG_STAGE_LIVE) {
    const char* why = from == CONFIG_STAGE_NO_LIVE    ? "no_source"
                      : from == CONFIG_STAGE_TOO_LARGE ? "too_large"
                      : from == CONFIG_STAGE_NO_HEAP   ? "no_heap"
                      : from == CONFIG_STAGE_COPY_FAILED ? "copy_failed"
                                                       : "file_unreadable";
    Log::kvfe(TAG, "config.edit_failed reason=%s err=%s action=keep_live", why, err ? err.c_str() : "none");
    return false;
  }
  if (!fn(staged.as<JsonObject>())) return false;
  return commit(staged, "config.edit_failed", deferPrefix);
}

// Keep `prefix` as the live topic prefix. The value already in the doc
// stays the one an edit writes.
// in: boot prefix. out: false if prefix is null, the disk value is not a
// string, or either string does not fit.
bool Config::holdTopicPrefix(const char* prefix) {
  switch (configHoldPrefix(_doc, _hold, prefix)) {
    case CONFIG_HOLD_OK:
      return true;
    case CONFIG_HOLD_NULL:
      Log::warn(TAG, "config.topic_prefix_hold_skipped reason=null");
      return false;
    case CONFIG_HOLD_TYPE:
      Log::warn(TAG, "config.topic_prefix_hold_skipped reason=type");
      return false;
    case CONFIG_HOLD_LENGTH:
      Log::warn(TAG, "config.topic_prefix_hold_skipped reason=length");
      return false;
  }
  return false;
}

bool Config::topicPrefixHeld() {
  return _hold.held;
}

bool Config::topicPrefixPending() {
  return configPrefixPending(_doc, _hold);
}

bool Config::hasLiveDoc() { return _doc.is<JsonObject>(); }

bool Config::copyDiskDoc(JsonDocument& dst) {
  dst.clear();
  // Through text: dst.set() deep-copies recursively at about 1 KB of stack
  // per nesting level and overflowed the 8 KB loop task on config.save.
  String text;
  if (serializeJson(_doc, text) == 0) return false;
  if (deserializeJson(dst, text, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH)) != DeserializationError::Ok) {
    return false;
  }
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

bool Config::keepStoredSecrets(JsonDocument& staged) {
  JsonDocument stored(secretJsonAllocator());
  LittleFsStageSource src;
  size_t n = 0;
  const ConfigKeep r = configKeepStoredSecrets(src, staged, stored, &n);
  if (r == CONFIG_KEEP_COPY_FAILED) {
    Log::error(TAG, "config.save_failed reason=copy_failed");
    return false;
  }
  if (r == CONFIG_KEEP_OK) Log::kvf(TAG, "config.mask_kept fields=%u", (unsigned)n);
  return true;
}

ConfigFileRead Config::readFileMasked(fs::FS& fs, const char* path, const char* shownPath, String& out) {
  out = String();
  File f = fs.open(path, "r");
  if (!f || f.isDirectory()) return CFG_READ_RAW;
  const bool family = configFileIsFamily(shownPath);
  if (!configFileNeedsMask(shownPath, f.peek())) {
    f.close();
    return CFG_READ_RAW;
  }
  const size_t size = f.size();
  if (size > CONFIG_FILE_MAX) {
    f.close();
    return family ? CFG_READ_REFUSE : CFG_READ_RAW;
  }
  char* buf = (char*)secretAlloc(size + 1);
  if (!buf) {
    f.close();
    return CFG_READ_REFUSE;
  }
  const size_t got = size ? f.read((uint8_t*)buf, size) : 0;
  f.close();
  ConfigFileRead r = CFG_READ_REFUSE;
  if (got == size) r = configFileReadMasked(shownPath, buf, size, out, secretJsonAllocator());
  secretFree(buf);
  return r;
}
