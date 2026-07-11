// thesada-fw - ttl_policy.h
// Rollover-safe millis() TTL compares. No Arduino deps, host-unit-testable.
// millis() wraps at ~49.7 days; a plain `now < expiry` flips its answer at
// the wrap, re-opening auth windows and breaking token expiry (F4). Signed
// subtraction is wrap-correct as long as the interval is < 2^31 ms (~24.8 d),
// far above any TTL used here (30 s .. 1 h).
// Slot convention: expiry == 0 means "empty / consumed", so a live entry
// whose computed expiry lands exactly on 0 is treated as empty - a harmless
// 1-in-2^32 early expiry, accepted for the cheap sentinel.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// True while a non-empty slot's expiry is still in the future.
inline bool ttlActive(uint32_t now, uint32_t expiry) {
  return expiry != 0 && (int32_t)(expiry - now) > 0;
}

// True once a point in time has been reached or passed (lockout release).
inline bool ttlReached(uint32_t now, uint32_t at) {
  return (int32_t)(now - at) >= 0;
}

// Signed milliseconds until expiry; <= 0 means expired. For oldest-slot
// eviction, compare remaining times - never the raw expiry values.
inline int32_t ttlRemaining(uint32_t now, uint32_t expiry) {
  return (int32_t)(expiry - now);
}
