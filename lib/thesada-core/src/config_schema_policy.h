// thesada-fw - config_schema_policy.h
// Ordered config.json schema migrations: pure, idempotent, host-tested.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <ArduinoJson.h>
#include <string.h>
#include <string>
#include <utility>
#include <vector>

// Bump with every new migration and append it to CONFIG_MIGRATIONS.
static const int CONFIG_SCHEMA_CURRENT = 1;

enum ConfigSchemaVerdict {
  CFG_SCHEMA_CURRENT,   // already current, nothing to do
  CFG_SCHEMA_MIGRATED,  // doc brought up to CONFIG_SCHEMA_CURRENT
  CFG_SCHEMA_NEWER,     // written by newer firmware, left untouched
  CFG_SCHEMA_INVALID,   // "schema" is not a whole number >= 0, left untouched
};

// Schema of a parsed config: absent is 0, -1 when it is not a whole number >= 0.
inline int configSchemaVersion(JsonVariantConst doc) {
  JsonVariantConst v = doc["schema"];
  if (v.isNull()) return 0;
  if (!v.is<int>()) return -1;
  const int n = v.as<int>();
  return n < 0 ? -1 : n;
}

// 0 -> 1: drop or rename keys saved with a trailing colon ("password:", 2026-04 OWB). out: keys changed.
// NOLINTNEXTLINE(misc-no-recursion)
inline size_t configDropColonKeys(JsonVariant node) {
  size_t changed = 0;
  if (node.is<JsonArray>()) {
    for (JsonVariant item : node.as<JsonArray>()) changed += configDropColonKeys(item);
    return changed;
  }
  if (!node.is<JsonObject>()) return 0;
  JsonObject obj = node.as<JsonObject>();
  std::vector<std::string> colon;
  for (JsonPair kv : obj) {
    const char* k = kv.key().c_str();
    const size_t n = strlen(k);
    if (n > 1 && k[n - 1] == ':') colon.push_back(k);
  }
  for (const std::string& k : colon) {
    const std::string plain = k.substr(0, k.size() - 1);
    if (obj[plain].isNull()) obj[plain] = obj[k];
    obj.remove(k);
    changed++;
  }
  for (JsonPair kv : obj) changed += configDropColonKeys(kv.value());
  return changed;
}

struct ConfigMigration {
  int         from;   // applies to a doc at this schema, leaves it at from + 1
  const char* name;
  size_t    (*apply)(JsonVariant doc);
};

static const ConfigMigration CONFIG_MIGRATIONS[] = {
  {0, "drop_colon_keys", configDropColonKeys},
};

// in: parsed doc, step callback per migration run. out: verdict. Only a
// CFG_SCHEMA_MIGRATED doc is changed, and it then carries "schema" = current.
inline ConfigSchemaVerdict configMigrate(JsonDocument& doc,
                                         void (*step)(int from, const char* name, size_t changed)) {
  const int v = configSchemaVersion(doc.as<JsonVariantConst>());
  if (v < 0) return CFG_SCHEMA_INVALID;
  if (v > CONFIG_SCHEMA_CURRENT) return CFG_SCHEMA_NEWER;
  if (v == CONFIG_SCHEMA_CURRENT) return CFG_SCHEMA_CURRENT;
  for (const ConfigMigration& m : CONFIG_MIGRATIONS) {
    if (m.from < v) continue;
    const size_t n = m.apply(doc.as<JsonVariant>());
    if (step) step(m.from, m.name, n);
  }
  doc["schema"] = CONFIG_SCHEMA_CURRENT;
  return CFG_SCHEMA_MIGRATED;
}

enum ConfigMigrateResult {
  CFG_MIGRATE_NONE,           // current, nothing written
  CFG_MIGRATE_REREAD_FAILED,  // the file did not parse a second time
  CFG_MIGRATE_NEWER,
  CFG_MIGRATE_INVALID,
  CFG_MIGRATE_BACKUP_FAILED,
  CFG_MIGRATE_WRITE_FAILED,
  CFG_MIGRATE_DONE,           // migrated doc on disk and live
};

// Migrate the live doc through a fresh parse of the file, since a deep copy of
// it overflows the boot stack. live becomes the migrated doc once it is on
// disk and is never read back, so live always holds a usable doc.
// Fs: parse(JsonDocument&), backup(), write(JsonVariantConst), each -> bool.
// in: fs, parsed live doc, step callback. out: result.
template <typename Fs>
inline ConfigMigrateResult configMigrateLive(Fs& fs, JsonDocument& live,
                                             void (*step)(int from, const char* name, size_t changed)) {
  if (configSchemaVersion(live.as<JsonVariantConst>()) == CONFIG_SCHEMA_CURRENT) return CFG_MIGRATE_NONE;
  JsonDocument work;
  if (!fs.parse(work)) return CFG_MIGRATE_REREAD_FAILED;
  switch (configMigrate(work, step)) {
    case CFG_SCHEMA_CURRENT: return CFG_MIGRATE_NONE;
    case CFG_SCHEMA_NEWER:   return CFG_MIGRATE_NEWER;
    case CFG_SCHEMA_INVALID: return CFG_MIGRATE_INVALID;
    case CFG_SCHEMA_MIGRATED: break;
  }
  if (!fs.backup()) return CFG_MIGRATE_BACKUP_FAILED;
  if (!fs.write(work.as<JsonVariantConst>())) return CFG_MIGRATE_WRITE_FAILED;
  live = std::move(work);
  return CFG_MIGRATE_DONE;
}
