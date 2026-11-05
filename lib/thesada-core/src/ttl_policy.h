// thesada-fw - ttl_policy.h
// Auth-state TTLs on 64-bit uptime ms. No Arduino deps, host-unit-testable.
// Nothing clears a slot when it expires, so a 32-bit millis() expiry read
// live again 2^31 ms later; 64-bit uptime does not wrap.
// Slot convention: expiry == 0 means "empty / consumed". Every expiry is
// uptime plus a nonzero TTL, so a live entry never lands on 0.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// True while a non-empty slot's expiry is still in the future.
// in: uptime ms, expiry in uptime ms (0 = empty). out: true while live.
inline bool ttlActive(uint64_t now, uint64_t expiry) {
  return expiry != 0 && now < expiry;
}
