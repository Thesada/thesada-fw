#!/bin/sh
# Public commit-msg lint: strip Cursor trailers; reject private refs.
# Bypass: MSG_OK=1 git commit ...
set -eu

msg_file="${1:?usage: check-commit-msg.sh <commit-msg-file>}"

if [ "${MSG_OK:-}" = "1" ]; then
  echo "commit-msg lint: MSG_OK=1 set, skipping." >&2
  exit 0
fi

first_line=$(head -n1 "$msg_file")
case "$first_line" in
  "Merge "*) exit 0 ;;
esac

# Strip Cursor Agent --trailer injections (land before this hook).
tmp_strip=$(mktemp)
grep -vE '^[[:space:]]*(Co-authored-by:[[:space:]]*Cursor[[:space:]]*<cursoragent@cursor\.com>|Made-with:[[:space:]]*Cursor|Made with \[Cursor\])' \
  "$msg_file" >"$tmp_strip" || true
if [ -s "$tmp_strip" ]; then
  awk 'NF{p=1} p{print}' "$tmp_strip" | sed -e :a -e '/^\n*$/{$d;N;ba' -e '}' >"$msg_file" || cp "$tmp_strip" "$msg_file"
fi
rm -f "$tmp_strip"

body=$(grep -v '^#' "$msg_file" || true)

fail=0
report() {
  [ -z "$2" ] && return 0
  echo "commit-msg lint: $1" >&2
  printf '%s\n' "$2" | sed 's/^/    /' >&2
  fail=1
}

report "Cursor attribution trailer - remove Co-authored-by/Made-with Cursor lines" \
  "$(printf '%s\n' "$body" | grep -nEi 'Co-authored-by:[[:space:]]*Cursor|Made-with:[[:space:]]*Cursor|Made with \[Cursor\]|cursoragent@cursor\.com' || true)"

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
