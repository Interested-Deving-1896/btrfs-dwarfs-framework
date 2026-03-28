# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(btrfs_dwarfs_userspace
    VERSION 1.0.0
    DESCRIPTION "BTRFS+DwarFS hybrid filesystem userspace tools"
    LANGUAGES C
)

# ── Build options ────────────────────────────────────────────────────────────
option(BUILD_DAEMON  "Build the bdfs_daemon"  ON)
option(BUILD_CLI     "Build the bdfs CLI"     ON)
option(BUILD_TESTS   "Build unit tests"       OFF)
option(ENABLE_ASAN   "Enable AddressSanitizer" OFF)

# ── Compiler flags ───────────────────────────────────────────────────────────
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_options(
    -Wall -Wextra -Wpedantic
    -Wformat=2 -Wformat-security
    -Wno-unused-parameter
    -D_GNU_SOURCE
    -D_FILE_OFFSET_BITS=64
)

if(ENABLE_ASAN)
    add_compile_options(-fsanitize=address,undefined)
    add_link_options(-fsanitize=address,undefined)
endif()

# ── Include paths ────────────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/../include
    ${CMAKE_SOURCE_DIR}/../include/uapi
)

# ── bdfs_daemon ──────────────────────────────────────────────────────────────
if(BUILD_DAEMON)
    add_executable(bdfs_daemon
        daemon/bdfs_daemon.c
        daemon/bdfs_exec.c
        daemon/bdfs_jobs.c
        daemon/bdfs_netlink.c
        daemon/bdfs_socket.c
    )

    target_include_directories(bdfs_daemon PRIVATE
        ${CMAKE_SOURCE_DIR}/daemon
    )

    target_link_libraries(bdfs_daemon PRIVATE pthread)

    install(TARGETS bdfs_daemon
        RUNTIME DESTINATION sbin
    )

    # systemd service unit
    install(FILES
        ${CMAKE_SOURCE_DIR}/../configs/bdfs_daemon.service
        DESTINATION lib/systemd/system
        OPTIONAL
    )
endif()

# ── bdfs CLI ─────────────────────────────────────────────────────────────────
if(BUILD_CLI)
    add_executable(bdfs
        cli/bdfs_main.c
        cli/bdfs_partition.c
        cli/bdfs_export_import.c
        cli/bdfs_mount.c
        cli/bdfs_snapshot_promote_demote.c
        cli/bdfs_status.c
    )

    target_include_directories(bdfs PRIVATE
        ${CMAKE_SOURCE_DIR}/cli
    )

    install(TARGETS bdfs
        RUNTIME DESTINATION bin
    )

    # Man pages
    install(FILES
        ${CMAKE_SOURCE_DIR}/../doc/bdfs.1
        DESTINATION share/man/man1
        OPTIONAL
    )
    install(FILES
        ${CMAKE_SOURCE_DIR}/../doc/bdfs_daemon.8
        DESTINATION share/man/man8
        OPTIONAL
    )
endif()

# ── Unit tests ───────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(../tests/unit unit_tests)
endif()

# ── Summary ──────────────────────────────────────────────────────────────────
message(STATUS "")
message(STATUS "BTRFS+DwarFS userspace build configuration:")
message(STATUS "  Build daemon : ${BUILD_DAEMON}")
message(STATUS "  Build CLI    : ${BUILD_CLI}")
message(STATUS "  Build tests  : ${BUILD_TESTS}")
message(STATUS "  ASAN         : ${ENABLE_ASAN}")
message(STATUS "  Install prefix: ${CMAKE_INSTALL_PREFIX}")
message(STATUS "")
