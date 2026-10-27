// thesada-fw - ota_verify_policy.h
// Pure decisions on the OTA verification path: the TLS insecure-fallback guard,
// manifest field validation, the version-update predicate, the fled-release
// skip and its expiry, and the SHA256 digest compare. No HTTPClient, no
// Update, no mbedtls, so host unit-testable.
//
// SCOPE: only these predicates moved. fetchManifest's HTTP and JSON handling,
// and the streaming mbedtls_sha256 accumulation in applyUpdate, stay in
// OTAUpdate.cpp and are NOT covered by this unit.
//
// NOT byte-for-byte with the inline code it replaced. Deliberate differences,
// all strictly more rejecting: null guards throughout, otaShaMatches gates
// digest LENGTH before comparing, and otaIsNewer requires a strict N.N.N on
// both sides (the old sscanf path treated unparseable fields as 0).
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>  // strcasecmp is POSIX, not <string.h>

// Manifest hard cap. Real manifests are ~200 B; larger is a misconfig or a
// hostile upstream.
#define OTA_MANIFEST_CAP (8u * 1024u)

// Length of a lowercase-hex SHA256 digest, without the terminator.
#define OTA_SHA256_HEX_LEN 64

// How OTA may come up on this TLS footing. With no CA cert both manifest and
// binary ride an unverified channel, and the manifest SHA256 is not MITM
// protection because an attacker who can MITM controls both - so refuse unless
// the operator opted in explicitly. One enum rather than two predicates, so a
// caller cannot evaluate half the decision.
enum OtaTlsMode {
  OTA_TLS_REFUSED,   // no CA, no opt-in: OTA stays down
  OTA_TLS_INSECURE,  // no CA, opt-in set: runs unverified, caller warns
  OTA_TLS_VERIFIED,  // CA present
};

inline OtaTlsMode otaTlsMode(bool haveCaCert, bool allowInsecure) {
  if (haveCaCert) return OTA_TLS_VERIFIED;
  return allowInsecure ? OTA_TLS_INSECURE : OTA_TLS_REFUSED;
}

// A manifest is usable iff version, url and sha256 are all present and
// non-empty. `size` is optional: the cellular chunked Range path needs it, the
// WiFi path falls back to Content-Length.
inline bool otaManifestValid(const char* version, const char* url,
                             const char* sha256) {
  if (!version || !url || !sha256) return false;
  return *version && *url && *sha256;
}

// Refuse a manifest body that would exceed the cap. Fed the running total so a
// streaming reader can abort mid-body rather than buffer first.
inline bool otaManifestSizeOk(size_t haveLen, size_t incomingLen) {
  return haveLen + incomingLen <= OTA_MANIFEST_CAP;
}

// Leading zeros are fine (CalVer uses 08). Advances *pp past the digits.
// Rejects overflow beyond INT_MAX.
// in: *pp at field start, out dest. out: true and *pp advanced; else false.
inline bool otaParseVersionField(const char** pp, int* out) {
  if (!pp || !*pp || !out) return false;
  const char* p = *pp;
  if (*p < '0' || *p > '9') return false;
  long long v = 0;
  while (*p >= '0' && *p <= '9') {
    v = v * 10 + (*p - '0');
    if (v > INT_MAX) return false;
    p++;
  }
  *out = (int)v;
  *pp = p;
  return true;
}

// Exactly three unsigned fields. Leading zeros OK; no sign, no trailing junk.
// in: version string + three outs. out: true only for a full N.N.N.
inline bool otaParseVersion(const char* s, int* major, int* minor, int* patch) {
  if (!s || !major || !minor || !patch) return false;
  const char* p = s;
  if (!otaParseVersionField(&p, major)) return false;
  if (*p++ != '.') return false;
  if (!otaParseVersionField(&p, minor)) return false;
  if (*p++ != '.') return false;
  if (!otaParseVersionField(&p, patch)) return false;
  return *p == '\0';
}

// Semver-ish compare over major.minor.patch. Either side must parse as a
// strict N.N.N or the result is false (not newer) - a malformed local no
// longer makes every remote look newer. `force` still bypasses via
// otaShouldUpdate.
inline bool otaIsNewer(const char* remote, const char* local) {
  if (!remote || !local) return false;
  int rMajor = 0, rMinor = 0, rPatch = 0;
  int lMajor = 0, lMinor = 0, lPatch = 0;
  if (!otaParseVersion(remote, &rMajor, &rMinor, &rPatch)) return false;
  if (!otaParseVersion(local, &lMajor, &lMinor, &lPatch)) return false;
  if (rMajor != lMajor) return rMajor > lMajor;
  if (rMinor != lMinor) return rMinor > lMinor;
  return rPatch > lPatch;
}

// Should the download proceed? force re-flashes even when remote is not newer.
inline bool otaShouldUpdate(const char* remote, const char* local, bool force) {
  return force || otaIsNewer(remote, local);
}

// Equal as N.N.N, the way otaIsNewer reads them. in: two versions.
// out: false when either does not parse.
inline bool otaSameVersion(const char* a, const char* b) {
  int aMajor = 0, aMinor = 0, aPatch = 0;
  int bMajor = 0, bMinor = 0, bPatch = 0;
  if (!otaParseVersion(a, &aMajor, &aMinor, &aPatch)) return false;
  if (!otaParseVersion(b, &bMajor, &bMinor, &bPatch)) return false;
  return aMajor == bMajor && aMinor == bMinor && aPatch == bPatch;
}

// ---- Fled-release skip ------------------------------------------------------

// A release a guard switch fled is refused for this long, then installed again.
#define OTA_FLED_SKIP_S (24u * 60u * 60u)
// Installs per expired skip. A crash-looping image hands its streak to the
// image it installs, and the guard rolls that one back at its first boot.
#define OTA_FLED_TRIES 2u
// After a boot's first save the skip age is saved at most this often.
#define OTA_FLED_SAVE_S (10u * 60u)
// Most a synced clock adds across one reset for time the unit was off, so a
// clock that jumped forward lifts the skip at most this much early.
#define OTA_FLED_GAP_MAX_S (60u * 60u)

// Saved skip state. seenEpoch is the synced clock at the save, 0 if unsynced.
struct OtaFledAge {
  uint32_t ageS;
  uint8_t tries;
  uint32_t seenEpoch;
};

// One NVS u64: the age in the low 24 bits, the installs since it expired in
// the next 8, the clock on top. in: state. out: stored word.
inline uint64_t otaFledPack(const OtaFledAge& a) {
  uint32_t age = a.ageS < OTA_FLED_SKIP_S ? a.ageS : OTA_FLED_SKIP_S;
  return ((uint64_t)a.seenEpoch << 32) | ((uint64_t)a.tries << 24) | age;
}

// in: stored word, 0 for a fresh record. out: state.
inline OtaFledAge otaFledUnpack(uint64_t v) {
  OtaFledAge a;
  a.ageS = (uint32_t)(v & 0xFFFFFFu);
  a.tries = (uint8_t)(v >> 24);
  a.seenEpoch = (uint32_t)(v >> 32);
  return a;
}

// RTC memory pair: unsaved seconds, then their complement. A power-on leaves
// noise, which reads as 0. in: pair. out: seconds, capped at two saves.
inline uint32_t otaFledCarry(uint32_t value, uint32_t check) {
  if (check != ~value) return 0;
  return value < 2u * OTA_FLED_SAVE_S ? value : 2u * OTA_FLED_SAVE_S;
}

// Seconds since the last save; a boot's first save also adds clock time off.
// in: uptime since it (plus RTC carry if first), first, epochs (0 unsynced). out: s.
inline uint32_t otaFledElapsed(uint32_t upS, bool firstThisBoot, uint32_t seenEpoch,
                               uint32_t nowEpoch) {
  if (!firstThisBoot || seenEpoch == 0 || nowEpoch <= seenEpoch) return upS;
  uint32_t gap = nowEpoch - seenEpoch;
  if (gap <= upS) return upS;
  uint32_t off = gap - upS;
  return upS + (off < OTA_FLED_GAP_MAX_S ? off : OTA_FLED_GAP_MAX_S);
}

// A boot's first save carries the clock gap; later ones wait OTA_FLED_SAVE_S, so
// a triggered check cannot write NVS each time. in: unsaved s, first. out: save.
inline bool otaFledSaveDue(uint32_t unsavedS, bool firstThisBoot) {
  return firstThisBoot || unsavedS >= OTA_FLED_SAVE_S;
}

// in: saved age, elapsed seconds. out: the age, held at OTA_FLED_SKIP_S.
inline uint32_t otaFledAdvance(uint32_t ageS, uint32_t elapsedS) {
  if (ageS >= OTA_FLED_SKIP_S || elapsedS >= OTA_FLED_SKIP_S - ageS) return OTA_FLED_SKIP_S;
  return ageS + elapsedS;
}

// The skip ages only while it can refuse something: an unexpired record of
// another build. in: record, running version, saved age. out: true to age.
inline bool otaFledAges(const char* fled, const char* running, uint32_t ageS) {
  int major = 0, minor = 0, patch = 0;
  if (!otaParseVersion(fled, &major, &minor, &patch)) return false;
  return ageS < OTA_FLED_SKIP_S && !otaSameVersion(fled, running);
}

enum OtaFledAction : uint8_t {
  OTA_FLED_NONE = 0,    // not the fled release
  OTA_FLED_SKIP = 1,    // refuse it
  OTA_FLED_RETRY = 2,   // install it; the next check may install it again
  OTA_FLED_LAST = 3,    // install it; the skip starts over
  OTA_FLED_FORCED = 4,  // install it past the skip
};

// What check does with a manifest version. An unparseable record refuses
// nothing. in: remote, record, age, installs since expiry, force. out: action.
inline OtaFledAction otaFledAction(const char* remote, const char* fled, uint32_t ageS,
                                   uint8_t tries, bool force) {
  if (!otaSameVersion(remote, fled)) return OTA_FLED_NONE;
  if (force) return OTA_FLED_FORCED;
  if (ageS < OTA_FLED_SKIP_S) return OTA_FLED_SKIP;
  return tries + 1u < OTA_FLED_TRIES ? OTA_FLED_RETRY : OTA_FLED_LAST;
}

// A try is spent before its download. One whose save failed reads as unspent
// after the install reboots, so it does not download. in: action, saved. out: download.
inline bool otaFledMayDownload(OtaFledAction action, bool saved) {
  return saved || (action != OTA_FLED_RETRY && action != OTA_FLED_LAST);
}

// The age and tries a fled try leaves: spent once saved, unspent when NVS
// refused the save. in: action, age now, tries, saved. out: record, seenEpoch 0.
inline OtaFledAge otaFledAfterTry(OtaFledAction action, uint32_t ageS, uint8_t tries, bool saved) {
  if (saved && action == OTA_FLED_LAST) return {0, 0, 0};
  if (saved && action == OTA_FLED_RETRY) return {ageS, (uint8_t)(tries + 1u), 0};
  return {ageS, tries, 0};
}

// Digest compare. Case-insensitive because the manifest may carry either case,
// but length-checked first so a truncated or padded digest is rejected outright
// rather than leaning on the compare.
inline bool otaShaMatches(const char* computedHex, const char* expectedHex) {
  if (!computedHex || !expectedHex) return false;
  if (strlen(computedHex) != OTA_SHA256_HEX_LEN) return false;
  if (strlen(expectedHex) != OTA_SHA256_HEX_LEN) return false;
  return strcasecmp(computedHex, expectedHex) == 0;
}
