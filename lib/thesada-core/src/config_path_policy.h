// thesada-fw - config_path_policy.h
// Dotted-key set and delete on a config doc, run by shell config.set and
// config.del inside Config::edit. Pure, host-tested.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "config_bounds.h"
#include "config_scalar_policy.h"

// Longest key config.set and config.get take, NUL included.
constexpr size_t CONFIG_KEY_MAX = 128;

// in: token. out: true and idx set when tok is all digits, at least one, and fits size_t.
inline bool configArrayIndex(const char* tok, size_t& idx) {
  if (!tok || !*tok) return false;
  size_t v = 0;
  for (const char* p = tok; *p; ++p) {
    if (*p < '0' || *p > '9') return false;
    const size_t d = (size_t)(*p - '0');
    if (v > (SIZE_MAX - d) / 10) return false;
    v = v * 10 + d;
  }
  idx = v;
  return true;
}

// in: dotted key. out: false when empty or a segment is empty (leading,
// trailing or doubled dot). Checked before any walk: strtok skips empty segments.
inline bool configKeyValid(const char* key) {
  if (!key || !*key || *key == '.') return false;
  for (const char* p = key; *p; ++p) {
    if (*p == '.' && (p[1] == '.' || p[1] == '\0')) return false;
  }
  return true;
}

enum ConfigStore {
  CONFIG_STORE_OK = 0,
  CONFIG_STORE_TOO_DEEP,
  CONFIG_STORE_NO_HEAP,
};

// Store value in target: a JSON literal when it parses within depth levels,
// else a bool, number or the string as typed. A literal that runs out of heap
// is refused, never stored as its text. alloc: the literal's, null for the default.
inline ConfigStore configPathStore(JsonVariant target, const char* value, int depth,
                                   ArduinoJson::Allocator* alloc = nullptr) {
  const char* v = value;
  while (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n') v++;
  if (*v == '{' || *v == '[') {
    JsonDocument lit(alloc ? alloc : ArduinoJson::detail::DefaultAllocator::instance());
    const DeserializationError err = deserializeJson(lit, v, DeserializationOption::NestingLimit(depth));
    if (err == DeserializationError::TooDeep) return CONFIG_STORE_TOO_DEEP;
    if (err == DeserializationError::NoMemory) return CONFIG_STORE_NO_HEAP;
    if (!err) return target.set(lit.as<JsonVariant>()) ? CONFIG_STORE_OK : CONFIG_STORE_NO_HEAP;
  }
  bool bv;
  long long iv;
  float fv;
  switch (configScalarKind(value, &bv, &iv, &fv)) {
    case CFG_SCALAR_BOOL:  target.set(bv); break;
    case CFG_SCALAR_INT:   target.set(iv); break;
    case CFG_SCALAR_FLOAT: target.set(fv); break;
    default:               target.set(value); break;
  }
  return CONFIG_STORE_OK;
}

// Set key to value, or delete it when value is null. Missing parent objects
// are created, never arrays. An array slot is nulled, so sibling indices stay.
// in: root, dotted key, value or null, msg buffer, literal allocator or null.
// out: false with msg set when refused.
inline bool configPathApply(JsonObject root, const char* keyIn, const char* value, char* msg, size_t cap,
                            ArduinoJson::Allocator* alloc = nullptr) {
  msg[0] = '\0';
  char key[CONFIG_KEY_MAX];
  const size_t klen = strnlen(keyIn, sizeof(key));
  if (klen >= sizeof(key)) {
    (void)snprintf(msg, cap, "Key too long (max %u bytes)", (unsigned)(sizeof(key) - 1));
    return false;
  }
  memcpy(key, keyIn, klen + 1);
  if (!configKeyValid(key)) {
    (void)snprintf(msg, cap, "Invalid key (empty segment)");
    return false;
  }
  int segs = 1;
  for (const char* p = key; *p; ++p) segs += *p == '.';
  if (segs > CONFIG_MAX_DEPTH) {
    (void)snprintf(msg, cap, "Key deeper than %d levels", CONFIG_MAX_DEPTH);
    return false;
  }
  char* lastDot = strrchr(key, '.');
  if (!lastDot) {
    if (value) {
      (void)snprintf(msg, cap, "Cannot set top-level keys directly (use section.key format)");
      return false;
    }
    if (root[key].isNull()) {
      (void)snprintf(msg, cap, "Key not found");
      return false;
    }
    root.remove(key);
    return true;
  }
  *lastDot = '\0';
  const char* finalKey = lastDot + 1;
  char parentPath[CONFIG_KEY_MAX];
  memcpy(parentPath, key, strlen(key) + 1);

  JsonVariant parent = root;
  size_t walked = 0;  // bytes of parentPath resolved so far
  char* saveptr = nullptr;
  char* token = strtok_r(key, ".", &saveptr);
  while (token) {
    char* next = strtok_r(nullptr, ".", &saveptr);
    size_t idx;
    if (parent.is<JsonArray>() && configArrayIndex(token, idx)) {
      JsonArray arr = parent.as<JsonArray>();
      if (idx >= arr.size()) {
        (void)snprintf(msg, cap, "Parent array index out of range");
        return false;
      }
      parent = arr[idx];
    } else if (parent.is<JsonObject>()) {
      walked = (size_t)(token - key) + strlen(token);
      if (parent[token].isNull()) {
        // A numeric next segment wants an array here: refuse, never create one.
        if (next && configArrayIndex(next, idx)) {
          const int n = (int)walked;
          (void)snprintf(msg, cap, "Parent path '%.*s' doesn't exist. Run config.set %.*s [] first.", n,
                         parentPath, n, parentPath);
          return false;
        }
        parent[token].to<JsonObject>();
      }
      parent = parent[token];
    } else {
      (void)snprintf(msg, cap, "Parent key not found");
      return false;
    }
    token = next;
  }

  size_t finalIdx;
  const bool numeric = configArrayIndex(finalKey, finalIdx);
  if (numeric && !parent.is<JsonArray>()) {
    (void)snprintf(msg, cap, "Parent path '%s' is not an array. Run config.set %s [] first.", parentPath, parentPath);
    return false;
  }
  JsonVariant target;
  if (numeric) {
    JsonArray arr = parent.as<JsonArray>();
    if (!value) {
      if (finalIdx >= arr.size()) {
        (void)snprintf(msg, cap, "Array index out of range");
        return false;
      }
      arr[finalIdx].clear();
      return true;
    }
    if (finalIdx > arr.size()) {
      (void)snprintf(msg, cap, "Array index out of range (would leave hole)");
      return false;
    }
    target = finalIdx == arr.size() ? arr.add<JsonVariant>() : JsonVariant(arr[finalIdx]);
  } else if (parent.is<JsonObject>()) {
    if (!value) {
      parent.as<JsonObject>().remove(finalKey);
      return true;
    }
    // An unbound member takes no value: materialise it first.
    if (parent[finalKey].isNull()) parent[finalKey].to<JsonObject>();
    target = parent[finalKey];
  } else {
    (void)snprintf(msg, cap, "Parent is not an object or array");
    return false;
  }
  switch (configPathStore(target, value, CONFIG_MAX_DEPTH - segs, alloc)) {
    case CONFIG_STORE_OK:
      return true;
    case CONFIG_STORE_TOO_DEEP:
      (void)snprintf(msg, cap, "Value nested past %d levels", CONFIG_MAX_DEPTH);
      return false;
    case CONFIG_STORE_NO_HEAP:
      break;
  }
  (void)snprintf(msg, cap, "Value out of memory");
  return false;
}
