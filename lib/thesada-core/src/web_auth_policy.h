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
