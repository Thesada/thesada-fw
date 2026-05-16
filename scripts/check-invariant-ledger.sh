#!/bin/sh
# Invariant-ledger discipline check (shared between pre-commit + CI).
#
# Refuses when load-bearing source files are touched without an update to
# docs/invariants.md. Two call sites:
#
#   1. scripts/hooks/pre-commit
#        Operates on staged files. Aborts the local commit.
#
#   2. .github/workflows/ci.yml
#        Operates on the push or PR commit range. Fails the workflow.
#
# Both pass the file list via stdin (one path per line) so the matching
# logic stays identical regardless of how the caller enumerates files.
#
# Bypass: either set INVARIANT_OK=1 in the environment, or add the trailer
# `INVARIANT_OK: 1` to a commit message in the range. Both leave a trail.
#
# Usage:
#   git diff --cached --name-only | scripts/check-invariant-ledger.sh
#   git diff --name-only A...B     | scripts/check-invariant-ledger.sh

set -eu

SENSITIVE_REGEX='^(lib/thesada-core/src/(OTAUpdate|MQTTClient|Shell|Config)\.cpp|lib/thesada-mod-httpserver/src/[^/]+\.cpp|lib/thesada-mod-cellular/src/Cellular\.cpp)$'

files="$(cat)"

touched_sensitive="$(printf '%s\n' "$files" | grep -E "$SENSITIVE_REGEX" || true)"
touched_ledger="$(printf '%s\n' "$files" | grep -E '^docs/invariants\.md$' || true)"

if [ -n "$touched_sensitive" ] && [ -z "$touched_ledger" ]; then
  if [ "${INVARIANT_OK:-}" = "1" ]; then
    echo "invariant-ledger check: INVARIANT_OK=1 set, skipping." >&2
    exit 0
  fi
  cat >&2 <<EOF
invariant-ledger check: load-bearing source touched without docs/invariants.md update.

Sensitive files in this change:
$touched_sensitive

If this change establishes or relies on a new invariant, update
docs/invariants.md (bump the Dated line) and include it.

If it does not, bypass with:
    INVARIANT_OK=1   (env var for the commit / workflow run)
or include 'INVARIANT_OK: 1' as a trailer in a commit message.

The bypass is explicit on purpose - it leaves an audit trail.
EOF
  exit 1
fi

exit 0
