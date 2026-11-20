#!/usr/bin/env bash
# The release tag names FIRMWARE_VERSION at the built commit and points at that
# commit. CI's release job runs it from the checkout, tests/test_release_target.sh covers it.
#
#   release-target.sh plan   <tag> <sha>   prints create or skip, fails on a mismatch
#   release-target.sh create <tag> <sha> <notes file> <asset>...
#   release-target.sh verify <tag> <sha>
#
# Needs GITHUB_REPOSITORY and an authenticated gh.

set -euo pipefail

usage() {
  echo "usage: $0 plan|verify <tag> <sha>, or create <tag> <sha> <notes> <asset>..." >&2
  exit 2
}
die() {
  echo "release-target: $*" >&2
  exit 1
}

[ $# -ge 3 ] || usage
mode="$1" tag="$2" sha="$3"
shift 3
case "$mode" in
  plan | verify) [ $# -eq 0 ] || usage ;;
  create) [ $# -ge 2 ] || usage ;;
  *) usage ;;
esac
repo="${GITHUB_REPOSITORY:?GITHUB_REPOSITORY unset}"

# api <path> <jq>: the answer, empty on a 404. Any other error stops the job.
api() {
  local out rc=0 err
  err="$(mktemp)"
  out="$(gh api "repos/$repo/$1" --jq "$2" 2>"$err")" || rc=$?
  if [ "$rc" -ne 0 ]; then
    if grep -q 'HTTP 404' "$err"; then
      rm -f "$err"
      return 0
    fi
    cat "$err" >&2
    rm -f "$err"
    die "GitHub read of $1 failed"
  fi
  rm -f "$err"
  printf '%s\n' "$out"
}

# tag_commit: the commit the tag points at, through annotated tags, empty when absent.
tag_commit() {
  local out type obj hops=0
  out="$(api "git/ref/tags/$tag" '.object.type + " " + .object.sha')" || exit 1
  [ -n "$out" ] || return 0
  read -r type obj <<<"$out"
  while [ "$type" = tag ]; do
    hops=$((hops + 1))
    [ "$hops" -le 5 ] || die "tag $tag: more than 5 nested tag objects"
    out="$(api "git/tags/$obj" '.object.type + " " + .object.sha')" || exit 1
    read -r type obj <<<"$out"
  done
  [ "$type" = commit ] || die "tag $tag points at a ${type:-missing object}, not a commit"
  printf '%s\n' "$obj"
}

check_tag() {
  local got
  got="$(tag_commit)" || exit 1
  [ "$got" = "$sha" ] || die "tag $tag is on ${got:-nothing}, the build is $sha"
}

if [ "$mode" = verify ]; then
  check_tag
  echo "release-target: tag $tag is on $sha"
  exit 0
fi

# version_at <commit>: FIRMWARE_VERSION in the config header at that commit.
version_at() {
  git show "$1:src/thesada_config.h" 2>/dev/null |
    sed -n 's/^#define FIRMWARE_VERSION "\([0-9.]*\)".*/\1/p'
}

head="$(git rev-parse HEAD)"
[ "$head" = "$sha" ] || die "checkout is $head, the build is $sha"
version="$(version_at "$sha")"
[ "$version" = "$tag" ] || die "FIRMWARE_VERSION at $sha is ${version:-missing}, the tag is $tag"

if [ "$mode" = create ]; then
  notes="$1"
  shift
  gh release create "$tag" "$@" --title "thesada-fw $tag" --notes-file "$notes" --target "$sha"
  check_tag
  echo "release-target: created $tag on $sha"
  exit 0
fi

existing="$(tag_commit)" || exit 1
release="$(api "releases/tags/$tag" .id)" || exit 1
if [ -z "$existing" ]; then
  [ -z "$release" ] || die "release $tag exists without its tag"
  echo create
elif [ "$existing" = "$sha" ]; then
  if [ -n "$release" ]; then echo skip; else echo create; fi
elif [ -n "$release" ] && [ "$(version_at "$existing")" = "$tag" ] &&
  git merge-base --is-ancestor "$existing" "$sha" 2>/dev/null; then
  # main moved on without a version bump: released earlier, on its own commit.
  echo skip
else
  die "tag $tag is on $existing, which does not carry $tag on the way to $sha. Fix the tag by hand, this job never moves one"
fi
