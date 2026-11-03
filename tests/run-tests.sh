#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Build and run the rootless userspace unit-test suite.
set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${BDFS_TEST_BUILD_DIR:-${TMPDIR:-/tmp}/bdfs-userspace-tests}"

command -v cmake >/dev/null 2>&1 || { echo "error: cmake is required" >&2; exit 127; }
command -v ctest >/dev/null 2>&1 || { echo "error: ctest is required" >&2; exit 127; }

cmake -S "${REPO_ROOT}/userspace" -B "${BUILD_DIR}" \
    -DBUILD_DAEMON=OFF \
    -DBUILD_CLI=OFF \
    -DBUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build "${BUILD_DIR}" --parallel "${BDFS_BUILD_JOBS:-2}"
ctest --test-dir "${BUILD_DIR}" --output-on-failure
