// thesada-fw - config_load_policy.h
// Which document stays live when /config.json is read, and what a config file
// must be before it replaces the one on disk. Pure, host-tested.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <string.h>
#include <utility>
#include "config_bounds.h"

enum ConfigLoad {
  CONFIG_LOAD_OK = 0,
  CONFIG_LOAD_MOUNT_FAILED,
  CONFIG_LOAD_FILE_MISSING,
  CONFIG_LOAD_PARSE_FAILED,  // malformed, or valid JSON that is not one object
  CONFIG_LOAD_TOO_LARGE,     // past CONFIG_FILE_MAX, not parsed
};

// Parse the config file into a fresh doc and move it into live only when it is
// one JSON object. Every failure leaves live as it was: at boot that is the
// empty doc, on a reload the last good one.
// Fs: mount() -> bool, size() -> bytes or -1 when missing,
// parse(JsonDocument&, DeserializationError&) -> bool, false when the file cannot be opened.
// in: fs, live doc, error out. out: result, err set on a parse failure.
template <typename Fs>
inline ConfigLoad configLoadInto(Fs& fs, JsonDocument& live, DeserializationError& err) {
  err = DeserializationError::Ok;
  if (!fs.mount()) return CONFIG_LOAD_MOUNT_FAILED;
  const long size = fs.size();
  if (size < 0) return CONFIG_LOAD_FILE_MISSING;
  if ((size_t)size > CONFIG_FILE_MAX) return CONFIG_LOAD_TOO_LARGE;
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
    case CONFIG_LOAD_TOO_LARGE:    return "config.too_large";
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

// Parse a config stream that must hold one value and nothing after it but JSON
// whitespace. Trailing content is InvalidInput, past CONFIG_MAX_DEPTH TooDeep.
template <typename Reader>
inline void configParseWhole(Reader& in, JsonDocument& doc, DeserializationError& err) {
  err = deserializeJson(doc, in, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH));
  if (err) return;
  for (int c; (c = in.read()) >= 0;) {
    if (!configIsJsonSpace((char)c)) {
      err = DeserializationError::InvalidInput;
      return;
    }
  }
}

// Config bytes (fs.write, /api/config, /api/file) parsed into doc: one JSON
// object and nothing after it. in: doc, bytes, length. out: true when it is.
inline bool configParseObjectBytes(JsonDocument& doc, const char* data, size_t len) {
  if (!data || len == 0 || len > CONFIG_FILE_MAX) return false;
  if (deserializeJson(doc, data, len, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH))) return false;
  return doc.is<JsonObject>() && configNothingAfterObject(data, len);
}

// in: bytes, length. out: true when they are one config object.
inline bool configBytesAreObject(const char* data, size_t len) {
  JsonDocument doc;
  return configParseObjectBytes(doc, data, len);
}

// A NUL-terminated config text as a stream for configParseWhole.
struct ConfigTextReader {
  const char* p;
  int read() { return *p ? (unsigned char)*p++ : -1; }
  size_t readBytes(char* buf, size_t n) {
    size_t i = 0;
    for (int c; i < n && (c = read()) >= 0; i++) buf[i] = (char)c;
    return i;
  }
};

// Where an edit's working copy came from. PARSE_FAILED covers malformed,
// trailing content and not one object, NO_LIVE a missing file and no live doc,
// NO_HEAP a file that ran out of heap while it was parsed, COPY_FAILED a missing
// file and a live doc that could not be copied.
enum ConfigStage {
  CONFIG_STAGE_FILE = 0,
  CONFIG_STAGE_LIVE,
  CONFIG_STAGE_PARSE_FAILED,
  CONFIG_STAGE_NO_LIVE,
  CONFIG_STAGE_TOO_LARGE,
  CONFIG_STAGE_RESCUED,
  CONFIG_STAGE_NO_HEAP,
  CONFIG_STAGE_COPY_FAILED,
};

// Stage an edit from the file, the live doc only when the file is missing.
// Fs: size() -> bytes or -1, parse(doc, err), copyLive(doc) -> bool, hasLive(). out: source.
template <typename Fs>
inline ConfigStage configStage(Fs& fs, JsonDocument& staged, DeserializationError& err) {
  err = DeserializationError::Ok;
  staged.clear();
  const long size = fs.size();
  if (size > (long)CONFIG_FILE_MAX) return CONFIG_STAGE_TOO_LARGE;
  if (size >= 0 && fs.parse(staged, err)) {
    if (err == DeserializationError::NoMemory) return CONFIG_STAGE_NO_HEAP;
    return err || !staged.is<JsonObject>() ? CONFIG_STAGE_PARSE_FAILED : CONFIG_STAGE_FILE;
  }
  staged.clear();
  if (fs.copyLive(staged) && staged.is<JsonObject>()) return CONFIG_STAGE_LIVE;
  staged.clear();
  return fs.hasLive() ? CONFIG_STAGE_COPY_FAILED : CONFIG_STAGE_NO_LIVE;
}

// configStage for the setup portal, the way back from a broken file: a file that
// does not parse, is not one object or is too large stages {"schema": schema},
// and so does a blank filesystem (no file, no live doc). NO_HEAP is not rescued.
template <typename Fs>
inline ConfigStage configStageRescue(Fs& fs, JsonDocument& staged, DeserializationError& err, int schema) {
  const ConfigStage from = configStage(fs, staged, err);
  const bool blank = from == CONFIG_STAGE_NO_LIVE && fs.size() < 0;
  if (from != CONFIG_STAGE_PARSE_FAILED && from != CONFIG_STAGE_TOO_LARGE && !blank) return from;
  staged.clear();
  staged["schema"] = schema;
  return CONFIG_STAGE_RESCUED;
}

// Largest topic prefix the boot-prefix hold keeps.
constexpr size_t CONFIG_PREFIX_CAP = 128;

// The boot prefix overlaid on the live doc while /config.json holds another.
struct ConfigPrefixHold {
  char disk[CONFIG_PREFIX_CAP];
  bool diskPresent;
  bool held;
};

// TYPE: the doc's mqtt.topic_prefix is not a string. LENGTH: a prefix does not fit.
enum ConfigHold {
  CONFIG_HOLD_OK = 0,
  CONFIG_HOLD_NULL,
  CONFIG_HOLD_TYPE,
  CONFIG_HOLD_LENGTH,
};

// Keep the doc's own prefix as the disk one and put prefix over it.
// in: doc as written, hold, boot prefix. out: result, hold unchanged on failure.
inline ConfigHold configHoldPrefix(JsonDocument& doc, ConfigPrefixHold& hold, const char* prefix) {
  if (!prefix) return CONFIG_HOLD_NULL;
  // A scalar or array mqtt takes no member: the overlay would not land.
  JsonVariantConst mqtt = doc.as<JsonObjectConst>()["mqtt"];
  if (!mqtt.isNull() && !mqtt.is<JsonObjectConst>()) return CONFIG_HOLD_TYPE;
  JsonVariantConst v = mqtt["topic_prefix"];
  // Missing stays missing. null, numbers, and objects are not strings.
  if (!v.isUnbound() && !v.is<const char*>()) return CONFIG_HOLD_TYPE;
  const bool  present = v.is<const char*>();
  const char* disk    = present ? v.as<const char*>() : "";
  if (!disk) return CONFIG_HOLD_NULL;
  const size_t diskLen = strlen(disk);
  if (diskLen >= sizeof(hold.disk) || strlen(prefix) >= sizeof(hold.disk)) return CONFIG_HOLD_LENGTH;
  memmove(hold.disk, disk, diskLen + 1);
  hold.diskPresent            = present;
  hold.held                   = true;
  doc["mqtt"]["topic_prefix"] = prefix;
  return CONFIG_HOLD_OK;
}

// Hold boot over doc. No room for the overlay: the doc's own prefix stays
// live, put back after the failed assignment cleared it. out: the hold, unheld then.
inline ConfigPrefixHold configHoldOrKeep(JsonDocument& doc, const char* boot) {
  ConfigPrefixHold next = ConfigPrefixHold();
  if (configHoldPrefix(doc, next, boot) == CONFIG_HOLD_OK && doc.overflowed()) {
    JsonObject mqtt = doc["mqtt"];
    if (next.diskPresent) mqtt["topic_prefix"] = next.disk;
    else mqtt.remove("topic_prefix");
    next = ConfigPrefixHold();
  }
  return next;
}

// NO_HEAP: the staged doc overflowed while it was built. TOO_LARGE: past CONFIG_FILE_MAX.
enum ConfigCommit {
  CONFIG_COMMIT_OK = 0,
  CONFIG_COMMIT_NOT_OBJECT,
  CONFIG_COMMIT_NO_HEAP,
  CONFIG_COMMIT_PERSIST_FAILED,
  CONFIG_COMMIT_TOO_LARGE,
};

// The one path into live: refuse a non-object or overflowed doc before any
// write, write it, re-hold the boot prefix, move it in. No copy of live.
template <typename Write>
inline ConfigCommit configCommit(JsonDocument& live, JsonDocument& staged, Write write,
                                 ConfigPrefixHold* hold = nullptr) {
  if (!staged.is<JsonObject>()) return CONFIG_COMMIT_NOT_OBJECT;
  if (staged.overflowed()) return CONFIG_COMMIT_NO_HEAP;
  if (measureJson(staged) > CONFIG_FILE_MAX) return CONFIG_COMMIT_TOO_LARGE;
  char boot[CONFIG_PREFIX_CAP];
  bool keep = false;
  if (hold && hold->held) {
    const char*  p = live.as<JsonObjectConst>()["mqtt"]["topic_prefix"] | "";
    const size_t n = strlen(p);
    keep           = n < sizeof(boot);
    if (keep) memcpy(boot, p, n + 1);
  }
  if (!write(staged.as<JsonVariantConst>())) return CONFIG_COMMIT_PERSIST_FAILED;
  if (hold) *hold = keep ? configHoldOrKeep(staged, boot) : ConfigPrefixHold();
  live = std::move(staged);
  return CONFIG_COMMIT_OK;
}

// The prefix a doc without mqtt.topic_prefix runs under.
constexpr const char* CONFIG_PREFIX_DEFAULT = "thesada/node";

// configCommit for a shell edit of the prefix: an edit that moves the live
// prefix writes the file and holds the old one live until restart.
// in: live, staged, write, hold. out: result, hold taken when the prefix moved.
template <typename Write>
inline ConfigCommit configCommitDeferPrefix(JsonDocument& live, JsonDocument& staged, Write write,
                                            ConfigPrefixHold& hold) {
  if (hold.held) return configCommit(live, staged, write, &hold);
  char boot[CONFIG_PREFIX_CAP];
  const char*  p    = live.as<JsonObjectConst>()["mqtt"]["topic_prefix"] | CONFIG_PREFIX_DEFAULT;
  const size_t n    = strlen(p);
  const bool   fits = n < sizeof(boot);
  if (fits) memcpy(boot, p, n + 1);
  const ConfigCommit r = configCommit(live, staged, write, &hold);
  if (r != CONFIG_COMMIT_OK || !fits) return r;
  if (strcmp(boot, live.as<JsonObjectConst>()["mqtt"]["topic_prefix"] | CONFIG_PREFIX_DEFAULT) == 0) return r;
  hold = configHoldOrKeep(live, boot);
  return r;
}

// in: live doc, hold. out: true while the file's prefix is not the live one.
inline bool configPrefixPending(const JsonDocument& live, const ConfigPrefixHold& hold) {
  if (!hold.held) return false;
  const char* disk = hold.diskPresent ? hold.disk : CONFIG_PREFIX_DEFAULT;
  return strcmp(disk, live.as<JsonObjectConst>()["mqtt"]["topic_prefix"] | CONFIG_PREFIX_DEFAULT) != 0;
}
