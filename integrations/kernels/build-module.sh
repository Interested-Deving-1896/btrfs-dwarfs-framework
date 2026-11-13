#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the BDFS out-of-tree kernel module against an already configured
# kernel source/build tree. This deliberately does not install or load it.
#
# Usage:
#   integrations/kernels/build-module.sh /path/to/kernel/build [artifact-dir]
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MODULE_DIR="${REPO_ROOT}/kernel/btrfs_dwarfs"
KDIR="${1:-}"
ARTIFACT_DIR="${2:-${REPO_ROOT}/build/kernel-module-artifacts}"

if [[ -z "${KDIR}" || ! -d "${KDIR}" ]]; then
  echo "Usage: $0 KERNEL_BUILD_DIR [ARTIFACT_DIR]" >&2
  exit 2
fi

if [[ ! -f "${KDIR}/Makefile" || ! -f "${KDIR}/.config" ]]; then
  echo "ERROR: KERNEL_BUILD_DIR must contain a Makefile and a configured .config." >&2
  exit 2
fi

for symbol in CONFIG_BTRFS_FS CONFIG_FUSE_FS CONFIG_NET; do
  if ! grep -Eq "^${symbol}=[ym]$" "${KDIR}/.config"; then
    echo "ERROR: ${symbol} must be enabled (=y or =m) in ${KDIR}/.config." >&2
    exit 1
  fi
done

if [[ ! -f "${MODULE_DIR}/Kbuild" ]]; then
  echo "ERROR: BDFS Kbuild file not found at ${MODULE_DIR}/Kbuild." >&2
  exit 2
fi

echo "==> Building BDFS external module"
echo "    Kernel tree: ${KDIR}"
echo "    Module tree: ${MODULE_DIR}"

make -C "${KDIR}" M="${MODULE_DIR}" CONFIG_BTRFS_DWARFS=m modules

MODULE_FILE="${MODULE_DIR}/btrfs_dwarfs.ko"
if [[ ! -f "${MODULE_FILE}" ]]; then
  echo "ERROR: Build completed without producing ${MODULE_FILE}." >&2
  exit 1
fi

mkdir -p "${ARTIFACT_DIR}"
install -m 0644 "${MODULE_FILE}" "${ARTIFACT_DIR}/btrfs_dwarfs.ko"
if command -v modinfo >/dev/null 2>&1; then
  modinfo "${ARTIFACT_DIR}/btrfs_dwarfs.ko"
fi

echo "==> BDFS module build succeeded: ${ARTIFACT_DIR}/btrfs_dwarfs.ko"
echo "NOTE: This builds only; it does not install, load, or make BDFS the root filesystem."
