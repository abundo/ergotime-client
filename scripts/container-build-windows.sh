#!/usr/bin/env bash
# Cross-compile a static Windows .exe from Linux inside podman or docker.
#
# The image holds the MinGW toolchain. Qt for Windows is built from the same
# 6.12.0 qtbase sources as the Linux static kit, with the Linux desktop kit
# mounted as QT_HOST_PATH so moc/rcc/uic do not have to be cross-compiled.
# Official aqt Windows kits are shared libraries, and the MSVC kit does not
# link on Linux. miqt-docker's win64 Qt 6.8 image is a Go build of an older Qt.
#
#   ./scripts/container-build-windows.sh            # Qt + build/ergotime.exe
#   ./scripts/container-build-windows.sh install-qt
#   ./scripts/container-build-windows.sh build
#
# Requires $QT_ROOT/6.12.0/gcc_64 (the dynamic Linux kit) for host tools.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-all}"
QT_VERSION="${QT_VERSION:-6.12.0}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
HOST_QT="${QT_HOST_PATH:-$QT_ROOT/$QT_VERSION/gcc_64}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
IMAGE="${IMAGE:-localhost/ergotime-cpp-mingw:${QT_VERSION}}"
PREFIX_REL="static/${QT_VERSION}-mingw"
DOCKERFILE="$ROOT/docker/Dockerfile.windows"

detect_engine() {
  if [[ -n "${CONTAINER_ENGINE:-}" ]]; then
    command -v "$CONTAINER_ENGINE" >/dev/null 2>&1 || {
      echo "error: CONTAINER_ENGINE=$CONTAINER_ENGINE not found" >&2
      exit 1
    }
    echo "$CONTAINER_ENGINE"
    return
  fi
  if command -v podman >/dev/null 2>&1; then
    echo podman
    return
  fi
  if command -v docker >/dev/null 2>&1; then
    echo docker
    return
  fi
  echo "error: neither podman nor docker found" >&2
  exit 1
}

ENGINE="$(detect_engine)"
case "$ACTION" in
  all|install-qt|build) ;;
  *)
    echo "error: unknown action '$ACTION' (use: all | install-qt | build)" >&2
    exit 1
    ;;
esac

if [[ ! -x "$HOST_QT/libexec/moc" ]]; then
  echo "error: Linux Qt host tools not found at $HOST_QT/libexec/moc" >&2
  echo "       install the 6.12 desktop kit first; the Windows build uses it for moc" >&2
  exit 1
fi

echo "==> container Windows static cross-build"
echo "    engine   : $ENGINE"
echo "    action   : $ACTION"
echo "    Qt       : $QT_VERSION"
echo "    host Qt  : $HOST_QT"
echo "    prefix   : $QT_ROOT/$PREFIX_REL"

mkdir -p "$QT_ROOT/src" "$QT_ROOT/build" "$QT_ROOT/static" "$ROOT/build"

echo "==> Building MinGW image"
"$ENGINE" build -f "$DOCKERFILE" -t "$IMAGE" "$ROOT/docker"

RUN=(
  --rm
  -i
  -e "QT_VERSION=$QT_VERSION"
  -e "QT_ROOT=/qt"
  -e "QT_HOST_PATH=/qt-host"
  -e "QT_WINDOWS_PREFIX=/qt/$PREFIX_REL"
  -e "JOBS=$JOBS"
  -e "HOME=/tmp/home"
  -e "ACTION=$ACTION"
  -v "$ROOT:/src:Z"
  -v "$QT_ROOT:/qt:Z"
  -v "$HOST_QT:/qt-host:Z"
  -w /src
)
if [[ "$ENGINE" == "podman" ]]; then
  RUN+=(--userns=keep-id --security-opt label=disable)
else
  RUN+=(--user "$(id -u):$(id -g)")
fi

echo "==> Running in container"
"$ENGINE" run "${RUN[@]}" "$IMAGE" bash -s <<'EOS'
set -euo pipefail
mkdir -p "$HOME"
export PATH="/usr/bin:$PATH"

install_qt() {
  if [[ -f "$QT_WINDOWS_PREFIX/lib/libQt6Core.a" && -f "$QT_WINDOWS_PREFIX/plugins/sqldrivers/libqsqlite.a" && -f "$QT_WINDOWS_PREFIX/plugins/platforms/libqwindows.a" ]]; then
    echo "==> Windows static Qt already at $QT_WINDOWS_PREFIX"
    return
  fi
  src="$QT_ROOT/src/qtbase-everywhere-src-$QT_VERSION"
  tarball="$QT_ROOT/src/qtbase-everywhere-src-$QT_VERSION.tar.xz"
  url="https://download.qt.io/official_releases/qt/${QT_VERSION%.*}/$QT_VERSION/submodules/qtbase-everywhere-src-$QT_VERSION.tar.xz"
  if [[ ! -d "$src" ]]; then
    if [[ ! -f "$tarball" ]]; then
      echo "==> Downloading $url"
      curl -fL --retry 3 -o "$tarball.partial" "$url"
      mv "$tarball.partial" "$tarball"
    fi
    tar -C "$QT_ROOT/src" -xf "$tarball"
  fi
  build="$QT_ROOT/build/qtbase-static-$QT_VERSION-mingw"
  rm -rf "$build"
  mkdir -p "$build"
  echo "==> Configuring static Qt for MinGW"
  cmake -G Ninja -S "$src" -B "$build" \
    -DCMAKE_SYSTEM_NAME=Windows \
    -DCMAKE_SYSTEM_PROCESSOR=x86_64 \
    -DCMAKE_C_COMPILER=x86_64-w64-mingw32-gcc \
    -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-g++ \
    -DCMAKE_RC_COMPILER=x86_64-w64-mingw32-windres \
    -DCMAKE_FIND_ROOT_PATH=/usr/x86_64-w64-mingw32 \
    -DCMAKE_FIND_ROOT_PATH_MODE_PROGRAM=NEVER \
    -DCMAKE_FIND_ROOT_PATH_MODE_LIBRARY=ONLY \
    -DCMAKE_FIND_ROOT_PATH_MODE_INCLUDE=ONLY \
    -DCMAKE_INSTALL_PREFIX="$QT_WINDOWS_PREFIX" \
    -DCMAKE_BUILD_TYPE=Release \
    -DQT_HOST_PATH="$QT_HOST_PATH" \
    -DQT_QMAKE_TARGET_MKSPEC=win32-g++ \
    -DBUILD_SHARED_LIBS=OFF \
    -DQT_BUILD_EXAMPLES=OFF \
    -DQT_BUILD_TESTS=OFF \
    -DFEATURE_widgets=ON \
    -DFEATURE_sql=ON \
    -DFEATURE_sql_sqlite=ON \
    -DFEATURE_system_sqlite=OFF \
    -DFEATURE_testlib=OFF \
    -DFEATURE_network=ON \
    -DFEATURE_dbus=OFF \
    -DFEATURE_icu=OFF \
    -DFEATURE_openssl=OFF \
    -DFEATURE_schannel=OFF \
    -DFEATURE_xcb=OFF \
    -DFEATURE_fontconfig=OFF \
    -DFEATURE_system_zlib=OFF \
    -DFEATURE_system_pcre2=OFF \
    -DFEATURE_system_harfbuzz=OFF \
    -DFEATURE_system_freetype=OFF \
    -DFEATURE_system_png=OFF \
    -DFEATURE_system_jpeg=OFF \
    -DFEATURE_opengl=ON
  cmake --build "$build" --parallel "$JOBS"
  cmake --install "$build"
}

case "${ACTION}" in
  install-qt|all) install_qt ;;
esac
case "${ACTION}" in
  build|all)
    make static-windows \
      QT_WINDOWS_PREFIX="$QT_WINDOWS_PREFIX" \
      MINGW_CXX=x86_64-w64-mingw32-g++ \
      MINGW_NM=x86_64-w64-mingw32-nm
    ;;
esac
EOS

echo "==> Done"
echo "    static Qt: $QT_ROOT/$PREFIX_REL"
echo "    binary   : $ROOT/build/ergotime.exe"
