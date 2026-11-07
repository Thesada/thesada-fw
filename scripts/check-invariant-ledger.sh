#!/bin/sh
# Invariant-ledger gate (pre-commit and CI). Run with --help for the contract.

set -eu

usage() {
  cat >&2 <<'EOF'
usage: git diff --cached --name-only | check-invariant-ledger.sh
       check-invariant-ledger.sh --range BASE...HEAD
       check-invariant-ledger.sh --list

A change to a file that docs/invariants.md cites on a Source: line must also
change the ledger. Citing a file there puts it under the gate. --list prints
the gated files in the work tree, tracked or not ignored.
Bypass: INVARIANT_OK=1, or an `INVARIANT_OK: 1` trailer in the CI range.
Exit 0 clean, 1 ledger not in the change (--list: a cited name matches no
file), 2 the ledger or the range could not be read.
EOF
  exit 2
}

root="$(cd "$(dirname "$0")/.." && pwd)"
ledger="${INVARIANT_LEDGER:-$root/docs/invariants.md}"
if [ ! -r "$ledger" ]; then
  echo "invariant-ledger check: cannot read $ledger" >&2
  exit 2
fi

# Print the stdin paths the ledger cites, a bare name matching any path ending
# in /name. With $1=1 a cited name that no path matches fails.
gated() {
  awk -v ledger="$ledger" -v list="$1" '
    function cite(tok,   stem, n, i, ext) {
      if (match(tok, /[{][a-z,]+[}]$/)) {
        stem = substr(tok, 1, RSTART - 1)
        n = split(substr(tok, RSTART + 1, RLENGTH - 2), ext, ",")
        for (i = 1; i <= n; i++) names[stem ext[i]] = 0
      } else {
        names[tok] = 0
      }
    }
    BEGIN {
      re = "[A-Za-z0-9_./-]+[.](cpp|c|h|hpp|ino|ini|sh|py|lua|html|js|txt|csv|yml|example|[{][a-z,]+[}])[^A-Za-z0-9_]"
      while ((getline line < ledger) > 0) {
        if (line ~ /^Source:/) on = 1
        if (line ~ /^[ \t\r]*$/) on = 0
        if (!on) continue
        s = line " "
        while (match(s, re)) {
          tok = substr(s, RSTART, RLENGTH - 1)
          s = substr(s, RSTART + RLENGTH - 1)
          cite(tok)
        }
      }
      for (k in names) count++
      if (!count) {
        print "invariant-ledger check: no Source: citations in " ledger > "/dev/stderr"
        dead = 1
        exit
      }
    }
    {
      hit = 0
      for (k in names) {
        if ($0 == k || substr($0, length($0) - length(k)) == "/" k) { names[k]++; hit = 1 }
      }
      if (hit) print
    }
    END {
      if (dead) exit 2
      if (list != 1) exit 0
      for (k in names) {
        if (!names[k]) {
          print "invariant-ledger check: cited name matches no file: " k > "/dev/stderr"
          bad = 1
        }
      }
      exit bad
    }'
}

case "${1:-}" in
  --list)
    git -C "$root" ls-files --cached --others --exclude-standard | gated 1
    exit 0
    ;;
  --range)
    [ -n "${2:-}" ] || usage
    if ! files="$(git -C "$root" diff --name-only "$2")"; then
      echo "invariant-ledger check: cannot diff $2" >&2
      exit 2
    fi
    ;;
  "")
    files="$(cat)"
    ;;
  *)
    usage
    ;;
esac

touched_sensitive="$(printf '%s\n' "$files" | gated 0)" || exit 2

if [ -n "$touched_sensitive" ] && ! printf '%s\n' "$files" | grep -qxF docs/invariants.md; then
  if [ "${INVARIANT_OK:-}" = "1" ]; then
    echo "invariant-ledger check: INVARIANT_OK=1 set, skipping." >&2
    exit 0
  fi
  cat >&2 <<EOF
invariant-ledger check: load-bearing source touched without docs/invariants.md update.

Sensitive files in this change (cited on a Source: line of the ledger):
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
