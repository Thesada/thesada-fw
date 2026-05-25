#!/bin/sh
# thesada-fw commit-message lint.
#
# thesada-fw is a public repo. Commit messages are mirrored to GitHub and
# show up verbatim in `git log` and in auto-generated release notes, so
# private references must never land in one. This scans a single commit
# message for the recurring offenders:
#
#   - internal issue-tracker refs    #<digits>
#   - internal hostnames             (use example.com in public text)
#   - RFC1918 IP addresses           10/172.16-31/192.168
#
# Merge commits are skipped - their `#<n>` is a GitHub PR number, which is
# public and legitimate.
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

# Merge commits carry a public GitHub PR number - not ours to police.
first_line=$(head -n1 "$msg_file")
case "$first_line" in
  "Merge "*) exit 0 ;;
esac

# Drop comment lines (the git template's `#` block) so they cannot
# false-positive as tracker refs. Real refs appear inline, not at col 0.
body=$(grep -v '^#' "$msg_file" || true)

fail=0
report() {
  # in: $1 label, $2 matching lines
  [ -z "$2" ] && return 0
  echo "commit-msg lint: $1" >&2
  printf '%s\n' "$2" | sed 's/^/    /' >&2
  fail=1
}

report "internal issue-tracker ref (#NN) - drop it or describe the change instead" \
  "$(printf '%s\n' "$body" | grep -nE '#[0-9]+' || true)"

report "internal hostname - use example.com in public text" \
  "$(printf '%s\n' "$body" | grep -nE '[A-Za-z0-9-]+\.thesada\.app' || true)"

report "private IP address (RFC1918) - redact it" \
  "$(printf '%s\n' "$body" | grep -nE '\b(10\.[0-9]{1,3}|192\.168|172\.(1[6-9]|2[0-9]|3[01]))\.[0-9]{1,3}\.[0-9]{1,3}\b' || true)"

if [ "$fail" -ne 0 ]; then
  echo "" >&2
  echo "thesada-fw is public - sanitize the message, or bypass with:" >&2
  echo "    MSG_OK=1   (env var for the commit)" >&2
  exit 1
fi
exit 0
