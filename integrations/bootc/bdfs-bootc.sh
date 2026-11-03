#!/usr/bin/env bash
# bdfs-bootc — bootc integration for btrfs-dwarfs-framework
#
# Bridges bootc's OCI-container-as-OS model with bdfs's snapshot and
# workspace capabilities. Uses Incus as the container/image runtime backend
# instead of Docker or Podman.
#
# Subcommands:
#   workspace   Create a bdfs workspace from the active bootc image root
#   commit      Pack a bdfs workspace into an Incus image and optionally push
#   switch      Switch the booted image (wraps bootc switch)
#   upgrade     Upgrade in place (wraps bootc upgrade)
#   export      Export the active bootc root as a DwarFS image
#   status      Show bootc status and any active bdfs workspaces
#
# Environment:
#   BDFS_BOOTC_IMAGE     Default OCI image reference
#   BDFS_BOOTC_REGISTRY  Default registry prefix for push operations
#   INCUS_CMD            Incus CLI binary (default: incus)
#
# Dependencies: bootc, bdfs, incus
#
# Usage:
#   bdfs-bootc.sh workspace [--source PATH] [--name NAME]
#   bdfs-bootc.sh commit    <workspace-name> --image IMAGE [--push]
#   bdfs-bootc.sh switch    --image IMAGE
#   bdfs-bootc.sh upgrade   [--check]
#   bdfs-bootc.sh export    [--source PATH] --out PATH [--compression zstd]
#   bdfs-bootc.sh status

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/bdfs-incus.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
BOOTC_CMD="${BOOTC_CMD:-bootc}"
BDFS_BOOTC_IMAGE="${BDFS_BOOTC_IMAGE:-}"
BDFS_BOOTC_REGISTRY="${BDFS_BOOTC_REGISTRY:-}"

info() { echo "[bdfs-bootc] $*"; }
warn() { echo "[bdfs-bootc] WARN: $*" >&2; }
die()  { echo "[bdfs-bootc] ERROR: $*" >&2; exit 1; }

require_bdfs()  { command -v bdfs  &>/dev/null || die "bdfs not found"; }
require_bootc() { command -v bootc &>/dev/null || die "bootc not found"; }

cmd_workspace() {
    require_bdfs
    local source="/" name="bootc-$(date +%Y%m%d%H%M%S)"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --source) source="$2"; shift 2 ;;
            --name)   name="$2";   shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    info "Creating workspace '${name}' from bootc root: ${source}"
    "$BDFS_CMD" workspace create --name "$name" --source "$source"
    info "Workspace created: ${name}"
}

cmd_commit() {
    require_bdfs; _bdfs_incus_require
    local workspace="" image="" push=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --image) image="$2"; shift 2 ;;
            --push)  push=true;  shift   ;;
            -*)      die "Unknown option: $1" ;;
            *)       workspace="$1"; shift ;;
        esac
    done
    [[ -n "$workspace" ]] || die "Usage: bdfs-bootc.sh commit <workspace> --image IMAGE"
    [[ -n "$image"     ]] || die "--image required"

    local ws_root
    ws_root="$("$BDFS_CMD" workspace path "$workspace")" \
        || die "Workspace not found: ${workspace}"

    local tmp_tar
    tmp_tar="$(mktemp /tmp/bdfs-bootc-XXXXXX.tar.gz)"
    info "Packing workspace rootfs..."
    tar --numeric-owner -czf "$tmp_tar" -C "$ws_root" . \
        || die "Failed to tar workspace rootfs"

    local alias
    alias="$(_bdfs_incus_image_import "$tmp_tar" "bdfs-bootc-${workspace}")"
    rm -f "$tmp_tar"

    if $push; then
        local remote_ref="${BDFS_BOOTC_REGISTRY:+${BDFS_BOOTC_REGISTRY}/}${image}"
        _bdfs_incus_image_copy_out "$alias" "$remote_ref"
        info "Pushed: ${remote_ref}"
    else
        info "Committed to local Incus image store: alias='${alias}'"
        info "To push: incus image copy local:${alias} oci:<registry>/<image>:<tag>"
    fi
}

cmd_switch() {
    require_bootc
    local image=""
    while [[ $# -gt 0 ]]; do
        case "$1" in --image) image="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done
    [[ -n "$image" ]] || die "--image required"
    info "Switching bootc image → ${image}"
    "$BOOTC_CMD" switch "$image"
}

cmd_upgrade() {
    require_bootc
    local check=false
    [[ "${1:-}" == "--check" ]] && check=true
    if $check; then "$BOOTC_CMD" upgrade --check
    else info "Running bootc upgrade..."; "$BOOTC_CMD" upgrade; fi
}

cmd_export() {
    require_bdfs
    local source="/" out="" compression="zstd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --source)      source="$2";      shift 2 ;;
            --out)         out="$2";         shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$out" ]] || die "--out required"
    info "Exporting bootc root → ${out}"
    "$BDFS_CMD" export --source "$source" --out "$out" --compression "$compression"
    info "Exported: ${out}"
}

cmd_status() {
    echo "[bdfs-bootc] Status"; echo ""
    command -v bootc &>/dev/null \
        && { echo "  bootc:"; bootc status 2>/dev/null | sed 's/^/    /' || echo "    (not a bootc system)"; echo ""; }
    command -v bdfs &>/dev/null \
        && { echo "  bdfs workspaces (bootc):"; bdfs workspace list 2>/dev/null | grep -i bootc | sed 's/^/    /' || echo "    none"; echo ""; }
    command -v incus &>/dev/null \
        && { echo "  Incus images (bdfs-bootc):"; incus image list --format csv 2>/dev/null | grep 'bdfs-bootc' | sed 's/^/    /' || echo "    none"; }
}

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift
case "$SUBCMD" in
    workspace) cmd_workspace "$@" ;;
    commit)    cmd_commit    "$@" ;;
    switch)    cmd_switch    "$@" ;;
    upgrade)   cmd_upgrade   "$@" ;;
    export)    cmd_export    "$@" ;;
    status)    cmd_status    "$@" ;;
    --help|-h) sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: workspace|commit|switch|upgrade|export|status" ;;
esac
