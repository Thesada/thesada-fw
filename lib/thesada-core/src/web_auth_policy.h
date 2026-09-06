// thesada-fw - web_auth_policy.h
// Pure decision logic for HTTP admin auth. No Arduino deps, so it is
// host-unit-testable. Default (or empty) credentials must never serve the
// authenticated surface (F2) - the veto beats Bearer tokens too, so a token
// minted before a password reset cannot outlive the reset.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string.h>

// The shipped default password. A missing or empty web.password resolves to
// this via the config fallback, and an explicit "" must not authenticate.
inline bool webAuthPassIsDefault(const char* pass) {
  return !pass || !*pass || strcmp(pass, "changeme") == 0;
}

// Admit iff a real password is set AND one of the two schemes verified.
// Default password vetoes everything, including a valid Bearer token.
inline bool webAuthAllowed(bool passIsDefault, bool bearerValid, bool basicOk) {
  if (passIsDefault) return false;
  return bearerValid || basicOk;
}

// Safe methods per RFC 9110. Anything else - unknown or missing included -
// counts as state-changing, so a new verb is refused rather than waved through.
inline bool webAuthMethodChangesState(const char* method) {
  if (!method) return true;
  return !(strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0 ||
           strcmp(method, "OPTIONS") == 0);
}

// A cross-site request replays cached Basic credentials; Bearer cannot ride
// along that way. Refuse Basic for that one shape - curl sends no header.
// stateChanging is the caller's verdict: the method, OR a route whose GET has
// a side effect (/api/ws/token mints a WS grant, so it counts).
// What the login rate limiter counts. A refusal by policy, or a request that
// carried no credential at all, is not a guess - counting either lets a
// foreign page lock the operator out of a device it cannot even read.
inline bool webAuthCountsAsGuess(bool allowed, bool basicAllowed,
                                 bool credentialOffered) {
  return !allowed && basicAllowed && credentialOffered;
}

inline bool webAuthBasicAllowed(bool stateChanging, const char* secFetchSite) {
  if (!secFetchSite || !*secFetchSite) return true;
  if (!stateChanging) return true;
  return strcmp(secFetchSite, "cross-site") != 0;
}
