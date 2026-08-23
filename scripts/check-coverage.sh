#!/usr/bin/env bash

# Line coverage over the pure units, floored per file (scripts/coverage-floors.txt).
# Bench tests prove the happy path; this gates the adversarial branches.
set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

OUT_DIR="${COVERAGE_OUT:-build/coverage}"
FLOORS="scripts/coverage-floors.txt"

command -v gcovr >/dev/null || { echo "check-coverage: gcovr not installed (pip install gcovr)"; exit 2; }

# Apple's gcov is an llvm-cov shim and reads clang's gcda fine; on GCC hosts
# the versions have to match the compiler, hence the override.
GCOV_EXE="${GCOV:-gcov}"

if [ "${SKIP_TESTS:-0}" != "1" ]; then
  rm -rf .pio/build/native-cov
  pio test -e native-cov
fi

[ -d .pio/build/native-cov ] || { echo "check-coverage: no coverage build - run without SKIP_TESTS=1"; exit 2; }

mkdir -p "$OUT_DIR"
gcovr \
  --gcov-executable "$GCOV_EXE" \
  --root . \
  --filter 'lib/thesada-core/src/.*\.h' \
  --json-summary "$OUT_DIR/summary.json" \
  --html-details "$OUT_DIR/index.html" \
  --txt "$OUT_DIR/coverage.txt" \
  .pio/build/native-cov >/dev/null

python3 - "$OUT_DIR/summary.json" "$FLOORS" <<'PY'
import json, sys

summary, floors_path = sys.argv[1], sys.argv[2]
data = json.load(open(summary))
seen = {f["filename"]: f for f in data["files"]}

floors = []
for line in open(floors_path):
    line = line.split("#", 1)[0].strip()
    if line:
        path, floor = line.rsplit(None, 1)
        floors.append((path, float(floor)))

fail = []
print(f"{'unit':48s} {'lines':>10s} {'cov':>7s} {'floor':>6s}")
for path, floor in floors:
    f = seen.pop(path, None)
    if f is None:
        print(f"{path:48s} {'-':>10s} {'MISSING':>7s} {floor:6.0f}")
        fail.append(f"{path}: no coverage data (unit gone, or the gcovr filter no longer matches)")
        continue
    pct = f["line_percent"]
    mark = "" if pct >= floor else "  <-- below floor"
    print(f"{path:48s} {f['line_covered']:>4d}/{f['line_total']:<5d} {pct:6.1f}% {floor:6.0f}{mark}")
    if pct < floor:
        fail.append(f"{path}: {pct:.1f}% < {floor:.0f}%")

for path in sorted(seen):
    print(f"{path:48s} {'':>10s} {seen[path]['line_percent']:6.1f}%  (no floor set)")

print(f"\ntotal {data['line_percent']:.1f}%")
if fail:
    print("\ncheck-coverage: FAILED")
    for f in fail:
        print(f"  {f}")
    sys.exit(1)
print("check-coverage: OK")
PY
