#!/usr/bin/env bash
# Build static Qt 6.12 and/or the static ErgoTime binary inside podman or docker.
# The host does not need cmake or X11 development packages.
#
#   ./scripts/container-build-static.sh            # Qt + build/ergotime-static
#   ./scripts/container-build-static.sh install-qt
#   ./scripts/container-build-static.sh build
#
# CONTAINER_ENGINE=podman|docker   (podman is preferred)
# QT_VERSION=6.12.0  QT_ROOT=$HOME/Qt  JOBS=N

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ACTION="${1:-all}"
QT_VERSION="${QT_VERSION:-6.12.0}"
QT_ROOT="${QT_ROOT:-$HOME/Qt}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"
IMAGE="${IMAGE:-localhost/ergotime-cpp-static:${QT_VERSION}}"
DOCKERFILE="$ROOT/docker/Dockerfile.static"

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

echo "==> container static build"
echo "    engine : $ENGINE"
echo "    action : $ACTION"
echo "    Qt     : $QT_VERSION"
echo "    prefix : $QT_ROOT/static/$QT_VERSION"
echo "    jobs   : $JOBS"

mkdir -p "$QT_ROOT/src" "$QT_ROOT/build" "$QT_ROOT/static" "$ROOT/build"

echo "==> Building toolchain image"
"$ENGINE" build -q -f "$DOCKERFILE" -t "$IMAGE" "$ROOT/docker"

RUN=(
  --rm
  -i
  -e "QT_VERSION=$QT_VERSION"
  -e "QT_ROOT=/qt"
  -e "QT_STATIC_PREFIX=/qt/static/$QT_VERSION"
  -e "JOBS=$JOBS"
  -e "HOME=/tmp/home"
  -e "ACTION=$ACTION"
  -v "$ROOT:/src:Z"
  -v "$QT_ROOT:/qt:Z"
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
case "${ACTION}" in
  install-qt|all) ./scripts/install-qt-static.sh ;;
esac
case "${ACTION}" in
  build|all) make static ;;
esac
EOS

echo "==> Done"
echo "    static Qt: $QT_ROOT/static/$QT_VERSION"
echo "    binary   : $ROOT/build/ergotime-static"
