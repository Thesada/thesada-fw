// thesada-fw - ota_verify_policy.h
// Pure decisions on the OTA verification path: the TLS insecure-fallback guard,
// manifest field validation, the version-update predicate, and the SHA256
// digest compare. No HTTPClient, no Update, no mbedtls, so host unit-testable.
//
// SCOPE: only these predicates moved. fetchManifest's HTTP and JSON handling,
// and the streaming mbedtls_sha256 accumulation in applyUpdate, stay in
// OTAUpdate.cpp and are NOT covered by this unit.
//
// NOT byte-for-byte with the inline code it replaced. Deliberate differences,
// all strictly more rejecting: null guards throughout, and otaShaMatches gates
// digest LENGTH before comparing where the original relied on strcasecmp alone.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <stdio.h>
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

// Semver-ish compare over major.minor.patch.
//
// NOTE: mirrors the original exactly, including its weakness - the sscanf
// return is not checked, so an unparseable version reads as 0.0.0 rather than
// being rejected. Safe for the !force path (0.0.0 is never newer, so a
// malformed manifest is refused); `force` bypasses this predicate entirely.
// Pinned by tests rather than silently changed.
inline bool otaIsNewer(const char* remote, const char* local) {
  if (!remote || !local) return false;
  int rMajor = 0, rMinor = 0, rPatch = 0;
  int lMajor = 0, lMinor = 0, lPatch = 0;
  sscanf(remote, "%d.%d.%d", &rMajor, &rMinor, &rPatch);
  sscanf(local,  "%d.%d.%d", &lMajor, &lMinor, &lPatch);
  if (rMajor != lMajor) return rMajor > lMajor;
  if (rMinor != lMinor) return rMinor > lMinor;
  return rPatch > lPatch;
}

// Should the download proceed? force re-flashes even when remote is not newer.
inline bool otaShouldUpdate(const char* remote, const char* local, bool force) {
  return force || otaIsNewer(remote, local);
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
