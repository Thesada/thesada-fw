// thesada-fw - ap_policy.h
// Pure decision logic for the fallback access point. Host-unit-testable.
// See docs/invariants.md for why the AP is a lifetime surface, not a
// provisioning-only one.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stddef.h>
#include <string.h>

// WPA2 minimum. Below this the radio cannot use a passphrase at all.
#define AP_PASS_MIN_LEN 8

// Shipped placeholder. Exactly 8 characters, so a naive length check passes it
// and the AP comes up protected by a password that is public knowledge.
#define AP_PASS_PLACEHOLDER "changeme"

// Is this AP passphrase safe to bring an access point up with?
//
// Rejects absent, too short, and the shipped placeholder. The placeholder case
// is the one that matters: it is exactly AP_PASS_MIN_LEN characters, so a
// length-only gate admits it. Mirrors webAuthPassIsDefault in
// web_auth_policy.h, which draws the same distinction for the admin password.
// in: passphrase. out: true when usable.
inline bool apPasswordUsable(const char* pass) {
  if (!pass || !*pass) return false;
  if (strcmp(pass, AP_PASS_PLACEHOLDER) == 0) return false;
  return strlen(pass) >= AP_PASS_MIN_LEN;
}

// Should the fallback AP be started at all?
//
// Refusing is deliberate, and it is the fail-closed direction. An open or
// publicly-keyed AP is not a degraded recovery path, it is an unauthenticated
// console: the portal writes WiFi credentials and the device raises this AP on
// any WiFi failure for its whole service life, not just at first boot. A unit
// that never had its passphrase seeded must announce that loudly rather than
// come up looking provisioned.
// in: passphrase. out: true when the AP may start.
inline bool apMayStart(const char* pass) {
  return apPasswordUsable(pass);
}

// Build the AP SSID. Prefers the per-device identity over the operator label.
//
// The label is not unique: it ships as a fixed string in the config image, so
// every unit off the line would broadcast the same SSID and a per-device join
// QR would be ambiguous the moment two units are in range - the phone picks by
// signal strength and the passphrase does not match.
// in: out buffer, capacity, device id (may be empty), fallback name.
// out: true when the whole SSID fitted.
inline bool apSsidFor(char* out, size_t cap, const char* deviceId,
                      const char* fallbackName) {
  if (!out || cap == 0) return false;
  const char* base = (deviceId && *deviceId) ? deviceId : fallbackName;
  if (!base || !*base) { out[0] = '\0'; return false; }
  const char* suffix = "-setup";
  size_t need = strlen(base) + strlen(suffix);
  if (need >= cap) { out[0] = '\0'; return false; }
  memcpy(out, base, strlen(base));
  memcpy(out + strlen(base), suffix, strlen(suffix) + 1);
  return true;
}
