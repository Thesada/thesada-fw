#!/usr/bin/env bash
# Self-test for scripts/release-target.sh against a fake gh that records its calls.

set -euo pipefail
cd "$(cd "$(dirname "$0")/.." && pwd)"

gate="$PWD/scripts/release-target.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
fail=0

# A throwaway history: c1 releases 1.0.0, c2 moves main on, c3 bumps to 1.0.1.
# side carries 0.9.9 and side2 a stray 1.0.0, neither on the way to c2.
repo="$tmp/repo"
git init -q "$repo"
commit() {
  mkdir -p "$repo/src"
  printf '#define FIRMWARE_VERSION "%s"  // CalVer\n' "$1" >"$repo/src/thesada_config.h"
  git -C "$repo" add src/thesada_config.h
  git -C "$repo" -c core.hooksPath=/dev/null -c commit.gpgsign=false \
    -c user.name=t -c user.email=t@example.invalid commit -q --allow-empty -m "$2"
  git -C "$repo" rev-parse HEAD
}
c1="$(commit 1.0.0 c1)"
side="$(commit 0.9.9 side)"
git -C "$repo" checkout -q --detach "$c1"
side2="$(commit 1.0.0 side2)"
git -C "$repo" checkout -q --detach "$c1"
c2="$(commit 1.0.0 c2)"
c3="$(commit 1.0.1 c3)"
other=1111111111111111111111111111111111111111

# The fake reads its answers from files in $tmp/s: ref (type and sha of the tag ref,
# empty is a 404), tagobj (what an annotated tag points at), release (1 exists),
# err (1 makes every read a 500), retarget (sha GitHub tags on create instead).
mkdir -p "$tmp/bin"
cat >"$tmp/bin/gh" <<'GH'
#!/usr/bin/env bash
s="$FAKE_GH_STATE"
printf '%s\n' "$*" >>"$s/calls"
get() { cat "$s/$1" 2>/dev/null || true; }
if [ "$(get err)" = 1 ]; then echo "gh: Server Error (HTTP 500)" >&2; exit 1; fi
case "$1 $2" in
  "api repos/"*/git/ref/tags/*)
    r="$(get ref)"
    [ -n "$r" ] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
    echo "$r" ;;
  "api repos/"*/git/tags/*)
    get tagobj ;;
  "api repos/"*/releases/tags/*)
    [ "$(get release)" = 1 ] || { echo "gh: Not Found (HTTP 404)" >&2; exit 1; }
    echo 42 ;;
  "release create")
    t=""
    while [ $# -gt 0 ]; do [ "$1" = --target ] && t="$2"; shift; done
    r="$(get retarget)"
    echo "commit ${r:-$t}" >"$s/ref"
    echo 1 >"$s/release" ;;
  *) echo "fake gh: unexpected call: $*" >&2; exit 9 ;;
esac
GH
chmod +x "$tmp/bin/gh"

# run <want exit> <stdout on 0, a stderr substring otherwise, or -> <label> <state k=v...> -- <args...>
run() {
  local want="$1" wantout="$2" label="$3" got=0 out
  shift 3
  rm -rf "$tmp/s" && mkdir "$tmp/s"
  while [ "$1" != -- ]; do printf '%s' "${1#*=}" >"$tmp/s/${1%%=*}"; shift; done
  shift
  out="$(cd "$repo" && PATH="$tmp/bin:$PATH" FAKE_GH_STATE="$tmp/s" GITHUB_REPOSITORY=o/r \
    "$gate" "$@" 2>"$tmp/err")" || got=$?
  if [ "$got" != "$want" ]; then
    printf 'FAIL  %s (exit %s, want %s)\n' "$label" "$got" "$want" >&2
    sed 's/^/      /' "$tmp/err" >&2
    fail=1
  elif [ "$want" = 0 ] && [ "$wantout" != - ] && [ "$out" != "$wantout" ]; then
    printf 'FAIL  %s (printed %s, want %s)\n' "$label" "$out" "$wantout" >&2
    fail=1
  elif [ "$want" != 0 ] && [ "$wantout" != - ] && ! grep -qF "$wantout" "$tmp/err"; then
    printf 'FAIL  %s (stderr lacks: %s)\n' "$label" "$wantout" >&2
    sed 's/^/      /' "$tmp/err" >&2
    fail=1
  else
    printf 'ok    %s\n' "$label"
  fi
}

git -C "$repo" checkout -q --detach "$c3"
v=1.0.1
run 0 create "no tag, no release: create" ref= -- plan $v "$c3"
run 0 skip "tag on the built commit with its release: skip" ref="commit $c3" release=1 -- plan $v "$c3"
run 0 create "tag on the built commit, no release: create" ref="commit $c3" -- plan $v "$c3"
run 0 skip "annotated tag on the built commit: skip" ref="tag $other" tagobj="commit $c3" release=1 -- plan $v "$c3"
run 1 "is on $c2, which does not carry $v" "tag on a commit without the version fails" ref="commit $c2" release=1 -- plan $v "$c3"
run 1 "is on $c2" "annotated tag on another commit fails" ref="tag $c3" tagobj="commit $c2" release=1 -- plan $v "$c3"
run 1 "exists without its tag" "release without its tag fails" ref= release=1 -- plan $v "$c3"
run 1 "the tag is 0.0.0" "tag that is not the config version fails" ref= -- plan 0.0.0 "$c3"
run 1 "the build is $c2" "built commit that is not the checkout fails" ref= -- plan $v "$c2"
run 1 "GitHub read of git/ref/tags/$v failed" "an API error fails, never reads as no tag" err=1 -- plan $v "$c3"
run 2 usage: "missing arguments" -- plan $v
run 2 usage: "unknown mode" -- tag $v "$c3"

git -C "$repo" checkout -q --detach "$c2"
run 0 skip "main moved on past a released version: skip" ref="commit $c1" release=1 -- plan 1.0.0 "$c2"
run 1 "is on $c1" "earlier tag without its release fails" ref="commit $c1" -- plan 1.0.0 "$c2"
run 1 "is on $side, which does not carry" "tag on a commit with another version fails" ref="commit $side" release=1 -- plan 1.0.0 "$c2"
run 1 "is on $side2, which does not carry" "tag off the build's history fails" ref="commit $side2" release=1 -- plan 1.0.0 "$c2"
run 1 "is on $other" "tag on an unknown commit fails" ref="commit $other" release=1 -- plan 1.0.0 "$c2"

git -C "$repo" checkout -q --detach "$c3"
touch "$tmp/notes.md"
run 0 - "create passes the built commit and verifies it" ref= -- create $v "$c3" "$tmp/notes.md" a.bin b.json
if grep -qxF "release create $v a.bin b.json --title thesada-fw $v --notes-file $tmp/notes.md --target $c3" "$tmp/s/calls"; then
  echo "ok    create call carries --target"
else
  echo "FAIL  create call without the target:" >&2
  sed 's/^/      /' "$tmp/s/calls" >&2 || true
  fail=1
fi
run 1 "is on $other, the build is $c3" "create fails when GitHub tags another commit" ref= retarget="$other" -- create $v "$c3" "$tmp/notes.md" a.bin

run 0 - "verify: tag on the built commit" ref="commit $c3" -- verify $v "$c3"
run 1 "is on $c2" "verify: tag on another commit fails" ref="commit $c2" -- verify $v "$c3"
run 1 "is on nothing" "verify: no tag fails" ref= -- verify $v "$c3"

[ "$fail" -eq 0 ] && echo "test_release_target: ok"
exit "$fail"
