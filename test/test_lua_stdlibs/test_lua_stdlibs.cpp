// Host tests for lua_stdlibs.h, built against the Lua fork the boards link.
// SPDX-License-Identifier: GPL-3.0-only
#include <unity.h>
#include <string.h>
#include "lua_stdlibs.h"

static lua_State* L = nullptr;
static char out[128];

void setUp(void) {
  L = luaL_newstate();
  luaOpenLibs(L);
}

void tearDown(void) {
  lua_close(L);
  L = nullptr;
}

// in: Lua chunk returning one value. out: tostring of it, or "error: <msg>".
static const char* run(const char* code) {
  if (luaL_dostring(L, code) != LUA_OK) {
    snprintf(out, sizeof(out), "error: %s", lua_tostring(L, -1));
  } else {
    const char* s = luaL_tolstring(L, -1, nullptr);
    snprintf(out, sizeof(out), "%s", s ? s : "(nil)");
    lua_pop(L, 1);
  }
  lua_settop(L, 0);
  return out;
}

// The premise: the fork's own luaL_openlibs leaves the names, not the tables.
void test_fork_openlibs_alone_leaves_name_strings(void) {
  lua_State* raw = luaL_newstate();
  luaL_openlibs(raw);
  lua_getglobal(raw, "math");
  TEST_ASSERT_EQUAL_INT(LUA_TSTRING, lua_type(raw, -1));
  lua_close(raw);
}

void test_globals_are_library_tables(void) {
  TEST_ASSERT_EQUAL_STRING("table table table",
      run("return type(math) .. ' ' .. type(table) .. ' ' .. type(string)"));
}

void test_function_syntax_calls_work(void) {
  TEST_ASSERT_EQUAL_STRING("a,b", run("return table.concat({'a', 'b'}, ',')"));
  TEST_ASSERT_EQUAL_STRING("2", run("return math.floor(2.7)"));
  TEST_ASSERT_EQUAL_STRING("x=7", run("return string.format('x=%d', 7)"));
  TEST_ASSERT_EQUAL_STRING("3", run("local t = {} table.insert(t, 3) return t[1]"));
}

void test_method_syntax_still_works(void) {
  TEST_ASSERT_EQUAL_STRING("ABC 05", run("return ('abc'):upper() .. ' ' .. ('%02d'):format(5)"));
}

// The require cache and the global hold the same table.
void test_loaded_table_matches_global(void) {
  luaL_getsubtable(L, LUA_REGISTRYINDEX, LUA_LOADED_TABLE);
  const char* const names[] = {"math", "table", "string"};
  for (const char* name : names) {
    lua_getfield(L, -1, name);
    lua_getglobal(L, name);
    TEST_ASSERT_TRUE_MESSAGE(lua_istable(L, -1), name);
    TEST_ASSERT_TRUE_MESSAGE(lua_rawequal(L, -1, -2), name);
    lua_pop(L, 2);
  }
  lua_pop(L, 1);
}

// A stock opener returns the table and sets no global.
static int stockOpen(lua_State* S) {
  lua_newtable(S);
  lua_pushinteger(S, 42);
  lua_setfield(S, -2, "answer");
  return 1;
}

void test_stock_opener_table_is_stored(void) {
  luaOpenStdLib(L, "stock", stockOpen);
  TEST_ASSERT_EQUAL_INT(0, lua_gettop(L));
  TEST_ASSERT_EQUAL_STRING("42", run("return stock.answer"));
}

int main(int, char**) {
  UNITY_BEGIN();
  RUN_TEST(test_fork_openlibs_alone_leaves_name_strings);
  RUN_TEST(test_globals_are_library_tables);
  RUN_TEST(test_function_syntax_calls_work);
  RUN_TEST(test_method_syntax_still_works);
  RUN_TEST(test_loaded_table_matches_global);
  RUN_TEST(test_stock_opener_table_is_stored);
  return UNITY_END();
}
