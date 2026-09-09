#!/usr/bin/env bash

# allow-long-comment
# check-lib-deps.sh - every local library under lib/ must be listed in
# platformio.ini lib_deps, and every local lib_deps entry must have a
# directory behind it.
#
# PlatformIO only compiles the local libraries named in lib_deps. A library
# that is under lib/ but missing from that list is never put in the
# dependency graph, never compiled, and never linked - so its module never
# registers and its ENABLE_ flag does nothing at all. Someone follows the
# README, uncomments the flag, and gets no behaviour and nothing to debug.
#
# No other gate catches this: a library that is never built cannot fail to
# build, so the whole matrix stays green while the module is dead. That is
# why this reads the wiring instead of trusting a successful build.
#
# Bash builtins only, no sed/grep/awk: this runs on the ubuntu runner and on
# a macOS dev box, where those three differ in dialect.
#
# Usage: scripts/check-lib-deps.sh
# Exit 0 = every lib/ directory is wired and every local dep exists,
#       1 = at least one is not.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

INI=platformio.ini
fail=0

# A lib_deps entry for a LOCAL library is a bare indented token drawn only
# from [A-Za-z0-9_.-]. Whitelisting that charset rather than blacklisting
# separators keeps external deps (owner/pkg @ ver, URLs), ini keys, section
# headers, build flags and PlatformIO's ${env.*} interpolation all out.
declare -a DEPS=()
while IFS= read -r raw || [ -n "$raw" ]; do
  [[ "$raw" == [[:space:]]* ]] || continue
  line="${raw%%;*}"
  line="${line#"${line%%[![:space:]]*}"}"
  line="${line%"${line##*[![:space:]]}"}"
  [ -n "$line" ] || continue
  case "$line" in
    -*)                     continue ;;
    *[!A-Za-z0-9_.-]*)      continue ;;
  esac
  DEPS+=("$line")
done < "$INI"

listed() {
  local want=$1 d
  for d in ${DEPS+"${DEPS[@]}"}; do
    [ "$d" = "$want" ] && return 0
  done
  return 1
}

# library.json is what makes a directory a PlatformIO library.
count=0
for dir in lib/*/; do
  name=${dir#lib/}
  name=${name%/}
  [ -f "${dir}library.json" ] || continue
  count=$((count + 1))
  if ! listed "$name"; then
    echo "MISSING  lib/${name}/ is not in ${INI} lib_deps - it will never be compiled"
    fail=1
  fi
done

# The reverse. PlatformIO fails late and unhelpfully on a dep with no source.
for name in ${DEPS+"${DEPS[@]}"}; do
  if [ ! -d "lib/$name" ]; then
    echo "ORPHAN   ${name} is in ${INI} lib_deps but lib/${name}/ does not exist"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  echo "check-lib-deps: FAILED"
  exit 1
fi

echo "check-lib-deps: ok (${count} local libraries, all wired)"
