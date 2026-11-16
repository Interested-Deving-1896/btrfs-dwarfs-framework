#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the userspace daemon unit-test suite through the supported CMake build.
# The CLI target is intentionally disabled until its source graph is repaired;
# daemon compilation remains enabled so daemon regressions fail this job.

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${TMPDIR:-/tmp}/bdfs-unit-tests-${GITHUB_RUN_ID:-local}}"

cmake -S "${repo_root}/userspace" -B "${build_dir}" \
    -DBUILD_DAEMON=ON \
    -DBUILD_CLI=OFF \
    -DBUILD_TESTS=ON \
    -DCMAKE_BUILD_TYPE=Debug

cmake --build "${build_dir}" --parallel
ctest --test-dir "${build_dir}" --output-on-failure
