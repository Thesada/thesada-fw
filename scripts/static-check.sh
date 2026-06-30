#!/usr/bin/env bash

# cppcheck over the pure security units - catches the over-read class bench misses.
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

shopt -s nullglob
files=(
  lib/thesada-core/src/*_policy.h
  lib/thesada-core/src/*_payload.h
  lib/thesada-core/src/*_keymap.h
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
echo "static-check: OK"
