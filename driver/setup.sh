#!/bin/bash
#
# Build libfprint with the omsmoc driver (out-of-tree development setup).
#
# The driver is developed as a set of files inside this repository and hooked
# into a pristine libfprint source tree by this script, so that the same files
# can later be submitted upstream unchanged.
#
# Usage:
#   driver/setup.sh [options]
#
# Options:
#   -v, --version VER   libfprint version to fetch (default: 1.94.100)
#   -b, --build-dir DIR build directory (default: <repo>/build)
#   -t, --test          run the umockdev regression test after building
#   -c, --clean         remove the extracted source and build directory first
#   -h, --help          show this help
#
# Everything is fetched into the (git ignored) build directory; the repository
# itself only ever contains the driver sources, the tests and the patch.

set -euo pipefail

VERSION=1.94.100
REPO_ROOT=$(cd "$(dirname "$0")/.." && pwd)
BUILD_ROOT="$REPO_ROOT/build"
RUN_TEST=0

usage() {
    sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

while [ $# -gt 0 ]; do
    case "$1" in
        -v|--version) VERSION="$2"; shift 2 ;;
        -b|--build-dir) BUILD_ROOT="$2"; shift 2 ;;
        -t|--test) RUN_TEST=1; shift ;;
        -c|--clean) rm -rf "$BUILD_ROOT"; shift ;;
        -h|--help) usage ;;
        *) echo "Unknown option: $1" >&2; usage ;;
    esac
done

SRC_DIR="$BUILD_ROOT/libfprint-$VERSION"
BUILD_DIR="$BUILD_ROOT/libfprint-build"
TARBALL="$BUILD_ROOT/libfprint-$VERSION.tar.gz"
PATCH_FILE="$REPO_ROOT/driver/libfprint-oms.patch"

echo "==> libfprint $VERSION in $SRC_DIR"

mkdir -p "$BUILD_ROOT"

if [ ! -d "$SRC_DIR" ]; then
    if [ ! -f "$TARBALL" ]; then
        echo "==> Downloading libfprint $VERSION"
        curl -L --fail -o "$TARBALL" \
            "https://gitlab.freedesktop.org/libfprint/libfprint/-/archive/v$VERSION/libfprint-v$VERSION.tar.gz"
    fi
    tar xzf "$TARBALL" -C "$BUILD_ROOT"
    mv "$BUILD_ROOT/libfprint-v$VERSION" "$SRC_DIR"
fi

echo "==> Applying driver patch"
if patch -d "$SRC_DIR" -p1 --dry-run --reverse --silent < "$PATCH_FILE" 2>/dev/null; then
    echo "    already applied"
else
    patch -d "$SRC_DIR" -p1 < "$PATCH_FILE"
fi

echo "==> Linking driver sources into the libfprint tree"
ln -sf "$REPO_ROOT/driver/omsmoc.c" "$SRC_DIR/libfprint/drivers/omsmoc.c"
ln -sf "$REPO_ROOT/driver/omsmoc.h" "$SRC_DIR/libfprint/drivers/omsmoc.h"
ln -sf "$REPO_ROOT/driver/tools/oms-smoke.c" "$SRC_DIR/examples/oms-smoke.c"
ln -sfn "$REPO_ROOT/driver/tests/omsmoc" "$SRC_DIR/tests/omsmoc"

echo "==> Configuring"
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    meson setup "$BUILD_DIR" "$SRC_DIR" \
        -Ddrivers=default \
        -Dintrospection=true \
        -Ddoc=false \
        -Dgtk-examples=false \
        -Dinstalled-tests=false \
        -Dudev_rules=disabled \
        -Dudev_hwdb=disabled
fi

echo "==> Building"
ninja -C "$BUILD_DIR"

if [ "$RUN_TEST" = 1 ]; then
    echo "==> Running the umockdev regression test (no hardware needed)"
    meson test -C "$BUILD_DIR" omsmoc --print-errorlogs
fi

cat <<EOF

Done.  Useful commands:

  # build
  ninja -C $BUILD_DIR

  # hardware bring-up tool (uses the built libfprint)
  $BUILD_DIR/examples/oms-smoke info
  $BUILD_DIR/examples/oms-smoke verify 30      # press an enrolled finger
  $BUILD_DIR/examples/oms-smoke enroll 120     # press/lift the finger 6 times
  $BUILD_DIR/examples/oms-smoke list
  $BUILD_DIR/examples/oms-smoke delete <slot>  # deletes one chip template

  # no-hardware regression test
  meson test -C $BUILD_DIR omsmoc

  # fprintd against this build (stop the system daemon first)
  sudo env LD_LIBRARY_PATH=$BUILD_DIR/libfprint /usr/lib/fprintd --no-timeout
  fprintd-enroll && fprintd-verify

EOF
