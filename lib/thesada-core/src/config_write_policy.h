// thesada-fw - config_write_policy.h
// Which paths are the config file, where an upload stages, and when a write
// landed whole. Pure, host-tested. No file write replaces its target in place.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// in: path after pathSafePolicy. out: true when littlefs 2.10+ resolves it to
// /config.json. lfs_dir_find skips "." segments and refuses any path after a file.
inline bool configPathIsLive(const char* path) {
  if (!path) return false;
  const char* p = path;
  for (;;) {
    while (*p == '/') p++;
    if (p[0] == '.' && (p[1] == '/' || p[1] == '\0')) { p++; continue; }
    break;
  }
  return strcmp(p, "config.json") == 0;
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
