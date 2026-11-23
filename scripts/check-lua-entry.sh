#!/usr/bin/env bash
# Every C call into Lua runs under the script budget: no lua_pcall, dostring or
# sethook outside lua_budget_hooks.h, and ScriptEngine wires every state to it.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

# <file>:<call text>. luaBudgetSafeCall runs inside a budgeted call, from Lua.
# luaOpenStdLib calls a C library opener while the state is built.
HOOKS=lib/thesada-mod-scriptengine/src/lua_budget_hooks.h
SE=lib/thesada-mod-scriptengine/src/ScriptEngine.cpp
ALLOWED=(
  "$HOOKS:int rc = lua_pcall(L, nargs, nresults, 0);"
  "$HOOKS:lua_call(L, n, LUA_MULTRET);"
  "$HOOKS:lua_sethook(L, luaBudgetHook, LUA_MASKCOUNT, LUA_HOOK_COUNT);"
  "lib/thesada-mod-scriptengine/src/lua_stdlibs.h:lua_call(L, 1, 1);  // a C opener, no script runs"
)

if [ ! -f "$SE" ] || [ ! -f "$HOOKS" ]; then
  echo "check-lua-entry: $SE or $HOOKS not found" >&2
  exit 1
fi

hits="$(find lib src examples -type f \( -name '*.cpp' -o -name '*.h' -o -name '*.ino' \) \
  -exec grep -HnE '(^|[^A-Za-z0-9_])(lua_pcallk?|lua_callk?|luaL_dostring|luaL_dofile|luaL_dobuffer|lua_resume|lua_sethook)[[:space:]]*\(' {} + \
  2>/dev/null | sort || true)"

status=0

# need <function> <line>: ScriptEngine::<function>() holds <line> ahead of luaOpenLibs.
need() {
  if awk -v fn="void ScriptEngine::$1() {" -v want="$2" '
      $0 == fn { on = 1; next }
      on && /^}/ { exit }
      on {
        line = $0
        sub(/^[ \t]+/, "", line)
        if (line == want) { found = 1; exit }
        if (line ~ /^luaOpenLibs\(/) early = 1
      }
      END { exit !(found && !early) }' "$SE"; then
    printf 'ok    %s %s()  %s\n' "$SE" "$1" "$2"
  else
    printf 'FAIL  %s %s() lacks %s ahead of luaOpenLibs\n' "$SE" "$1" "$2" >&2
    status=1
  fi
}
need createState 'luaBudgetInstall(gL, &_budget);'
need callAllRegistrars 'luaBudgetWrapProtectedCalls(gL);'
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  file="${hit%%:*}"
  rest="${hit#*:}"
  line="${rest%%:*}"
  code="${rest#*:}"
  code="${code#"${code%%[![:space:]]*}"}"
  ok=0
  for a in "${ALLOWED[@]}"; do
    [ "$a" = "${file}:${code}" ] && { ok=1; break; }
  done
  if [ "$ok" -eq 1 ]; then
    printf 'ok    %s:%s  %s\n' "$file" "$line" "$code"
  else
    printf 'FAIL  %s:%s  %s\n' "$file" "$line" "$code" >&2
    status=1
  fi
done <<EOF
$hits
EOF

if [ "$status" -ne 0 ]; then
  echo "check-lua-entry: call Lua from C through luaBudgetedCall on a state luaBudgetInstall set up, or a loop can outlast the task watchdog." >&2
  exit 1
fi
echo "check-lua-entry: ok"
