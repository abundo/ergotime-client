#!/bin/sh
# Print the client version.
# A clean checkout of the tag vX.Y.Z, where X.Y.Z is project() VERSION in
# CMakeLists.txt, prints X.Y.Z. Every other build prints X.Y.Z+<git sha1>
# (and .dirty when the worktree has changes). The client treats a trailing
# sha as the signal to use ~/.ergotime2-dev.
set -eu
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
base=$(sed -n 's/^project(ergotime VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n 1)
if [ -z "$base" ]; then
  echo "version.sh: set project(ergotime VERSION x.y.z) in CMakeLists.txt" >&2
  exit 1
fi
git() {
  command git -c safe.directory="$root" "$@"
}
if ! git -C "$root" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  printf '%s+unknown\n' "$base"
  exit 0
fi
sha=$(git -C "$root" rev-parse HEAD)
dirty=0
if [ -n "$(git -C "$root" status --porcelain)" ]; then
  dirty=1
fi
exact=0
if [ "${GITHUB_REF:-}" = "refs/tags/v${base}" ]; then
  exact=1
fi
if tag=$(git -C "$root" describe --tags --exact-match HEAD 2>/dev/null); then
  if [ "$tag" = "v${base}" ]; then
    exact=1
  fi
fi
if [ "$exact" -eq 1 ] && [ "$dirty" -eq 0 ]; then
  printf '%s\n' "$base"
  exit 0
fi
if [ "$dirty" -eq 1 ]; then
  printf '%s+%s.dirty\n' "$base" "$sha"
else
  printf '%s+%s\n' "$base" "$sha"
fi
