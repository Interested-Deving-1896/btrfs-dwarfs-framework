#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the daemon, CLI and unit tests; smoke-test non-privileged CLI entrypoints.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${TMPDIR:-/tmp}/bdfs-unit-tests-${GITHUB_RUN_ID:-local}}"

cmake -S "${repo_root}/userspace" -B "${build_dir}" \
    -DBUILD_DAEMON=ON \
    -DBUILD_CLI=ON \
    -DBUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure

"${build_dir}/bdfs" --help >/dev/null
"${build_dir}/bdfs" --version
