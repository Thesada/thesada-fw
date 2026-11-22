// thesada-fw - config_mask_policy.h
// Which config fields are secrets, how every read path masks them, and how a
// mask written back keeps the stored value. Pure, host-tested.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <stddef.h>
#include <string.h>
#include "config_bounds.h"
#include "config_load_policy.h"
#include "config_path_policy.h"

static const char CONFIG_MASK_SET[] = "(set)";
static const char CONFIG_MASK_EMPTY[] = "(empty)";

// in: a and its length, b. out: true when a ends with b, ASCII case-blind.
inline bool configMaskEndsWith(const char* a, size_t n, const char* b) {
  const size_t m = strlen(b);
  if (m > n) return false;
  for (size_t i = 0; i < m; i++) {
    char c = a[n - m + i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != b[i]) return false;
  }
  return true;
}

// in: one key name, length n. out: true when its value is a secret. Long words
// match as a suffix, short ones as the whole key or after _ . -, so pin and
// heartbeat_pin (GPIO numbers) stay readable and sim_pin does not.
inline bool configKeyIsSecretN(const char* key, size_t n) {
  if (!key || n == 0) return false;
  static const char* const kLong[] = {"password", "passwd", "secret", "token", "key",
                                      "passphrase", "credential", "claim_code"};
  static const char* const kShort[] = {"pass", "pwd", "psk", "seed", "sim_pin"};
  for (const char* w : kLong) {
    if (configMaskEndsWith(key, n, w)) return true;
  }
  for (const char* w : kShort) {
    const size_t m = strlen(w);
    if (!configMaskEndsWith(key, n, w)) continue;
    if (n == m) return true;
    const char c = key[n - m - 1];
    if (c == '_' || c == '.' || c == '-') return true;
  }
  return false;
}

inline bool configKeyIsSecret(const char* key) { return key && configKeyIsSecretN(key, strlen(key)); }

// in: dotted config path. out: true when any segment is a secret key, so
// everything at or below it is masked.
inline bool configPathIsSecret(const char* path) {
  if (!path) return false;
  const char* seg = path;
  for (const char* p = path;; ++p) {
    if (*p == '.' || *p == '\0') {
      if (configKeyIsSecretN(seg, (size_t)(p - seg))) return true;
      if (*p == '\0') return false;
      seg = p + 1;
    }
  }
}

inline bool configIsMaskMarker(const char* s) {
  return s && (strcmp(s, CONFIG_MASK_SET) == 0 || strcmp(s, CONFIG_MASK_EMPTY) == 0);
}

// in: a non-null secret value. out: the marker it reads as.
inline const char* configMaskFor(JsonVariantConst v) {
  const char* s = v.as<const char*>();
  return (v.is<const char*>() && s && !*s) ? CONFIG_MASK_EMPTY : CONFIG_MASK_SET;
}

// Replace every secret value under v with its marker, in place.
// out: values masked. Depth is capped by CONFIG_MAX_DEPTH at parse.
// NOLINTNEXTLINE(misc-no-recursion)
inline size_t configMaskTree(JsonVariant v) {
  size_t n = 0;
  if (v.is<JsonObject>()) {
    for (JsonPair kv : v.as<JsonObject>()) {
      if (configKeyIsSecret(kv.key().c_str()) && !kv.value().isNull()) {
        kv.value().set(configMaskFor(kv.value()));
        n++;
      } else {
        n += configMaskTree(kv.value());
      }
    }
  } else if (v.is<JsonArray>()) {
    for (JsonVariant e : v.as<JsonArray>()) n += configMaskTree(e);
  }
  return n;
}

// in: JSON bytes, length. out: ':' outside strings, one per object member.
inline size_t configJsonMemberMarks(const char* data, size_t len) {
  size_t n = 0;
  char quote = 0;  // ArduinoJson also takes single-quoted strings
  for (size_t i = 0; data && i < len; i++) {
    const char c = data[i];
    if (quote) {
      if (c == '\\') i++;
      else if (c == quote) quote = 0;
    } else if (c == '"' || c == '\'') {
      quote = c;
    } else if (c == ':') {
      n++;
    }
  }
  return n;
}

// out: object members at any depth under v.
// NOLINTNEXTLINE(misc-no-recursion)
inline size_t configTreeMembers(JsonVariantConst v) {
  size_t n = 0;
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) n += 1 + configTreeMembers(kv.value());
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) n += configTreeMembers(e);
  }
  return n;
}

// A duplicate key collapses to one member in the parse, so the bytes say more
// than the tree. in: bytes, length, their parse. out: true when no member was lost.
inline bool configParseKeptMembers(const char* data, size_t len, JsonVariantConst v) {
  return configJsonMemberMarks(data, len) == configTreeMembers(v);
}

// The read form of the node at path (config.get, config.dump, Lua). The live
// doc is copied through text, never changed. out: false when the copy did not fit.
template <typename S>
inline bool configMaskedNodeText(const char* path, JsonVariantConst node, S& out, bool pretty,
                                 ArduinoJson::Allocator* alloc = nullptr) {
  out = S();
  JsonDocument doc(alloc ? alloc : ArduinoJson::detail::DefaultAllocator::instance());
  if (configPathIsSecret(path) && !node.isNull()) {
    doc.set(configMaskFor(node));
  } else {
    S text;
    serializeJson(node, text);
    if (deserializeJson(doc, text, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH))) return false;
    if (configMaskTree(doc.as<JsonVariant>()) == 0) {
      if (pretty) serializeJsonPretty(node, out); else serializeJson(node, out);
      return true;
    }
  }
  if (pretty) serializeJsonPretty(doc, out); else serializeJson(doc, out);
  return true;
}

// in: bytes, length. out: true when a marker string appears anywhere.
inline bool configTextHasMarker(const char* data, size_t len) {
  if (!data) return false;
  const char* const marks[] = {CONFIG_MASK_SET, CONFIG_MASK_EMPTY};
  for (const char* m : marks) {
    const size_t ml = strlen(m);
    for (size_t i = 0; i + ml <= len; i++) {
      if (memcmp(data + i, m, ml) == 0) return true;
    }
  }
  return false;
}

// out: true when a secret key under v holds a marker.
// NOLINTNEXTLINE(misc-no-recursion)
inline bool configHasMaskMarker(JsonVariantConst v) {
  if (v.is<JsonObjectConst>()) {
    for (JsonPairConst kv : v.as<JsonObjectConst>()) {
      if (configKeyIsSecret(kv.key().c_str()) && configIsMaskMarker(kv.value().as<const char*>())) return true;
      if (configHasMaskMarker(kv.value())) return true;
    }
  } else if (v.is<JsonArrayConst>()) {
    for (JsonVariantConst e : v.as<JsonArrayConst>()) {
      if (configHasMaskMarker(e)) return true;
    }
  }
  return false;
}

// The stored element an array element of staged stands for: the one with the
// same ssid when it has one, else the same index.
inline JsonVariantConst configKeepCounterpart(JsonVariantConst e, JsonArrayConst stored, size_t i) {
  const char* ssid = e["ssid"].as<const char*>();
  if (!ssid) return stored[i];
  for (JsonVariantConst s : stored) {
    const char* other = s["ssid"].as<const char*>();
    if (other && strcmp(other, ssid) == 0) return s;
  }
  return JsonVariantConst();
}

// A marker under a secret key in staged takes the stored value at the same
// place, or is dropped when there is none. out: markers resolved, *failed set
// when a stored value could not be copied in.
// NOLINTNEXTLINE(misc-no-recursion)
inline size_t configKeepMasked(JsonVariant staged, JsonVariantConst stored, bool* failed = nullptr) {
  size_t n = 0;
  if (staged.is<JsonObject>()) {
    JsonObject obj = staged.as<JsonObject>();
    for (bool again = true; again;) {
      again = false;
      for (JsonPair kv : obj) {
        const char* k = kv.key().c_str();
        if (!configKeyIsSecret(k) || !configIsMaskMarker(kv.value().as<const char*>())) continue;
        n++;
        JsonVariantConst was = stored[k];
        if (was.isNull()) {
          obj.remove(k);
          again = true;
          break;
        }
        if (!kv.value().set(was) && failed) *failed = true;
      }
    }
    for (JsonPair kv : obj) n += configKeepMasked(kv.value(), stored[kv.key().c_str()], failed);
  } else if (staged.is<JsonArray>()) {
    JsonArrayConst st = stored.as<JsonArrayConst>();
    size_t i = 0;
    for (JsonVariant e : staged.as<JsonArray>()) {
      n += configKeepMasked(e, configKeepCounterpart(e, st, i), failed);
      i++;
    }
  }
  return n;
}

enum ConfigKeep {
  CONFIG_KEEP_NONE = 0,     // no marker under a secret key, nothing to keep
  CONFIG_KEEP_OK,           // markers resolved against the stored config
  CONFIG_KEEP_COPY_FAILED,  // a live doc holds the secrets but could not be copied
  CONFIG_KEEP_SET_FAILED,   // a stored value did not fit staged, staged cleared
};

// Resolve the markers in staged against the file, else the live doc. Without
// either the markers are dropped. A live doc that could not be copied, or a
// stored value that did not fit, refuses. Src: parse(doc, err), copyLive(doc), hasLive().
template <typename Src>
inline ConfigKeep configKeepStoredSecrets(Src& src, JsonDocument& staged, JsonDocument& stored, size_t* kept = nullptr) {
  if (kept) *kept = 0;
  if (!configHasMaskMarker(staged.as<JsonVariantConst>())) return CONFIG_KEEP_NONE;
  stored.clear();
  DeserializationError err;
  const bool fileOk = src.parse(stored, err) && !err && stored.is<JsonObject>();
  if (!fileOk) {
    stored.clear();
    if (!src.copyLive(stored) || !stored.is<JsonObject>()) {
      stored.clear();
      if (src.hasLive()) return CONFIG_KEEP_COPY_FAILED;
    }
  }
  bool failed = false;
  const size_t n = configKeepMasked(staged.as<JsonVariant>(), stored.as<JsonVariantConst>(), &failed);
  if (failed) {
    staged.clear();
    return CONFIG_KEEP_SET_FAILED;
  }
  if (kept) *kept = n;
  return CONFIG_KEEP_OK;
}

// config.set key value: true when value is a marker for a secret, so the
// stored secret stays and nothing is written. An invalid key is not kept, config.set
// refuses it. A literal goes through configKeepMasked.
inline bool configSetKeepsSecret(const char* key, const char* value) {
  return key && configKeyValid(key) && configPathIsSecret(key) && configIsMaskMarker(value);
}

// in: path, any spelling. out: true when its last segment names the config
// file or a copy of it (bak, tmp, SD backup), case-blind, with trailing / . and space dropped.
inline bool configFileIsFamily(const char* path) {
  if (!path) return false;
  size_t n = strlen(path);
  while (n > 0 && (path[n - 1] == '/' || path[n - 1] == '.' || path[n - 1] == ' ')) n--;
  size_t b = n;
  while (b > 0 && path[b - 1] != '/') b--;
  const char* base = path + b;
  const size_t bl = n - b;
  static const char kHead[] = "config";
  static const char kJson[] = ".json";
  if (bl < sizeof(kHead) - 1 + sizeof(kJson) - 1) return false;
  for (size_t i = 0; i < sizeof(kHead) - 1; i++) {
    char c = base[i];
    if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
    if (c != kHead[i]) return false;
  }
  const char next = base[sizeof(kHead) - 1];
  if (next != '.' && next != '_') return false;
  for (size_t i = sizeof(kHead) - 1; i + sizeof(kJson) - 1 <= bl; i++) {
    if (configMaskEndsWith(base, i + sizeof(kJson) - 1, kJson)) return true;
  }
  return false;
}

// in: path, first non-space byte of the file (-1 when none). out: true when a
// reader must buffer it and run configFileReadMasked instead of streaming it.
inline bool configFileNeedsMask(const char* path, int first) {
  return configFileIsFamily(path) || first == '{';
}

// in: reader with read() -> byte or -1. out: first byte that is not JSON whitespace, -1 at the end.
template <typename Reader>
inline int configFirstNonSpace(Reader& in) {
  for (int c; (c = in.read()) >= 0;) {
    if (!configIsJsonSpace((char)c)) return c;
  }
  return -1;
}

enum ConfigPreRead {
  CFG_PRE_RAW = 0,  // stream the file, the mask does not cover it
  CFG_PRE_REFUSE,   // serve nothing
  CFG_PRE_BUFFER,   // read it whole and run configFileReadMasked
};

// What a read does before any byte is buffered. A covered file that failed to
// open, or does not fit the buffer, is refused, never streamed by the caller.
// in: shown path, opened, exists, first non-space byte, size. out: verdict.
inline ConfigPreRead configFilePreRead(const char* path, bool opened, bool exists, int first, size_t size) {
  if (!opened) return exists && configFileIsFamily(path) ? CFG_PRE_REFUSE : CFG_PRE_RAW;
  if (!configFileNeedsMask(path, first)) return CFG_PRE_RAW;
  return size > CONFIG_FILE_MAX ? CFG_PRE_REFUSE : CFG_PRE_BUFFER;
}

enum ConfigFileRead {
  CFG_READ_RAW = 0,  // serve the bytes as they are: no secret in them
  CFG_READ_MASKED,   // serve out instead
  CFG_READ_REFUSE,   // config-named but not one object, content after the object, or no heap or depth: serve nothing
};

// What a file read (fs.cat, /api/file, /api/config, backup) serves.
// in: path, bytes, length. out: verdict, out filled when MASKED.
template <typename S>
inline ConfigFileRead configFileReadMasked(const char* path, const char* data, size_t len, S& out,
                                           ArduinoJson::Allocator* alloc = nullptr) {
  out = S();
  const bool family = configFileIsFamily(path);
  if (len == 0) return CFG_READ_RAW;
  if (!data) return CFG_READ_REFUSE;
  JsonDocument doc(alloc ? alloc : ArduinoJson::detail::DefaultAllocator::instance());
  const DeserializationError err =
      deserializeJson(doc, data, len, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH));
  // Heap or depth left the object uninspected: refused, never served raw.
  if (err == DeserializationError::NoMemory || err == DeserializationError::TooDeep) return CFG_READ_REFUSE;
  if (err || !doc.is<JsonObject>()) return family ? CFG_READ_REFUSE : CFG_READ_RAW;
  if (!configNothingAfterObject(data, len)) return CFG_READ_REFUSE;
  const bool whole = configParseKeptMembers(data, len, doc.as<JsonVariantConst>());
  if (configMaskTree(doc.as<JsonVariant>()) == 0 && whole) return CFG_READ_RAW;
  serializeJsonPretty(doc, out);
  return CFG_READ_MASKED;
}

enum ConfigRawWrite {
  CFG_RAW_NOT_OBJECT = 0,  // refused, nothing written
  CFG_RAW_AS_IS,           // no marker: the bytes are written as sent
  CFG_RAW_KEEP,            // a marker: keep the stored secrets, rewrite from doc
};

// How fs.write bytes become /config.json. Markers are found in the decoded
// values, so an escaped spelling keeps the secret too. in: doc, bytes, length.
inline ConfigRawWrite configRawWritePlan(JsonDocument& doc, const char* data, size_t len) {
  if (!configParseObjectBytes(doc, data, len)) return CFG_RAW_NOT_OBJECT;
  return configHasMaskMarker(doc.as<JsonVariantConst>()) ? CFG_RAW_KEEP : CFG_RAW_AS_IS;
}
