// thesada-fw - lua_budget_hooks.h
// Wires a Lua state to one LuaBudget: the count hook, the budgeted pcall and
// the pcall/xpcall wrapper. Host-tested in test/test_lua_budget_hooks.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <stdint.h>
#include <lua_budget_policy.h>
#include "lua_stdlibs.h"

// The budget plus the clock and watchdog feed it runs on.
struct LuaBudgetHost {
  LuaBudget budget;
  uint32_t (*now)();
  void (*feed)();
};

// in: any thread of a state luaBudgetInstall set up. out: its host.
inline LuaBudgetHost* luaBudgetHostOf(lua_State* L) {
  return *static_cast<LuaBudgetHost**>(lua_getextraspace(L));
}

// Count hook: feed while the running call is inside its budget, raise once
// it is spent. Idle between calls.
inline void luaBudgetHook(lua_State* L, lua_Debug*) {
  LuaBudgetHost* h = luaBudgetHostOf(L);
  if (!luaBudgetActive(h->budget)) return;
  if (luaBudgetTick(h->budget, h->now())) {
    luaL_error(L, "script.budget_exceeded budget_ms=%d", (int)LUA_SCRIPT_BUDGET_MS);
    return;
  }
  h->feed();
}

// Call before any coroutine exists: new threads copy the hook and the extra
// space. in: fresh state, host that outlives it. out: none.
inline void luaBudgetInstall(lua_State* L, LuaBudgetHost* host) {
  *static_cast<LuaBudgetHost**>(lua_getextraspace(L)) = host;
  lua_sethook(L, luaBudgetHook, LUA_MASKCOUNT, LUA_HOOK_COUNT);
}

// The one way C calls Lua: lua_pcall under the budget, nested calls sharing it.
// in: function + nargs on the stack. out: pcall code, LUA_ERRRUN on a caught overrun.
inline int luaBudgetedCall(lua_State* L, int nargs, int nresults) {
  LuaBudgetHost* h = luaBudgetHostOf(L);
  const int base = lua_gettop(L) - nargs - 1;
  luaBudgetEnter(h->budget, h->now());
  h->feed();
  int rc = lua_pcall(L, nargs, nresults, 0);
  if (luaBudgetLeave(h->budget) && rc == LUA_OK) {
    lua_settop(L, base);
    lua_pushfstring(L, "script.budget_exceeded caught budget_ms=%d", (int)LUA_SCRIPT_BUDGET_MS);
    rc = LUA_ERRRUN;
  }
  return rc;
}

// pcall/xpcall wrapper: once the budget is spent, re-raise after the protected
// call, so a caught budget error climbs every pcall up to luaBudgetedCall.
inline int luaBudgetSafeCall(lua_State* L) {
  const int n = lua_gettop(L);
  lua_pushvalue(L, lua_upvalueindex(1));
  lua_insert(L, 1);
  lua_call(L, n, LUA_MULTRET);
  if (luaBudgetSpent(luaBudgetHostOf(L)->budget)) {
    return luaL_error(L, "script.budget_exceeded budget_ms=%d", (int)LUA_SCRIPT_BUDGET_MS);
  }
  return lua_gettop(L);
}

// in: state with the base library open. out: none.
inline void luaBudgetWrapProtectedCalls(lua_State* L) {
  static const char* const names[] = {"pcall", "xpcall"};
  for (const char* name : names) {
    lua_getglobal(L, name);
    if (lua_isfunction(L, -1)) {
      lua_pushcclosure(L, luaBudgetSafeCall, 1);
      lua_setglobal(L, name);
    } else {
      lua_pop(L, 1);
    }
  }
}
