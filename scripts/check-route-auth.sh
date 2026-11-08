#!/usr/bin/env bash

# Every HTTP route authenticates through _authOrReply / _bodyOrReply, so the login lockout
# covers it: raw _checkAuth and authenticate() calls only at the listed spots.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

HS=lib/thesada-mod-httpserver/src/HttpServer.cpp
ALLOWED=(
  "static bool _checkAuth(AsyncWebServerRequest* req, bool hasSideEffect = false,"
  "req->authenticate(webUser, webPass);"
  "if (!_checkAuth(req, hasSideEffect, &guessed)) {"
  "if (index == 0 && _checkAuth(req)) req->_tempObject = httpBodyBegin(total, maxBytes);"
  "if (index == 0 && _checkAuth(req)) req->_tempObject = malloc(1);"
)

[ -f "$HS" ] || { echo "check-route-auth: $HS not found" >&2; exit 1; }

hits="$(find lib src -type f \( -name '*.cpp' -o -name '*.h' \) \
  -exec grep -HnE '(_checkAuth|[^A-Za-z_]authenticate)[[:space:]]*\(' {} + 2>/dev/null | sort || true)"

status=0
seen=0
while IFS= read -r hit; do
  [ -z "$hit" ] && continue
  seen=$((seen + 1))
  file="${hit%%:*}"
  text="$(printf '%s' "${hit#*:*:}" | sed -E 's/^[[:space:]]+//')"
  ok=0
  if [ "$file" = "$HS" ]; then
    for a in "${ALLOWED[@]}"; do
      [ "$text" = "$a" ] && { ok=1; break; }
    done
  fi
  if [ "$ok" -eq 0 ]; then
    echo "check-route-auth: raw auth call outside the lockout path: $hit" >&2
    status=1
  fi
done <<< "$hits"

if [ "$seen" -lt "${#ALLOWED[@]}" ]; then
  echo "check-route-auth: found $seen auth calls, expected at least ${#ALLOWED[@]} - pattern stale?" >&2
  status=1
fi
[ "$status" -eq 0 ] && echo "check-route-auth: ok ($seen calls, all on the lockout path)"
exit "$status"
