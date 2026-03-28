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

# ── Kernel header feature detection ─────────────────────────────────────────
# btrfs_qgroup_info_item.exclusive was renamed to .excl in kernel 5.15+.
# Detect which name is present so bdfs_policy.c compiles against both.
include(CheckStructHasMember)
check_struct_has_member(
    "struct btrfs_qgroup_info_item" excl
    "linux/btrfs_tree.h"
    BDFS_BTRFS_QGROUP_HAS_EXCL
    LANGUAGE C
)
if(BDFS_BTRFS_QGROUP_HAS_EXCL)
    add_compile_definitions(BDFS_BTRFS_QGROUP_HAS_EXCL)
    message(STATUS "btrfs_qgroup_info_item: using field 'excl' (kernel >= 5.15)")
else()
    message(STATUS "btrfs_qgroup_info_item: using field 'exclusive' (kernel < 5.15)")
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
        daemon/bdfs_policy.c
    )

    target_include_directories(bdfs_daemon PRIVATE
        ${CMAKE_SOURCE_DIR}/daemon
    )

    target_link_libraries(bdfs_daemon PRIVATE pthread)

    install(TARGETS bdfs_daemon
        RUNTIME DESTINATION sbin
    )

    # Generate the systemd service unit with the correct sbin path substituted.
    # configure_file replaces @CMAKE_INSTALL_FULL_SBINDIR@ at cmake-configure
    # time so the installed unit always points at the real binary location.
    set(BDFS_DAEMON_BIN "${CMAKE_INSTALL_PREFIX}/sbin/bdfs_daemon")
    configure_file(
        ${CMAKE_SOURCE_DIR}/../configs/bdfs_daemon.service.in
        ${CMAKE_CURRENT_BINARY_DIR}/bdfs_daemon.service
        @ONLY
    )
    install(FILES
        ${CMAKE_CURRENT_BINARY_DIR}/bdfs_daemon.service
        DESTINATION lib/systemd/system
        OPTIONAL
    )

    # Default daemon config
    install(FILES
        ${CMAKE_SOURCE_DIR}/../configs/bdfs.conf
        DESTINATION etc/bdfs
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
        cli/bdfs_verify.c
        cli/bdfs_policy.c
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

    # Boot image update script
    install(PROGRAMS
        ${CMAKE_SOURCE_DIR}/../boot/bdfs-image-update
        DESTINATION sbin
        OPTIONAL
    )

    # Boot systemd units
    install(FILES
        ${CMAKE_SOURCE_DIR}/../boot/systemd-generator/bdfs-image-update.service
        ${CMAKE_SOURCE_DIR}/../boot/systemd-generator/bdfs-image-update.timer
        DESTINATION lib/systemd/system
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
