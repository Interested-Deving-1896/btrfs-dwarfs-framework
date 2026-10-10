#!/usr/bin/env bash
# integrations/lib/bdfs-incus.sh
#
# Shared Incus dispatch library for btrfs-dwarfs-framework.
# Replaces Docker/Podman as the container/VM runtime backend.
#
# Sourced by bdfs-bootc.sh, bdfs-bootc-image-builder.sh,
# bdfs-devcontainer.sh, bdfs-incusos.sh, and bdfs-incus-runtime.sh.
# Never executed directly.
#
# ── OCI image policy ──────────────────────────────────────────────────────────
#
# Ephemeral tool runs (--ephemeral flag):
#   incus launch oci:<image> <name> --ephemeral
#   Instance is deleted automatically on stop. No local image cache.
#
# Persistent / workspace base images (default):
#   1. Check local Incus image store for alias
#   2. If missing: incus image copy oci:<image> local: --alias <alias>
#   3. incus launch local:<alias> <name>
#   Cached images are stable, versionable, and reused across launches.
#
# ── Functions ─────────────────────────────────────────────────────────────────
#
# _bdfs_incus_require          Verify incus is installed and daemon is running
# _bdfs_incus_image_ensure     Pull OCI image to local store if not cached
# _bdfs_incus_launch           Launch container or VM (ephemeral or persistent)
# _bdfs_incus_exec             Run a command inside a running instance
# _bdfs_incus_stop             Stop an instance (graceful then force)
# _bdfs_incus_delete           Delete an instance
# _bdfs_incus_publish          Publish an instance/snapshot as a local image
# _bdfs_incus_image_export     Export a local image to a tarball
# _bdfs_incus_image_import     Import a tarball as a local image
# _bdfs_incus_image_copy_out   Copy a local image to a remote registry (OCI)
# _bdfs_incus_storage_import   Import a file into an Incus storage volume
# _bdfs_incus_info             Print instance info as JSON
# _bdfs_incus_rootfs_path      Return the host path to an instance's rootfs
# _bdfs_incus_wait_ready       Wait until an instance is in Running state
# _bdfs_incus_oci_remote_add   Add an OCI registry as an Incus remote (idempotent)

[[ -n "${_BDFS_INCUS_LOADED:-}" ]] && return 0
_BDFS_INCUS_LOADED=1

INCUS_CMD="${INCUS_CMD:-incus}"

# Default OCI remote name registered in Incus for Docker Hub
BDFS_INCUS_OCI_REMOTE="${BDFS_INCUS_OCI_REMOTE:-oci-docker}"
# Default storage pool
BDFS_INCUS_POOL="${BDFS_INCUS_POOL:-default}"
# Default instance type for tool runs
BDFS_INCUS_TOOL_TYPE="${BDFS_INCUS_TOOL_TYPE:-container}"

_bdfs_incus_info_log() { echo "[bdfs-incus] $*"; }
_bdfs_incus_warn()     { echo "[bdfs-incus] WARN: $*" >&2; }
_bdfs_incus_die()      { echo "[bdfs-incus] ERROR: $*" >&2; exit 1; }

