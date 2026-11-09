#!/usr/bin/env bash
# Build Qt 6.12 qtbase as static libraries for this C++ client.
#
# Official desktop kits are shared libraries. The binary produced by `make static`
# still links the system X11, OpenGL, font, D-Bus, and OpenSSL libraries.
#
#   ./scripts/install-qt-static.sh
#   make static
#
# The host needs cmake, a C++ compiler, and X11/GL devel packages. Without those:
#   make static-container
#
# Install prefix: ${QT_STATIC_PREFIX:-$QT_ROOT/static/$QT_VERSION}

set -euo pipefail

QT_VERSION="${QT_VERSION:-6.12.0}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
PREFIX="${QT_STATIC_PREFIX:-$QT_ROOT/static/$QT_VERSION}"
SRC="${QT_SRC:-$QT_ROOT/src/qtbase-everywhere-src-$QT_VERSION}"
BUILD="${QT_BUILD:-$QT_ROOT/build/qtbase-static-$QT_VERSION}"
if [[ -z "${JOBS:-}" ]]; then
  if command -v nproc >/dev/null 2>&1; then
    JOBS="$(nproc)"
  elif command -v sysctl >/dev/null 2>&1; then
    JOBS="$(sysctl -n hw.ncpu)"
  else
    JOBS=4
  fi
fi
TARBALL_URL="${QT_TARBALL_URL:-https://download.qt.io/official_releases/qt/${QT_VERSION%.*}/$QT_VERSION/submodules/qtbase-everywhere-src-$QT_VERSION.tar.xz}"
HOST_OS="$(uname -s)"

echo "==> Qt static build from source"
echo "    version : $QT_VERSION"
echo "    prefix  : $PREFIX"
echo "    jobs    : $JOBS"

has_static_core() {
  [[ -f "$PREFIX/lib/libQt6Core.a" || -f "$PREFIX/lib64/libQt6Core.a" ]]
}

has_sqlite() {
  [[ -f "$PREFIX/plugins/sqldrivers/libqsqlite.a" ]]
}

has_wayland() {
  [[ -f "$PREFIX/plugins/platforms/libqwayland.a" && -f "$PREFIX/plugins/wayland-shell-integration/libxdg-shell.a" ]]
}

platform_ready() {
  case "$HOST_OS" in
    Darwin)
      [[ -f "$PREFIX/plugins/platforms/libqcocoa.a" ]]
      ;;
    *)
      has_wayland && [[ -f "$PREFIX/plugins/platforms/libqxcb.a" || -f "$PREFIX/plugins/platforms/libqwindows.a" ]]
      ;;
  esac
}

if has_static_core && has_sqlite && platform_ready; then
  echo "==> Already installed at $PREFIX"
  exit 0
fi

need() {
  command -v "$1" >/dev/null 2>&1 || {
    echo "error: missing tool: $1" >&2
    if [[ "$HOST_OS" != "Darwin" ]]; then
      echo "hint: make static-container   # podman image with cmake and X11 devel" >&2
    fi
    exit 1
  }
}
need cmake
if [[ "$HOST_OS" == "Darwin" ]]; then
  command -v clang++ >/dev/null 2>&1 || command -v g++ >/dev/null 2>&1 || {
    echo "error: missing a C++ compiler (clang++ or g++)" >&2
    exit 1
  }
else
  need g++
fi
need python3
need tar
need xz
if command -v ninja >/dev/null 2>&1; then
  GEN=Ninja
else
  GEN="Unix Makefiles"
fi

if [[ "$HOST_OS" != "Darwin" && ! -f /usr/include/xcb/xcb_cursor.h ]]; then
  echo "error: missing xcb/xcb_cursor.h (needed for the Qt XCB platform plugin)" >&2
  echo "       Fedora/RHEL: sudo dnf install xcb-util-cursor-devel" >&2
  echo "       Or: make static-container" >&2
  exit 1
fi

mkdir -p "$QT_ROOT/src" "$QT_ROOT/build" "$(dirname "$PREFIX")"

if [[ ! -d "$SRC" ]]; then
  tarball="$QT_ROOT/src/qtbase-everywhere-src-$QT_VERSION.tar.xz"
  if [[ ! -f "$tarball" ]]; then
    echo "==> Downloading $TARBALL_URL"
    curl -fL --retry 3 -o "$tarball.partial" "$TARBALL_URL"
    mv "$tarball.partial" "$tarball"
  fi
  echo "==> Extracting $tarball"
  tar -C "$QT_ROOT/src" -xf "$tarball"
fi

if [[ ! -d "$SRC" ]]; then
  echo "error: source dir not found: $SRC" >&2
  exit 1
fi

if [[ -f "$BUILD/CMakeCache.txt" ]]; then
  echo "==> Reusing build directory $BUILD"
else
  rm -rf "$BUILD"
  mkdir -p "$BUILD"
fi

echo "==> Configuring static Qt (qtbase)"
CMAKE_ARGS=(
  -G "$GEN"
  -S "$SRC"
  -B "$BUILD"
  -DCMAKE_INSTALL_PREFIX="$PREFIX"
  -DCMAKE_BUILD_TYPE=Release
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON
  -DBUILD_SHARED_LIBS=OFF
  -DQT_BUILD_EXAMPLES=OFF
  -DQT_BUILD_TESTS=OFF
  -DFEATURE_widgets=ON
  -DFEATURE_sql=ON
  -DFEATURE_sql_sqlite=ON
  -DFEATURE_system_sqlite=OFF
  -DFEATURE_testlib=OFF
  -DFEATURE_network=ON
  -DFEATURE_icu=OFF
  -DFEATURE_framework=OFF
)
if [[ "$HOST_OS" == "Darwin" ]]; then
  # Cocoa + SecureTransport. No XCB, Wayland, D-Bus, or OpenSSL.
  CMAKE_ARGS+=(
    -DFEATURE_dbus=OFF
    -DFEATURE_openssl=OFF
    -DFEATURE_securetransport=ON
    -DFEATURE_xcb=OFF
    -DFEATURE_wayland=OFF
    -DFEATURE_fontconfig=OFF
    -DINPUT_opengl=desktop
  )
else
  CMAKE_ARGS+=(
    -DFEATURE_dbus=ON
    -DFEATURE_openssl=ON
    -DFEATURE_opengl=ON
    -DFEATURE_xcb=ON
    -DFEATURE_wayland=ON
    -DFEATURE_waylandscanner=ON
    -DFEATURE_qtwaylandscanner=ON
    -DFEATURE_wayland_client=ON
    -DFEATURE_wayland_egl=ON
    -DFEATURE_wayland_client_xdg_shell=ON
    -DINPUT_opengl=desktop
  )
fi
cmake "${CMAKE_ARGS[@]}"
cd "$BUILD"

echo "==> Building (this takes a while); log: $BUILD/build.log"
cmake --build . --parallel "$JOBS" >"$BUILD/build.log" 2>&1 || {
  echo "error: Qt build failed. Last lines of $BUILD/build.log:" >&2
  tail -40 "$BUILD/build.log" >&2
  exit 1
}
echo "==> Installing to $PREFIX"
cmake --install . >"$BUILD/install.log" 2>&1 || {
  echo "error: Qt install failed. Last lines of $BUILD/install.log:" >&2
  tail -40 "$BUILD/install.log" >&2
  exit 1
}

if [[ -d "$PREFIX/lib64" && ! -d "$PREFIX/lib" ]]; then
  ln -sfn lib64 "$PREFIX/lib"
fi

if ! has_static_core || ! has_sqlite; then
  echo "error: static install at $PREFIX is missing libQt6Core.a or libqsqlite.a" >&2
  exit 1
fi
if ! platform_ready; then
  echo "error: platform plugin missing after install ($HOST_OS)" >&2
  exit 1
fi

echo "==> Static Qt installed at $PREFIX"
echo "Next: make static"
