#!/bin/sh
# Write the version translation unit. Leaves the file untouched when the
# version string has not changed, so an unchanged sha does not relink.
set -eu
if [ "$#" -ne 1 ]; then
  echo "usage: write-version-cpp.sh OUTPUT.cpp" >&2
  exit 1
fi
out=$1
root=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
version=$("$root/scripts/version.sh")
case "$version" in
  *[!0-9A-Za-z.+]*)
    echo "write-version-cpp.sh: refusing version '$version'" >&2
    exit 1
    ;;
esac
tmp=$(mktemp "${TMPDIR:-/tmp}/ergotime-version.XXXXXX")
cat >"$tmp" <<EOF
namespace ergotime {
extern const char version[];
const char version[] = "${version}";
}
EOF
if [ -f "$out" ] && cmp -s "$tmp" "$out"; then
  rm -f "$tmp"
  exit 0
fi
mkdir -p "$(dirname "$out")"
mv "$tmp" "$out"
