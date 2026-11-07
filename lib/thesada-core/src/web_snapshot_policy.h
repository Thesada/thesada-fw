// thesada-fw - web_snapshot_policy.h
// The config values HTTP handlers read, copied out of the config doc on the main
// loop, so no async_tcp code walks it. Pure, host-tested. No Arduino deps.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>
#include "web_auth_policy.h"

static const size_t WEB_SNAP_USER_CAP = 64;
static const size_t WEB_SNAP_PASS_CAP = 160;  // Secret::MAX_LEN, asserted at the user
static const size_t WEB_SNAP_NAME_CAP = 64;
static const char   WEB_SNAP_NAME_DEFAULT[] = "thesada-node";

struct WebSnap {
  bool userFits;                    // false: web.user is too long, nothing authenticates
  bool passFits;                    // false: the web.password fallback is too long, passCfg is ""
  char user[WEB_SNAP_USER_CAP];     // web.user
  char passCfg[WEB_SNAP_PASS_CAP];  // web.password, the fallback behind the NVS secret
  char devName[WEB_SNAP_NAME_CAP];  // device.friendly_name, else device.name
};

// Copy src into dst when it fits, else "". in: dst, cap, src (null reads as ""). out: fits.
inline bool webSnapCopy(char* dst, size_t cap, const char* src) {
  if (!src) src = "";
  size_t n = strlen(src);
  if (n >= cap) { dst[0] = '\0'; return false; }
  memcpy(dst, src, n + 1);
  return true;
}

// Credentials are never clipped, since a clipped one would accept a different
// credential. The name is display only and falls back to the default.
inline void webSnapFill(WebSnap& s, const char* user, const char* passCfg, const char* devName) {
  s.userFits = webSnapCopy(s.user, sizeof(s.user), user);
  s.passFits = webSnapCopy(s.passCfg, sizeof(s.passCfg), passCfg);
  if (!webSnapCopy(s.devName, sizeof(s.devName), devName)) {
    webSnapCopy(s.devName, sizeof(s.devName), WEB_SNAP_NAME_DEFAULT);
  }
}

enum WebCredState {
  WEB_CRED_OK = 0,
  WEB_CRED_DEFAULT,   // web.password is the shipped default or empty
  WEB_CRED_TOO_LONG,  // web.user, or the fallback actually in use, did not fit
};

// in: snapshot, the password Secret::resolve returned with passCfg as fallback.
// A too-long fallback is "", so a non-empty password means NVS won and it is unused.
inline WebCredState webCredState(const WebSnap& s, const char* effectivePass) {
  if (!s.userFits) return WEB_CRED_TOO_LONG;
  if (!s.passFits && (!effectivePass || !*effectivePass)) return WEB_CRED_TOO_LONG;
  if (webAuthPassIsDefault(effectivePass)) return WEB_CRED_DEFAULT;
  return WEB_CRED_OK;
}
