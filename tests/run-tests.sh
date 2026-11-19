#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Build and run the unprivileged C unit-test suite.
set -eu

ROOT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD_DIR=${BDFS_BUILD_DIR:-"$ROOT_DIR/build/unit-tests"}

cmake -S "$ROOT_DIR/userspace" -B "$BUILD_DIR" \
    -DBUILD_DAEMON=OFF \
    -DBUILD_CLI=OFF \
    -DBUILD_TESTS=ON
cmake --build "$BUILD_DIR" --parallel
ctest --test-dir "$BUILD_DIR" --output-on-failure

python3 -m unittest discover -s "$ROOT_DIR/tests/python" -v

INSTALL_DIR=${BDFS_INSTALL_DIR:-"$BUILD_DIR/stage"}
cmake --install "$BUILD_DIR" --prefix "$INSTALL_DIR"
test -s "$INSTALL_DIR/include/bdfs/target.h"
test -s "$INSTALL_DIR/lib/libbdfs_target.a"
