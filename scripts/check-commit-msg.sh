#!/bin/sh
# thesada-fw commit-message lint.
#
# Development happens on Forgejo; publish copies commit subjects into the
# public GitHub release body (it strips #<n> issue refs itself). This scans
# a single commit message for what must never reach that body:
#
#   - internal hostnames             (use example.com in public text)
#   - RFC1918 IP addresses           10/172.16-31/192.168
#
# in:  $1 - path to the commit message file (as passed to a commit-msg hook)
# out: exit 0 clean, exit 1 with the offending lines on a hit.
#
# Bypass (genuine false positive only - leaves a trail in the message):
#     MSG_OK=1 git commit ...
set -eu

msg_file="${1:?usage: check-commit-msg.sh <commit-msg-file>}"

if [ "${MSG_OK:-}" = "1" ]; then
  echo "commit-msg lint: MSG_OK=1 set, skipping." >&2
  exit 0
fi

# Drop comment lines (the git template's `#` block).
body=$(grep -v '^#' "$msg_file" || true)

fail=0
report() {
  # in: $1 label, $2 matching lines
  [ -z "$2" ] && return 0
  echo "commit-msg lint: $1" >&2
  printf '%s\n' "$2" | sed 's/^/    /' >&2
  fail=1
}

report "internal hostname - use example.com in public text" \
  "$(printf '%s\n' "$body" | grep -nE '[A-Za-z0-9-]+\.thesada\.app' || true)"

report "private IP address (RFC1918) - redact it" \
  "$(printf '%s\n' "$body" | grep -nE '\b(10\.[0-9]{1,3}|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}\b' || true)"

if [ "$fail" -ne 0 ]; then
  echo "" >&2
  echo "commit subjects reach the public release body - sanitize the message, or bypass with:" >&2
  echo "    MSG_OK=1   (env var for the commit)" >&2
  exit 1
fi
exit 0
