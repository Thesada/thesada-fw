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

# PlatformIO accepts `lib_deps = A, B` as well as one value per line, and the
# value may sit on the assignment line itself; split on comma before filtering
# or an inline list reads as one malformed token and every module it names is
# reported missing.
add_dep() {
  local v="$1"
  v="${v#"${v%%[![:space:]]*}"}"
  v="${v%"${v##*[![:space:]]}"}"
  [ -n "$v" ] || return 0
  case "$v" in
    -*)                return 0 ;;
    *[!A-Za-z0-9_.-]*) return 0 ;;
  esac
  DEPS+=("$v")
}

add_deps_from() {
  local rest="$1" part
  while [ "$rest" != "${rest#*,}" ]; do
    part="${rest%%,*}"
    add_dep "$part"
    rest="${rest#*,}"
  done
  add_dep "$rest"
}

# Collect only the continuation lines of a `lib_deps =` assignment. Scoping
# matters: an indented bare token under some other multiline key would
# otherwise read as a listed library, and a missing lib_deps entry would pass
# the gate silently - the exact failure this script exists to catch.
#
# Within that block, a LOCAL library is a bare token drawn only from
# [A-Za-z0-9_.-]. Whitelisting that charset rather than blacklisting
# separators keeps external deps (owner/pkg @ ver, URLs) and PlatformIO's
# ${env.*} interpolation out. There are several lib_deps blocks ([env] plus
# the native envs); every one of them counts.
declare -a DEPS=()
in_deps=0
while IFS= read -r raw || [ -n "$raw" ]; do
  line="${raw%%;*}"
  trimmed="${line#"${line%%[![:space:]]*}"}"
  trimmed="${trimmed%"${trimmed##*[![:space:]]}"}"

  # Blank lines carry no structure, inside a block or out.
  [ -n "$trimmed" ] || continue

  # Unindented: a section header or a new key. Only `lib_deps` opens a block.
  if [[ "$raw" != [[:space:]]* ]]; then
    key="${trimmed%%=*}"
    key="${key%"${key##*[![:space:]]}"}"
    if [ "$key" = "lib_deps" ]; then
      in_deps=1
      case "$trimmed" in *=*) add_deps_from "${trimmed#*=}" ;; esac
    else
      in_deps=0
    fi
    continue
  fi

  [ "$in_deps" -eq 1 ] || continue
  add_deps_from "$trimmed"
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

# A run that inspected nothing must not report success: that is the same
# vacuous green this gate exists to remove.
if [ "$count" -eq 0 ]; then
  echo "check-lib-deps: found no lib/*/library.json - wrong directory or broken checkout"
  exit 1
fi

if [ "$fail" -ne 0 ]; then
  echo "check-lib-deps: FAILED"
  exit 1
fi

echo "check-lib-deps: ok (${count} local libraries, all wired)"
