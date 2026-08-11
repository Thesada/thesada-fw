#!/usr/bin/env bash

# allow-long-comment
# check-path-safety.sh - every LittleFS.open() whose path is NOT a string
# literal must be a reviewed call site, because that path can come from the
# shell, the HTTP API or the MQTT CLI and must pass Shell::pathSafe() first.
#
# Literal paths ("/config.json") are safe by construction and are ignored, which
# is what keeps the allow-list to the handful of dynamic sites instead of all 28
# opens. A new dynamic open fails the build until it is added below WITH the
# guard that makes it safe named in the comment.
#
# Allow-list entries are <file>:<exact call text>, so moving code around does
# not break the build but changing or adding a call does.
#
# Usage: scripts/check-path-safety.sh
# Exit 0 = every dynamic open is allow-listed, 1 = at least one is not.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

# Reviewed dynamic call sites. Each line: the guard that makes it safe.
ALLOWED=(
  # path checked by _pathSafe -> Shell::pathSafe at HttpServer.cpp:676/698/731
  "lib/thesada-mod-httpserver/src/HttpServer.cpp:LittleFS.open(path.c_str(), FILE_READ)"
  "lib/thesada-mod-httpserver/src/HttpServer.cpp:LittleFS.open(path.c_str(), FILE_WRITE)"
  # sha256File: internal callers only, fixed paths from the config schema
  "lib/thesada-core/src/MQTTClient.cpp:LittleFS.open(path, \"r\")"
  # executeFile: called with literal /scripts/*.lua, and via lua.load which
  # checks Shell::pathSafe on argv[1] before reaching here
  "lib/thesada-mod-scriptengine/src/ScriptEngine.cpp:LittleFS.open(path, \"r\")"
  # lua.load: Shell::pathSafe(argv[1]) checked immediately above the open
  "lib/thesada-mod-scriptengine/src/ScriptEngine.cpp:LittleFS.open(argv[1], \"r\")"
)

# First-party source only. base/.pio and build/ hold vendored deps and artifacts.
# find -exec, not mapfile: this has to run on the macOS bash 3.2 too.
hits="$(find lib src examples -type f \
  \( -name '*.cpp' -o -name '*.h' -o -name '*.ino' \) \
  -exec grep -Hn "LittleFS\.open(" {} + 2>/dev/null | sort || true)"

if ! find lib src examples -type f -name '*.cpp' 2>/dev/null | read -r _; then
  echo "check-path-safety: no sources found - wrong working directory?" >&2
  exit 1
fi

status=0
found=0

while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  file="${hit%%:*}"
  rest="${hit#*:}"
  line="${rest%%:*}"
  code="${rest#*:}"

  # Isolate the call and its first argument.
  call="LittleFS.open(${code#*LittleFS.open(}"
  arg="${call#LittleFS.open(}"

  # A literal path starts the argument list with a double quote. Safe.
  case "$arg" in
    '"'*) continue ;;
  esac

  found=$((found + 1))
  # Cut at the paren that closes the call, not the first one seen - the first
  # is usually c_str()'s and would collapse read and write sites onto one key.
  trimmed="$(printf '%s' "$call" | awk '{
    depth = 0
    for (i = 1; i <= length($0); i++) {
      c = substr($0, i, 1)
      if (c == "(") depth++
      else if (c == ")") {
        depth--
        if (depth == 0) { print substr($0, 1, i); exit }
      }
    }
    print $0
  }')"
  key="${file}:${trimmed}"

  ok=0
  for a in "${ALLOWED[@]}"; do
    [ "$a" = "$key" ] && { ok=1; break; }
  done

  if [ "$ok" -eq 1 ]; then
    printf 'ok    %s:%s  %s\n' "$file" "$line" "$trimmed"
  else
    printf 'FAIL  %s:%s  %s\n' "$file" "$line" "$trimmed" >&2
    status=1
  fi
done <<EOF
$hits
EOF

if [ "$status" -ne 0 ]; then
  cat >&2 <<'MSG'

check-path-safety: a LittleFS.open() with a non-literal path is not allow-listed.

That path may reach the device from the shell, the HTTP API or the MQTT CLI.
Guard it with Shell::pathSafe() before the open, then add the call to ALLOWED
in this script with a comment naming the guard. Do not add it without one.
MSG
  exit 1
fi

echo "check-path-safety: ok (${found} dynamic open(s), all allow-listed)"
