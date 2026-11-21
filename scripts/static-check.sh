#!/usr/bin/env bash

# cppcheck over the pure security units - catches the over-read class bench misses.
# Then a ban on the ADS1115 library calls that spin without a deadline.
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

shopt -s nullglob
files=(
  lib/thesada-core/src/*_policy.h
  lib/thesada-core/src/*_payload.h
  lib/thesada-core/src/*_keymap.h
  lib/thesada-core/src/meshtastic_*.h
  lib/thesada-core/src/cli_topics.h
  lib/thesada-core/src/secret_heap.h
)
if [ ${#files[@]} -eq 0 ]; then
  echo "static-check: no pure units found - nothing to do"
  exit 0
fi

echo "static-check: cppcheck on ${files[*]}"
cppcheck \
  --enable=warning,performance,portability \
  --inline-suppr \
  --error-exitcode=1 \
  --std=c++17 \
  --language=c++ \
  --suppress=missingIncludeSystem \
  -q \
  "${files[@]}"

# readADC_* spins on a ready bit a NACKed read never sets (invariants.md, Sensors).
ads_src=lib/thesada-mod-ads1115/src
[ -d "$ads_src" ] || { echo "static-check: $ads_src missing - wrong working directory?" >&2; exit 1; }
rc=0
grep -rnE 'readADC_[A-Za-z0-9_]*[[:space:]]*\(' "$ads_src" || rc=$?
if [ "$rc" -eq 0 ]; then
  echo "static-check: readADC_* call in $ads_src, wait through adsWaitReady instead" >&2
  exit 1
fi
if [ "$rc" -ne 1 ]; then
  echo "static-check: grep failed rc=$rc" >&2
  exit 1
fi
# A deep copy of a config doc recurses about 1 KB of stack per level on the 8 KB loop task.
copy_re='\.set\([[:space:]]*(_doc|Config::get\(\)|live)[[:space:]]*\)|JsonDocument[[:space:]]+[A-Za-z_]+[[:space:]]*(\(|=|\{)[[:space:]]*(_doc|Config::get\(\)|live)[[:space:]]*[);}]'
rc=0
grep -rnE "$copy_re" lib/thesada-core/src lib/thesada-mod-*/src src --include='*.cpp' --include='*.h' || rc=$?
if [ "$rc" -eq 0 ]; then
  echo "static-check: deep copy of a config doc, stage from the file through Config::edit instead" >&2
  exit 1
fi
if [ "$rc" -ne 1 ]; then
  echo "static-check: grep failed rc=$rc" >&2
  exit 1
fi
echo "static-check: OK"
