#!/usr/bin/env bash
# bdfs-fwupd — fwupd integration for btrfs-dwarfs-framework
#
# Bridges fwupd's firmware update lifecycle with bdfs's snapshot and
# workspace capabilities. Provides safety snapshots around firmware updates
# and DwarFS archival of firmware state for audit and recovery.
#
# Commands:
#   snapshot    Take a bdfs snapshot before applying firmware updates
#   update      Snapshot, then run fwupdmgr update (safe update flow)
#   rollback    Restore the pre-update snapshot if firmware update fails
#   export      Pack current firmware metadata + capsules into a DwarFS image
#   status      Show fwupd device/update status + any active bdfs snapshots
#   workspace   Create a bdfs workspace for testing firmware update flows
#   audit       Export firmware state history as a DwarFS archive series
#
# Environment:
#   BDFS_FWUPD_SNAPSHOT_PREFIX   Prefix for auto-named snapshots (default: fwupd)
#   BDFS_FWUPD_EXPORT_DIR        Directory for DwarFS firmware archives (default: /var/lib/bdfs/fwupd)
#   BDFS_FWUPD_KEEP_SNAPSHOTS    Number of pre-update snapshots to retain (default: 3)
#
# Dependencies: fwupd (fwupdmgr), bdfs, dwarfs (for export)
#
# Usage:
#   bdfs-fwupd.sh snapshot [--name NAME] [--source PATH]
#   bdfs-fwupd.sh update   [--check] [--no-snapshot] [--offline]
#   bdfs-fwupd.sh rollback [--snapshot NAME]
#   bdfs-fwupd.sh export   [--out PATH] [--compression zstd|lz4|none]
#   bdfs-fwupd.sh status
#   bdfs-fwupd.sh workspace [--name NAME]
#   bdfs-fwupd.sh audit    [--out-dir PATH]

set -euo pipefail

BDFS_CMD="${BDFS_CMD:-bdfs}"
FWUPD_CMD="${FWUPD_CMD:-fwupdmgr}"
DWARFS_CMD="${DWARFS_CMD:-mkdwarfs}"

BDFS_FWUPD_SNAPSHOT_PREFIX="${BDFS_FWUPD_SNAPSHOT_PREFIX:-fwupd}"
BDFS_FWUPD_EXPORT_DIR="${BDFS_FWUPD_EXPORT_DIR:-/var/lib/bdfs/fwupd}"
BDFS_FWUPD_KEEP_SNAPSHOTS="${BDFS_FWUPD_KEEP_SNAPSHOTS:-3}"

# fwupd state paths
FWUPD_STATE_DIR="/var/lib/fwupd"
FWUPD_CACHE_DIR="/var/cache/fwupd"
FWUPD_EFI_DIR="/boot/efi/EFI/fwupd"

# ── Helpers ───────────────────────────────────────────────────────────────────

info()  { echo "[bdfs-fwupd] $*"; }
warn()  { echo "[bdfs-fwupd] WARN: $*" >&2; }
die()   { echo "[bdfs-fwupd] ERROR: $*" >&2; exit "${2:-1}"; }

require_cmd() {
    command -v "$1" &>/dev/null || die "'$1' not found — install $2"
}

require_fwupd() { require_cmd fwupdmgr "fwupd (https://fwupd.org)"; }
require_bdfs()  { require_cmd bdfs     "btrfs-dwarfs-framework"; }
require_root()  { [[ "${EUID}" -eq 0 ]] || die "This command requires root"; }

timestamp() { date +"%Y%m%d-%H%M%S"; }

snapshot_name() {
    local suffix="${1:-}"
    echo "${BDFS_FWUPD_SNAPSHOT_PREFIX}-${suffix:-$(timestamp)}"
}

# List bdfs snapshots matching our prefix
list_fwupd_snapshots() {
    "${BDFS_CMD}" snapshot list 2>/dev/null \
        | grep "^${BDFS_FWUPD_SNAPSHOT_PREFIX}-" \
        | sort
}

# Prune old snapshots, keeping the N most recent
prune_old_snapshots() {
    local keep="${BDFS_FWUPD_KEEP_SNAPSHOTS}"
    local all
    mapfile -t all < <(list_fwupd_snapshots)
    local count="${#all[@]}"
    if [[ "${count}" -gt "${keep}" ]]; then
        local to_delete=$(( count - keep ))
        info "Pruning ${to_delete} old snapshot(s) (keeping ${keep})"
        for (( i=0; i<to_delete; i++ )); do
            info "  Deleting snapshot: ${all[$i]}"
            "${BDFS_CMD}" snapshot delete "${all[$i]}" || warn "Failed to delete ${all[$i]}"
        done
    fi
}

# Get fwupd version string
fwupd_version() {
    "${FWUPD_CMD}" --version 2>/dev/null | head -1 | awk '{print $NF}' || echo "unknown"
}

# ── Commands ──────────────────────────────────────────────────────────────────

cmd_snapshot() {
    local name="" source="/"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --name)   name="$2";   shift 2 ;;
            --source) source="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_bdfs
    require_root

    [[ -z "${name}" ]] && name="$(snapshot_name "pre-update")"

    info "Creating pre-update snapshot: ${name}"
    "${BDFS_CMD}" snapshot create \
        --name "${name}" \
        --source "${source}" \
        --description "fwupd pre-update snapshot (fwupd $(fwupd_version))"

    info "Snapshot created: ${name}"
    prune_old_snapshots
    echo "${name}"
}

cmd_update() {
    local check_only=false no_snapshot=false offline=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --check)       check_only=true;  shift ;;
            --no-snapshot) no_snapshot=true; shift ;;
            --offline)     offline=true;     shift ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_fwupd
    require_root

    # Refresh metadata first
    info "Refreshing firmware metadata from LVFS"
    "${FWUPD_CMD}" refresh --force 2>/dev/null || warn "Metadata refresh failed — continuing with cached data"

    # Check for available updates
    info "Checking for firmware updates"
    if ! "${FWUPD_CMD}" get-updates 2>/dev/null; then
        info "No firmware updates available"
        return 0
    fi

    if "${check_only}"; then
        info "Check-only mode — not applying updates"
        return 0
    fi

    # Snapshot before updating
    local snapshot_name=""
    if ! "${no_snapshot}"; then
        require_bdfs
        snapshot_name="$(cmd_snapshot)"
        info "Pre-update snapshot: ${snapshot_name}"
    fi

    # Apply updates
    info "Applying firmware updates"
    local update_args=()
    "${offline}" && update_args+=(--offline)

    if "${FWUPD_CMD}" update "${update_args[@]}"; then
        info "Firmware updates applied successfully"
        if [[ -n "${snapshot_name}" ]]; then
            info "Pre-update snapshot retained for rollback: ${snapshot_name}"
            info "To rollback: bdfs-fwupd rollback --snapshot ${snapshot_name}"
        fi
    else
        local exit_code=$?
        warn "fwupdmgr update exited with code ${exit_code}"
        if [[ -n "${snapshot_name}" ]]; then
            warn "Update may have failed. To rollback: bdfs-fwupd rollback --snapshot ${snapshot_name}"
        fi
        return "${exit_code}"
    fi
}

cmd_rollback() {
    local snapshot=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --snapshot) snapshot="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_bdfs
    require_root

    # If no snapshot specified, use the most recent fwupd snapshot
    if [[ -z "${snapshot}" ]]; then
        snapshot="$(list_fwupd_snapshots | tail -1)"
        [[ -z "${snapshot}" ]] && die "No fwupd snapshots found to rollback to"
        info "Using most recent snapshot: ${snapshot}"
    fi

    info "Rolling back to snapshot: ${snapshot}"
    "${BDFS_CMD}" snapshot restore "${snapshot}"
    info "Rollback complete — reboot to apply"
}

cmd_export() {
    local out="" compression="zstd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --out)         out="$2";         shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_cmd mkdwarfs "dwarfs (https://github.com/mhx/dwarfs)"
    require_root

    mkdir -p "${BDFS_FWUPD_EXPORT_DIR}"
    [[ -z "${out}" ]] && out="${BDFS_FWUPD_EXPORT_DIR}/fwupd-state-$(timestamp).dwarfs"

    # Build a staging directory of firmware state to archive
    local staging
    staging="$(mktemp -d)"
    trap 'rm -rf "${staging}"' EXIT

    info "Collecting firmware state for export"

    # fwupd state: device history, pending updates, downloaded capsules
    [[ -d "${FWUPD_STATE_DIR}" ]] && cp -a "${FWUPD_STATE_DIR}" "${staging}/fwupd-state"
    [[ -d "${FWUPD_CACHE_DIR}" ]] && cp -a "${FWUPD_CACHE_DIR}" "${staging}/fwupd-cache"
    [[ -d "${FWUPD_EFI_DIR}" ]]   && cp -a "${FWUPD_EFI_DIR}"   "${staging}/fwupd-efi"

    # fwupd device list snapshot
    "${FWUPD_CMD}" get-devices --json 2>/dev/null > "${staging}/devices.json" || true
    "${FWUPD_CMD}" get-history  2>/dev/null > "${staging}/history.txt"        || true

    # Metadata
    cat > "${staging}/MANIFEST" << EOF
bdfs-fwupd export
timestamp=$(date -u +"%Y-%m-%dT%H:%M:%SZ")
fwupd_version=$(fwupd_version)
hostname=$(hostname)
kernel=$(uname -r)
EOF

    info "Packing firmware state into DwarFS image: ${out}"
    "${DWARFS_CMD}" \
        "${staging}" "${out}" \
        --compression "${compression}" \
        --progress none

    local size
    size="$(du -sh "${out}" | cut -f1)"
    info "Export complete: ${out} (${size})"
}

cmd_status() {
    require_fwupd

    echo "=== fwupd status ==="
    "${FWUPD_CMD}" get-devices 2>/dev/null || warn "Could not get device list"
    echo ""
    echo "=== Pending updates ==="
    "${FWUPD_CMD}" get-updates 2>/dev/null || info "No updates available"
    echo ""
    echo "=== bdfs fwupd snapshots ==="
    local snapshots
    snapshots="$(list_fwupd_snapshots)"
    if [[ -n "${snapshots}" ]]; then
        echo "${snapshots}"
    else
        echo "(none)"
    fi
}

cmd_workspace() {
    local name=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --name) name="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_bdfs
    require_root

    [[ -z "${name}" ]] && name="fwupd-test-$(timestamp)"

    info "Creating bdfs workspace for firmware update testing: ${name}"
    "${BDFS_CMD}" workspace create \
        --name "${name}" \
        --description "fwupd test workspace — safe to run fwupdmgr update here"

    info "Workspace ready: ${name}"
    info "Enter workspace: bdfs workspace enter ${name}"
    info "Then run: fwupdmgr update"
    info "Exit without committing to discard all changes"
}

cmd_audit() {
    local out_dir="${BDFS_FWUPD_EXPORT_DIR}/audit"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --out-dir) out_dir="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    require_fwupd
    mkdir -p "${out_dir}"

    info "Exporting firmware audit state to ${out_dir}"

    # Export current state
    cmd_export --out "${out_dir}/current-$(timestamp).dwarfs"

    # Export update history
    local history_file="${out_dir}/update-history-$(timestamp).json"
    "${FWUPD_CMD}" get-history --json 2>/dev/null > "${history_file}" || true
    info "Update history: ${history_file}"

    # List all exports
    info "Audit archives in ${out_dir}:"
    ls -lh "${out_dir}"/*.dwarfs 2>/dev/null || info "  (none yet)"
}

# ── Dispatch ──────────────────────────────────────────────────────────────────

usage() {
    cat << EOF
Usage: bdfs-fwupd <command> [options]

Commands:
  snapshot    Take a bdfs snapshot before firmware updates
  update      Snapshot + run fwupdmgr update (safe update flow)
  rollback    Restore the pre-update snapshot
  export      Pack firmware state into a DwarFS archive
  status      Show fwupd status + active bdfs snapshots
  workspace   Create a bdfs workspace for testing firmware updates
  audit       Export firmware state history as DwarFS archives

Options vary by command. Run 'bdfs-fwupd <command> --help' for details.

Environment:
  BDFS_FWUPD_SNAPSHOT_PREFIX   Snapshot name prefix (default: fwupd)
  BDFS_FWUPD_EXPORT_DIR        DwarFS export directory (default: /var/lib/bdfs/fwupd)
  BDFS_FWUPD_KEEP_SNAPSHOTS    Snapshots to retain (default: 3)
EOF
}

[[ $# -eq 0 ]] && { usage; exit 0; }

CMD="$1"; shift
case "${CMD}" in
    snapshot)  cmd_snapshot  "$@" ;;
    update)    cmd_update    "$@" ;;
    rollback)  cmd_rollback  "$@" ;;
    export)    cmd_export    "$@" ;;
    status)    cmd_status    "$@" ;;
    workspace) cmd_workspace "$@" ;;
    audit)     cmd_audit     "$@" ;;
    --help|-h) usage ;;
    *) die "Unknown command: ${CMD}. Run 'bdfs-fwupd --help' for usage." ;;
esac
