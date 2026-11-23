#!/usr/bin/env bash

# Every HTTP route authenticates through _authCounted (lockout first, one count per guess):
# the _checkAuth and authenticate identifiers may appear in code only on the lines below, once each.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

HS=lib/thesada-mod-httpserver/src/HttpServer.cpp
ALLOWED=(
  "static bool _checkAuth(AsyncWebServerRequest* req, bool hasSideEffect = false,"
  "req->authenticate(webUser, webPass);"
  "if (!_checkAuth(req, hasSideEffect, &guessed)) {"
)

[ -f "$HS" ] || { echo "check-route-auth: $HS not found" >&2; exit 1; }

# Token match on code with // comments cut, so a call split across lines or taken by address still counts.
# Every route lives in the HTTP server module, an unrelated authenticate() elsewhere is not a route.
hits="$(find lib/thesada-mod-httpserver -type f \( -name '*.cpp' -o -name '*.h' \) -print0 2>/dev/null |
  xargs -0 awk '{ line = $0; sub(/\/\/.*$/, "", line); print FILENAME ":" FNR ":" line }' |
  grep -wE '_checkAuth|authenticate' | sort || true)"

status=0
seen=0
count=()
for i in "${!ALLOWED[@]}"; do count[$i]=0; done
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  seen=$((seen + 1))
  file="${hit%%:*}"
  text="$(printf '%s' "${hit#*:*:}" | sed -E 's/^[[:space:]]+//;s/[[:space:]]+$//')"
  ok=0
  if [ "$file" = "$HS" ]; then
    for i in "${!ALLOWED[@]}"; do
      [ "$text" = "${ALLOWED[$i]}" ] && { ok=1; count[$i]=$((count[$i] + 1)); break; }
    done
  fi
  if [ "$ok" -eq 0 ]; then
    echo "check-route-auth: raw auth use outside the lockout path: $hit" >&2
    status=1
  fi
done <<< "$hits"

for i in "${!ALLOWED[@]}"; do
  if [ "${count[$i]}" -ne 1 ]; then
    echo "check-route-auth: expected the allowed line once, found ${count[$i]}: ${ALLOWED[$i]}" >&2
    status=1
  fi
done
[ "$status" -eq 0 ] && echo "check-route-auth: ok ($seen uses, all on the lockout path)"
exit "$status"
