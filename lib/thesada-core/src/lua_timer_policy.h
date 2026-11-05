// thesada-fw - lua_timer_policy.h
// Node.setTimeout deadlines on 64-bit uptime ms, which never wraps.
// Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>

// Longest delay a script may ask for: 30 days.
static const uint64_t LUA_TIMER_MAX_DELAY_MS = 30ULL * 24 * 3600 * 1000;

// in: uptime ms, delay as Lua passed it. out: false for NaN (refuse), else
// *fireAt = now + delay, negative delay as 0, above the cap as the cap.
inline bool luaTimerFireAt(uint64_t nowMs, double delayMs, uint64_t* fireAt) {
  if (delayMs != delayMs) return false;
  if (delayMs < 0) delayMs = 0;
  if (delayMs > (double)LUA_TIMER_MAX_DELAY_MS) delayMs = (double)LUA_TIMER_MAX_DELAY_MS;
  *fireAt = nowMs + (uint64_t)delayMs;
  return true;
}

// in: uptime ms, fire time from luaTimerFireAt. out: true once due.
inline bool luaTimerDue(uint64_t nowMs, uint64_t fireAtMs) {
  return nowMs >= fireAtMs;
}
