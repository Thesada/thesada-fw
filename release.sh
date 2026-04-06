#!/usr/bin/env bash
# thesada-fw release script
# Usage: ./release.sh
# Builds firmware, commits staged changes, tags, and creates a GitHub release.
# Requires: pio, git, gh (GitHub CLI, authenticated)
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")" && pwd)"
BASE="$REPO_ROOT/base"

# ── 1. Read version from config.h ────────────────────────────────────────────
VERSION=$(grep -oP '(?<=")\d+\.\d+\.\d+(?=")' "$BASE/src/thesada_config.h")
TAG="v$VERSION"
echo "=== Building thesada-fw $TAG ==="

# ── 2. Build full firmware ────────────────────────────────────────────────────
cd "$BASE"
echo "--- Full build (all modules) ---"
pio run -e esp32-s3-dev

BIN="$BASE/.pio/build/esp32-s3-dev/firmware.bin"
MANIFEST="$BASE/build/firmware.json"
[ -f "$BIN" ]      || { echo "ERROR: firmware.bin not found"; exit 1; }
[ -f "$MANIFEST" ] || { echo "ERROR: firmware.json not found"; exit 1; }

echo "  firmware : $BIN ($(du -h "$BIN" | cut -f1))"
echo "  manifest : $MANIFEST"

# ── 2b. Build minimal firmware (core only) ───────────────────────────────────
echo "--- Minimal build (core only) ---"
CONFIG_H="$BASE/src/thesada_config.h"
cp "$CONFIG_H" "$CONFIG_H.bak"

# Comment out all optional ENABLE_ defines
sed -i 's/^#define ENABLE_TEMPERATURE/\/\/ #define ENABLE_TEMPERATURE/' "$CONFIG_H"
sed -i 's/^#define ENABLE_ADS1115/\/\/ #define ENABLE_ADS1115/' "$CONFIG_H"
sed -i 's/^#define ENABLE_BATTERY/\/\/ #define ENABLE_BATTERY/' "$CONFIG_H"
sed -i 's/^#define ENABLE_PMU/\/\/ #define ENABLE_PMU/' "$CONFIG_H"
sed -i 's/^#define ENABLE_SD/\/\/ #define ENABLE_SD/' "$CONFIG_H"
sed -i 's/^#define ENABLE_CELLULAR/\/\/ #define ENABLE_CELLULAR/' "$CONFIG_H"
sed -i 's/^#define ENABLE_TELEGRAM/\/\/ #define ENABLE_TELEGRAM/' "$CONFIG_H"
sed -i 's/^#define ENABLE_WEBSERVER/\/\/ #define ENABLE_WEBSERVER/' "$CONFIG_H"
sed -i 's/^#define ENABLE_SCRIPTENGINE/\/\/ #define ENABLE_SCRIPTENGINE/' "$CONFIG_H"

pio run -e esp32-s3-dev
BIN_MINIMAL="$BASE/build/firmware_minimal.bin"
cp "$BASE/.pio/build/esp32-s3-dev/firmware.bin" "$BIN_MINIMAL"
echo "  minimal  : $BIN_MINIMAL ($(du -h "$BIN_MINIMAL" | cut -f1))"

# Restore full config and rebuild so .pio output matches the full release binary
mv "$CONFIG_H.bak" "$CONFIG_H"
echo "--- Restoring full build ---"
pio run -e esp32-s3-dev

# ── 3. Commit and tag ─────────────────────────────────────────────────────────
cd "$REPO_ROOT"
git add -A
git diff --cached --quiet && { echo "Nothing to commit - working tree clean"; } || \
  git commit -m "release: $TAG"

# Create tag (skip if already exists)
if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "Tag $TAG already exists - skipping tag creation"
else
  git tag "$TAG"
fi

git push origin main --tags

# ── 4. Generate changelog from commits since last tag ─────────────────────────
PREV_TAG=$(git describe --tags --abbrev=0 HEAD^ 2>/dev/null || echo "")
if [ -n "$PREV_TAG" ]; then
  CHANGELOG=$(git log "$PREV_TAG..HEAD" --pretty=format:"- %s" --no-merges | grep -v "^- release:")
else
  CHANGELOG=$(git log --pretty=format:"- %s" --no-merges -20 | grep -v "^- release:")
fi

NOTES="## Changes

${CHANGELOG}

**Full diff:** https://github.com/Thesada/thesada-fw/compare/${PREV_TAG:-main}...$TAG"

# ── 5. Create GitHub release ──────────────────────────────────────────────────
if gh release view "$TAG" >/dev/null 2>&1; then
  echo "Release $TAG already exists - uploading assets only"
  gh release upload "$TAG" "$BIN" "$BIN_MINIMAL" "$MANIFEST" --clobber
else
  gh release create "$TAG" "$BIN" "$BIN_MINIMAL" "$MANIFEST" \
    --title "thesada-fw $TAG" \
    --notes "$NOTES"
fi

echo ""
echo "=== Released $TAG ==="
echo "  https://github.com/Thesada/thesada-fw/releases/tag/$TAG"
