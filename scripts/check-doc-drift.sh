#!/bin/sh
# Doc-drift reminder (thesada-fw).
#
# Firmware documentation lives in the separate thesada-doc repository, so
# a local git hook cannot verify that a matching doc commit exists - this
# check only REMINDS, it never blocks. It prints a notice when staged
# files touch firmware source (src/, lib/) so the public docs get the
# same-session update they need.
#
# Shared entry point for scripts/hooks/pre-commit. The file list arrives
# on stdin, one path per line, so the rule matches whatever the caller
# enumerates.
#
# Usage:
#   git diff --cached --name-only | scripts/check-doc-drift.sh
#
# Exit code is always 0 - this is a reminder, not a gate.

set -eu

# Firmware source trees. A change under either should be mirrored into
# thesada-doc (firmware/*.md) in the same session.
SOURCE_REGEX='^(src/|lib/)'

files="$(cat)"
touched="$(printf '%s\n' "$files" | grep -E "$SOURCE_REGEX" || true)"

if [ -n "$touched" ]; then
  cat >&2 <<EOF

doc-drift reminder: firmware source changed in this commit.
  Mirror any user-visible change into the thesada-doc repo this session
  (firmware/*.md - platform, config-web, board notes). Doc lag is a bug.

Source files in this change:
$(printf '%s\n' "$touched" | sed 's/^/  /')
EOF
fi

exit 0
