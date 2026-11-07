// thesada-fw - lua_stdlibs.h
// Opens the Lua standard libraries so string, table and math hold their
// library tables. Host-tested against the Lua fork in test/test_lua_stdlibs.
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

extern "C" {
#include <lua/lua.h>
#include <lua/lualib.h>
#include <lua/lauxlib.h>
}

// Fork openers set their global and return the name arg luaL_requiref would
// store. Store the table. in: state, name, opener. out: none, stack unchanged.
inline void luaOpenStdLib(lua_State* L, const char* name, lua_CFunction openf) {
  lua_pushcfunction(L, openf);
  lua_pushstring(L, name);
  lua_call(L, 1, 1);  // a C opener, no script runs
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_getglobal(L, name);
  }
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  lua_pushvalue(L, -2);
  lua_setfield(L, -2, name);
  lua_pop(L, 1);
  lua_setglobal(L, name);
}

// luaL_openlibs, then string, table and math again as tables.
// in: fresh state. out: none.
inline void luaOpenLibs(lua_State* L) {
  luaL_openlibs(L);
  luaOpenStdLib(L, LUA_MATHLIBNAME, luaopen_math);
  luaOpenStdLib(L, LUA_TABLIBNAME, luaopen_table);
  luaOpenStdLib(L, LUA_STRLIBNAME, luaopen_string);
}
