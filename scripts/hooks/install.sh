#!/bin/sh
# Install thesada-fw git hooks into .git/hooks via symlink.
#
# Run once after clone. Re-run after pulling new hooks. Symlinks (not
# copies) so a hook edit lands without reinstalling.
#
# Usage:  ./scripts/hooks/install.sh

set -eu

repo_root="$(git rev-parse --show-toplevel)"
hooks_src="$repo_root/scripts/hooks"
hooks_dst="$repo_root/.git/hooks"

if [ ! -d "$hooks_dst" ]; then
  echo "install.sh: $hooks_dst missing - is this a git working tree?" >&2
  exit 1
fi

for hook in pre-commit commit-msg; do
  src="$hooks_src/$hook"
  dst="$hooks_dst/$hook"
  if [ ! -x "$src" ]; then
    chmod +x "$src"
  fi
  ln -sfn "$src" "$dst"
  echo "linked $hook -> $src"
done
