# SPDX-License-Identifier: GPL-2.0-or-later
#
# Top-level Makefile for the BTRFS+DwarFS framework.
#
# Targets:
#   all              Build kernel module + userspace (daemon + CLI)
#   kernel           Build only the kernel module
#   userspace        Build only the userspace tools
#   install          Install everything (requires root for kernel module)
#   clean            Remove all build artefacts
#   test             Run integration tests
#   check            Run unit tests (requires cmake -DBUILD_TESTS=ON)
#   fmt              Format C sources with clang-format
#
# Variables:
#   KDIR             Kernel source tree (default: running kernel)
#   PREFIX           Install prefix for userspace (default: /usr/local)
#   BUILD_DIR        CMake build directory (default: build/userspace)

KDIR      ?= /lib/modules/$(shell uname -r)/build
PREFIX    ?= /usr/local
BUILD_DIR ?= build/userspace

.PHONY: all kernel userspace install install-kernel install-userspace \
        clean clean-kernel clean-userspace test check fmt

all: kernel userspace

# ── Kernel module ────────────────────────────────────────────────────────────

kernel:
	$(MAKE) -C kernel KDIR=$(KDIR)

clean-kernel:
	$(MAKE) -C kernel clean

install-kernel: kernel
	$(MAKE) -C kernel install

# ── Userspace ────────────────────────────────────────────────────────────────

$(BUILD_DIR)/Makefile:
	cmake -S userspace -B $(BUILD_DIR) \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX=$(PREFIX) \
		-DBUILD_DAEMON=ON \
		-DBUILD_CLI=ON

userspace: $(BUILD_DIR)/Makefile
	cmake --build $(BUILD_DIR) --parallel

clean-userspace:
	rm -rf $(BUILD_DIR)

install-userspace: userspace
	cmake --install $(BUILD_DIR)

# ── Combined ─────────────────────────────────────────────────────────────────

install: install-kernel install-userspace

clean: clean-kernel clean-userspace

# ── Tests ────────────────────────────────────────────────────────────────────

test: userspace
	@echo "Running integration tests..."
	bash tests/integration/run_all.sh

check:
	cmake -S userspace -B build/tests \
		-DCMAKE_BUILD_TYPE=Debug \
		-DBUILD_TESTS=ON \
		-DENABLE_ASAN=ON
	cmake --build build/tests --parallel
	cd build/tests && ctest --output-on-failure

# ── Formatting ───────────────────────────────────────────────────────────────

fmt:
	find kernel userspace include -name '*.c' -o -name '*.h' | \
		xargs clang-format -i --style=file 2>/dev/null || \
		find kernel userspace include -name '*.c' -o -name '*.h' | \
		xargs clang-format -i --style="{BasedOnStyle: Linux, IndentWidth: 8}"
