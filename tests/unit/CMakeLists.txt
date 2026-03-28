# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)

# Unit tests for userspace components.
# These do not require root or real block devices.

enable_testing()

include_directories(
    ${CMAKE_SOURCE_DIR}/../../include
    ${CMAKE_SOURCE_DIR}/../../include/uapi
    ${CMAKE_SOURCE_DIR}/../../userspace/cli
    ${CMAKE_SOURCE_DIR}/../../userspace/daemon
)

# ── test_uuid ────────────────────────────────────────────────────────────────
add_executable(test_uuid test_uuid.c
    ../../userspace/cli/bdfs_main.c)
target_compile_definitions(test_uuid PRIVATE BDFS_UNIT_TEST)
add_test(NAME uuid COMMAND test_uuid)

# ── test_compression_names ───────────────────────────────────────────────────
add_executable(test_compression test_compression.c
    ../../userspace/cli/bdfs_main.c)
target_compile_definitions(test_compression PRIVATE BDFS_UNIT_TEST)
add_test(NAME compression COMMAND test_compression)

# ── test_job_alloc ───────────────────────────────────────────────────────────
add_executable(test_job_alloc test_job_alloc.c
    ../../userspace/daemon/bdfs_daemon.c)
target_compile_definitions(test_job_alloc PRIVATE BDFS_UNIT_TEST)
target_link_libraries(test_job_alloc PRIVATE pthread)
add_test(NAME job_alloc COMMAND test_job_alloc)
