// thesada-fw - config_write_policy.h
// Which paths are the config file, where an upload stages, and when a write
// landed whole. Pure, host-tested. No file write replaces its target in place.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "config_mask_policy.h"

// lfs_dir_find skips "." segments. in: path. out: path past its leading
// slashes and "." segments, "" for the root.
inline const char* fsSkipRootDots(const char* p) {
  for (;;) {
    while (*p == '/') p++;
    if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) { p++; continue; }
    return p;
  }
}

// in: path after pathSafePolicy. out: true when littlefs 2.10+ resolves it to
// /config.json. lfs_dir_find refuses any path after a file.
inline bool configPathIsLive(const char* path) {
  if (!path) return false;
  return strcmp(fsSkipRootDots(path), "config.json") == 0;
}

enum FsWriteRoute {
  FS_ROUTE_PLAIN = 0,      // writeChecked on the file itself
  FS_ROUTE_CONFIG,         // whole JSON object, tmp + rename (Config::writeRaw)
  FS_ROUTE_CONFIG_APPEND,  // refused: an append to the config file is in place
};

// How fs.write / fs.append reach a path. Only LittleFS holds the config file.
// in: target is LittleFS, path after stripPrefix, append. out: route.
inline FsWriteRoute fsWriteRoute(bool littleFs, const char* path, bool append) {
  if (!littleFs || !configPathIsLive(path)) return FS_ROUTE_PLAIN;
  return append ? FS_ROUTE_CONFIG_APPEND : FS_ROUTE_CONFIG;
}

// in: bytes handed to the File, tmp size read back after close, intended size.
// out: true when all agree. fclose does the real write and its result is dropped.
inline bool fileWriteLanded(size_t written, size_t onDisk, size_t expected) {
  return written == expected && onDisk == expected;
}

// As fileWriteLanded, and an empty config file never parses, so it never lands.
inline bool configWriteLanded(size_t written, size_t onDisk, size_t expected) {
  return expected > 0 && fileWriteLanded(written, onDisk, expected);
}

// The config editor stages a save here, then moves it over /config.json.
static const char CONFIG_STAGED_NAME[] = "config.json.new";

enum FsMoveRoute {
  FS_MOVE_PLAIN = 0,       // rename as asked
  FS_MOVE_CONFIG_STAGED,   // the staged save: written through Config::writeRaw, then removed
  FS_MOVE_CONFIG_REFUSED,  // any other move onto the config file
};

// How fs.mv reaches dst. Only the staged save may become /config.json, and it
// goes through the write path that keeps stored secrets, never a rename.
// in: both on LittleFS, src and dst after stripPrefix. out: route.
inline FsMoveRoute fsMoveRoute(bool littleFs, const char* src, const char* dst) {
  if (!littleFs || !configPathIsLive(dst)) return FS_MOVE_PLAIN;
  return src && strcmp(fsSkipRootDots(src), CONFIG_STAGED_NAME) == 0 ? FS_MOVE_CONFIG_STAGED
                                                                    : FS_MOVE_CONFIG_REFUSED;
}

static const char UPLOAD_TMP_NAME[] = ".upload.tmp";
static const char UPLOAD_BAK_NAME[] = ".upload.bak";

// FAT matches names case-blind and drops trailing dots and spaces, so those
// spellings are the staging file too. in: base name. out: aliases one.
inline bool uploadNameReserved(const char* name) {
  size_t n = strlen(name);
  while (n > 0 && (name[n - 1] == '.' || name[n - 1] == ' ')) n--;
  const char* reserved[] = {UPLOAD_TMP_NAME, UPLOAD_BAK_NAME};
  for (const char* r : reserved) {
    if (strlen(r) != n) continue;
    size_t i = 0;
    while (i < n) {
      char c = name[i];
      if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
      if (c != r[i]) break;
      i++;
    }
    if (i == n) return true;
  }
  return false;
}

// Staging path: name in the target's own directory, so a rename never crosses
// directories. out: false if it does not fit or the target is a staging name.
inline bool uploadStagePath(char* dst, size_t cap, const char* path, const char* name) {
  if (!dst || cap == 0 || !path || !name) return false;
  const char* slash = strrchr(path, '/');
  if (!slash || slash[1] == '\0' || uploadNameReserved(slash + 1)) return false;
  size_t dirLen = (size_t)(slash - path) + 1;
  size_t nameLen = strlen(name);
  if (dirLen + nameLen + 1 > cap) return false;
  memcpy(dst, path, dirLen);
  memcpy(dst + dirLen, name, nameLen + 1);
  return true;
}

inline bool uploadTmpPath(char* dst, size_t cap, const char* path) {
  return uploadStagePath(dst, cap, path, UPLOAD_TMP_NAME);
}

enum FatCommit {
  FAT_COMMIT_OK = 0,
  FAT_COMMIT_OK_BAK_LEFT,  // new file in place, the parked old one could not be removed
  FAT_COMMIT_KEPT_OLD,     // nothing changed, the old file is in place, tmp removed
  FAT_COMMIT_STRANDED,     // old file at bak, new one at tmp, target missing
};

// Move a landed tmp over path on a filesystem whose rename cannot replace (FAT).
// Fs has exists(p), remove(p), rename(a, b). Some copy survives every failure.
template <typename Fs>
inline FatCommit fatCommit(Fs& fs, const char* tmp, const char* bak, const char* path) {
  bool parked = false;
  if (fs.exists(path)) {
    fs.remove(bak);
    if (!fs.rename(path, bak)) { fs.remove(tmp); return FAT_COMMIT_KEPT_OLD; }
    parked = true;
  }
  if (!fs.rename(tmp, path)) {
    if (!parked) { fs.remove(tmp); return FAT_COMMIT_KEPT_OLD; }
    if (fs.rename(bak, path)) { fs.remove(tmp); return FAT_COMMIT_KEPT_OLD; }
    return FAT_COMMIT_STRANDED;
  }
  if (parked && !fs.remove(bak)) return FAT_COMMIT_OK_BAK_LEFT;
  return FAT_COMMIT_OK;
}

// in: shell config key. out: true for the prefix and the section that holds it.
inline bool configKeyIsPrefix(const char* key) {
  return strcmp(key, "mqtt.topic_prefix") == 0 || strcmp(key, "mqtt") == 0;
}

// The reply config.set / config.del give once the edit is on disk and live.
// in: buf, key, value (null for a delete), file prefix not live yet. out: buf filled.
// A secret value reads as its marker, a literal with secrets inside, or a
// duplicate key, as its masked form cut to fit, a literal that does not parse not at all.
inline void configSavedReply(char* buf, size_t cap, const char* key, const char* value, bool pending = false) {
  const bool restart = pending && configKeyIsPrefix(key);
  char shown[160];
  // configPathStore skips the same spaces, so a spaced literal is masked as stored.
  const char* lv = value ? configSkipSpace(value) : nullptr;
  if (value && configPathIsSecret(key)) {
    value = *value ? CONFIG_MASK_SET : CONFIG_MASK_EMPTY;
  } else if (lv && (*lv == '{' || *lv == '[')) {
    JsonDocument lit;
    if (deserializeJson(lit, lv, DeserializationOption::NestingLimit(CONFIG_MAX_DEPTH))) {
      value = "(value not shown)";
    } else {
      const bool whole = configParseKeptMembers(lv, strlen(lv), lit.as<JsonVariantConst>());
      if (configMaskTree(lit.as<JsonVariant>()) > 0 || !whole) {
        const size_t n = serializeJson(lit, shown, sizeof(shown) - 1);
        shown[n] = '\0';
        value = shown;
      }
    }
  }
  if (value && restart) {
    (void)snprintf(buf, cap, "Set %s = %s (saved, new prefix applies after restart)", key, value);
  } else if (value) {
    (void)snprintf(buf, cap, "Set %s = %s (saved - run config.reload to apply network changes)", key, value);
  } else if (restart) {
    (void)snprintf(buf, cap, "Deleted %s (saved, new prefix applies after restart)", key);
  } else {
    (void)snprintf(buf, cap, "Deleted %s (saved)", key);
  }
}
