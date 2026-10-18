#!/usr/bin/env bash
# Run the repository's userspace C unit-test suite.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${BUILD_DIR:-${repo_root}/build/unit-tests}"

command -v cmake >/dev/null 2>&1 || {
  echo "ERROR: cmake is required to build unit tests" >&2
  exit 1
}
command -v ctest >/dev/null 2>&1 || {
  echo "ERROR: ctest is required to run unit tests" >&2
  exit 1
}

cmake -S "${repo_root}/tests/unit" -B "${build_dir}" -DCMAKE_BUILD_TYPE=Debug
cmake --build "${build_dir}" --parallel 2
ctest --test-dir "${build_dir}" --output-on-failure
