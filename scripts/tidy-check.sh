#!/usr/bin/env bash

# clang-tidy over the pure policy headers, reached through the native test
# units so the host compiler sees exactly what pio test -e native compiles.
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

TIDY="${CLANG_TIDY:-$(command -v clang-tidy || true)}"
if [ -z "$TIDY" ] && [ -x /opt/homebrew/opt/llvm/bin/clang-tidy ]; then
  TIDY=/opt/homebrew/opt/llvm/bin/clang-tidy
fi
[ -n "$TIDY" ] || { echo "tidy-check: clang-tidy not found - make setup-sys, or CLANG_TIDY=/path/to/clang-tidy"; exit 2; }

UNITY=.pio/libdeps/native/Unity/src
if [ ! -f "$UNITY/unity.h" ]; then
  echo "tidy-check: fetching Unity for the native env"
  pio test -e native --without-testing --without-uploading >/dev/null
fi
[ -f "$UNITY/unity.h" ] || { echo "tidy-check: Unity missing under $UNITY after pio test"; exit 2; }

shopt -s nullglob
units=(test/test_*/test_*.cpp)
[ ${#units[@]} -gt 0 ] || { echo "tidy-check: no test units found"; exit 2; }

echo "tidy-check: $TIDY over ${#units[@]} test units, headers lib/thesada-core/src"
# The static analyzer skips header-defined functions nobody in the TU calls;
# analyze-headers makes it walk every policy function, reached or not.
"$TIDY" --quiet --extra-arg=-Xclang --extra-arg=-analyzer-opt-analyze-headers \
  "${units[@]}" -- -std=gnu++14 -I lib/thesada-core/src -I "$UNITY"
echo "tidy-check: OK"
