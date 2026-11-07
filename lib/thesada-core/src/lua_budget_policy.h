// thesada-fw - lua_budget_policy.h
// One LUA_SCRIPT_BUDGET_MS wall-clock budget per call from C into Lua, shared
// by the calls nested in it. Pure, no Arduino deps, host-unit-testable.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <stdint.h>
#include "boot_wait_policy.h"

// Most native time one outermost call gets back, so a loop of slow sends ends.
static const uint32_t LUA_NATIVE_CREDIT_MS = 60000;

struct LuaBudget {
  uint32_t startMs;
  uint32_t creditMs;  // native time given back this outermost call
  uint16_t depth;     // nesting is bounded by Lua's C-call limit (200)
  bool     hit;
};

// Start a call. Only the outermost call starts the clock.
// in: budget, millis now. out: none.
inline void luaBudgetEnter(LuaBudget& b, uint32_t nowMs) {
  if (b.depth == 0) {
    b.startMs = nowMs;
    b.creditMs = 0;
    b.hit = false;
  }
  b.depth++;
}

// Give back time a native binding spent blocked, up to LUA_NATIVE_CREDIT_MS
// per outermost call. A spent budget stays spent. in: budget, ms. out: none.
inline void luaBudgetCredit(LuaBudget& b, uint32_t ms) {
  if (b.depth == 0) return;
  const uint32_t room = LUA_NATIVE_CREDIT_MS - b.creditMs;
  if (ms > room) ms = room;
  b.startMs += ms;
  b.creditMs += ms;
}

// in: budget. out: true while any call into Lua runs.
inline bool luaBudgetActive(const LuaBudget& b) {
  return b.depth > 0;
}

// Count-hook tick. Rollover-safe. Once spent, stays spent until the call ends.
// in: budget, millis now. out: true when the running call must raise.
inline bool luaBudgetTick(LuaBudget& b, uint32_t nowMs) {
  if (b.depth == 0) return false;
  if (!b.hit && waitBudgetSpent(b.startMs, nowMs, LUA_SCRIPT_BUDGET_MS)) b.hit = true;
  return b.hit;
}

// in: budget. out: true while a call runs with its budget spent.
inline bool luaBudgetSpent(const LuaBudget& b) {
  return b.depth > 0 && b.hit;
}

// End a call. in: budget. out: true if the budget ran out during it, even
// when the script caught the error and returned normally.
inline bool luaBudgetLeave(LuaBudget& b) {
  if (b.depth == 0) return false;
  b.depth--;
  return b.hit;
}
