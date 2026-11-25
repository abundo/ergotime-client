#!/usr/bin/env bash
# Strip a release executable and pack it with UPX. The result stays executable.
#
#   ./scripts/pack-upx.sh build/ergotime-static
#   MINGW_STRIP=x86_64-w64-mingw32-strip ./scripts/pack-upx.sh build/ergotime.exe
#
# --lzma is the tight method that still finishes quickly. --best retries
# filters for a long time on a static Qt binary.
set -euo pipefail

bin="${1:?usage: pack-upx.sh BINARY}"
if [[ ! -f "$bin" ]]; then
  echo "error: not a file: $bin" >&2
  exit 1
fi

desc="$(file -b "$bin")"
case "$desc" in
  *"PE32"*)
    stripper="${MINGW_STRIP:-x86_64-w64-mingw32-strip}"
    if ! command -v "$stripper" >/dev/null 2>&1; then
      stripper=strip
    fi
    "$stripper" --strip-unneeded "$bin"
    ;;
  *"Mach-O"*)
    strip "$bin"
    # UPX 4 and 5 refuse Mach-O unless --force-macos is passed, and a
    # forced pack does not run under current macOS. Ship the stripped binary.
    echo "pack-upx: leaving Mach-O unpacked; UPX does not support this binary"
    exit 0
    ;;
  *)
    strip --strip-unneeded "$bin"
    ;;
esac

command -v upx >/dev/null 2>&1 || {
  echo "error: upx is not installed" >&2
  exit 1
}

upx -q --lzma "$bin"
upx -t "$bin" >/dev/null
