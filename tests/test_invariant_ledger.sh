#!/usr/bin/env bash
# Self-test for scripts/check-invariant-ledger.sh: each case states its exit code.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

gate=scripts/check-invariant-ledger.sh
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

check() {
  local want="$1" got="$2" label="$3"
  if [ "$got" = "$want" ]; then
    printf 'ok    %s\n' "$label"
  else
    printf 'FAIL  %s (exit %s, want %s)\n' "$label" "$got" "$want" >&2
    fail=1
  fi
}

# feed <want> <label> <path>...: the paths go to the gate on stdin.
feed() {
  local want="$1" label="$2" got=0
  shift 2
  printf '%s\n' "$@" | INVARIANT_OK='' "$gate" >/dev/null 2>&1 || got=$?
  check "$want" "$got" "$label"
}

feed 1 "policy header without the ledger" lib/thesada-core/src/ota_health_policy.h
feed 1 "src/main.cpp without the ledger" src/main.cpp README.md
feed 1 "module source without the ledger" lib/thesada-mod-cellular/src/CellularModule.cpp
feed 1 "brace citation TlsClient.{h,cpp}" lib/thesada-core/src/TlsClient.cpp
feed 0 "policy header with the ledger" lib/thesada-core/src/ota_health_policy.h docs/invariants.md
feed 0 "uncited files" README.md test/test_ota_health/test_ota_health.cpp
feed 0 "empty change"
# Release and dependency bumps touch these alone: the ledger names them in prose.
feed 0 "release bump" src/thesada_config.h
feed 0 "lib_deps bump" platformio.ini
feed 0 "new coverage floor" scripts/coverage-floors.txt

got=0
printf '%s\n' src/main.cpp | INVARIANT_OK=1 "$gate" >/dev/null 2>&1 || got=$?
check 0 "$got" "INVARIANT_OK=1 bypass"

got=0
INVARIANT_OK='' "$gate" --range 0000000000000000000000000000000000000001...HEAD \
  </dev/null >/dev/null 2>&1 || got=$?
check 2 "$got" "a range git cannot diff fails"

got=0
INVARIANT_OK='' "$gate" --range HEAD...HEAD </dev/null >/dev/null 2>&1 || got=$?
check 0 "$got" "an empty range passes"

got=0
list="$("$gate" --list </dev/null 2>/dev/null)" || got=$?
check 0 "$got" "--list: every cited name matches a tracked file"
for want in src/main.cpp lib/thesada-core/src/ota_health_policy.h \
    lib/thesada-core/src/TlsClient.h lib/thesada-mod-scriptengine/src/ScriptEngine.cpp; do
  got=1
  printf '%s\n' "$list" | grep -qxF "$want" && got=0
  check 0 "$got" "--list names $want"
done

printf 'Source: `lib/thesada-core/src/Shell.cpp`, `lib/nope/Gone.cpp`.\n' >"$tmp/stale.md"
got=0
INVARIANT_LEDGER="$tmp/stale.md" "$gate" --list </dev/null >/dev/null 2>&1 || got=$?
check 1 "$got" "--list: a cited name with no file fails"

printf '# no citations\n' >"$tmp/empty.md"
got=0
printf '%s\n' src/main.cpp | INVARIANT_LEDGER="$tmp/empty.md" "$gate" >/dev/null 2>&1 || got=$?
check 2 "$got" "a ledger with no Source: lines fails"

got=0
printf '%s\n' src/main.cpp | INVARIANT_LEDGER="$tmp/missing.md" "$gate" >/dev/null 2>&1 || got=$?
check 2 "$got" "a missing ledger fails"

[ "$fail" -eq 0 ] && echo "test_invariant_ledger: ok"
exit "$fail"
