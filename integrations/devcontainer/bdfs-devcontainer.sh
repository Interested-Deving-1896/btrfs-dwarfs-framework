#!/usr/bin/env bash
# bdfs-devcontainer — Dev Container integration for btrfs-dwarfs-framework
#
# Bridges the Dev Container spec (https://containers.dev) with bdfs's
# snapshot and workspace capabilities. Uses Incus as the container runtime
# backend instead of Docker or Podman.
#
# Dev Containers can run inside Incus system containers or VMs. Docker/Podman
# can still be used *inside* an Incus instance (nested) if the devcontainer
# spec requires it — see bdfs-incus-runtime.sh nested-docker.
#
# Subcommands:
#   snapshot    Snapshot a running Incus devcontainer into a bdfs workspace
#   export      Export a devcontainer instance or workspace as a DwarFS archive
#   import      Import a DwarFS archive as an Incus image for devcontainer use
#   build       Build a devcontainer image via devcontainer CLI, import to Incus
#   up          Launch a devcontainer as an Incus instance
#   status      Show running Incus devcontainer instances and bdfs workspaces
#
# Environment:
#   BDFS_DC_WORKSPACE_FOLDER  Default --workspace-folder path (default: $PWD)
#   BDFS_DC_IMAGE_STORE       Directory for exported DwarFS images
#   BDFS_DC_INSTANCE_PREFIX   Prefix for Incus instance names (default: devcontainer)
#   INCUS_CMD                 Incus CLI binary (default: incus)
#
# Dependencies: devcontainer CLI, incus, bdfs
#
# Usage:
#   bdfs-devcontainer.sh snapshot [--instance NAME] [--name WORKSPACE]
#   bdfs-devcontainer.sh export   [--instance NAME] --out PATH [--compression zstd]
#   bdfs-devcontainer.sh import   <image-path> --alias ALIAS
#   bdfs-devcontainer.sh build    [--workspace-folder PATH] [--pack --out PATH]
#   bdfs-devcontainer.sh up       [--workspace-folder PATH] [--instance NAME] [--vm]
#   bdfs-devcontainer.sh status

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/bdfs-incus.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
DC_CMD="${DC_CMD:-devcontainer}"
BDFS_DC_WORKSPACE_FOLDER="${BDFS_DC_WORKSPACE_FOLDER:-$PWD}"
BDFS_DC_IMAGE_STORE="${BDFS_DC_IMAGE_STORE:-/var/lib/bdfs/devcontainer-images}"
BDFS_DC_INSTANCE_PREFIX="${BDFS_DC_INSTANCE_PREFIX:-devcontainer}"

info() { echo "[bdfs-devcontainer] $*"; }
warn() { echo "[bdfs-devcontainer] WARN: $*" >&2; }
die()  { echo "[bdfs-devcontainer] ERROR: $*" >&2; exit 1; }

require_bdfs() { command -v bdfs &>/dev/null || die "bdfs not found"; }
require_dc()   { command -v "$DC_CMD" &>/dev/null || die "devcontainer CLI not found"; }

# Derive a safe Incus instance name from a workspace folder path
_dc_instance_name() {
    local folder="${1:-$BDFS_DC_WORKSPACE_FOLDER}"
    local base
    base="$(basename "$folder" | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/-*$//')"
    echo "${BDFS_DC_INSTANCE_PREFIX}-${base}"
}

# ── snapshot ──────────────────────────────────────────────────────────────────
# Snapshot a running Incus devcontainer instance into a bdfs workspace.

cmd_snapshot() {
    require_bdfs; _bdfs_incus_require
    local instance="" ws_name=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --instance) instance="$2"; shift 2 ;;
            --name)     ws_name="$2";  shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    [[ -z "$instance" ]] && instance="$(_dc_instance_name)"
    [[ -z "$ws_name"  ]] && ws_name="${instance}-snapshot-$(date +%Y%m%d%H%M%S)"

    # Get the rootfs path of the running Incus container
    local rootfs
    rootfs="$(_bdfs_incus_rootfs_path "$instance")" \
        || die "Could not locate rootfs for instance: ${instance}"

    info "Snapshotting Incus instance '${instance}' → bdfs workspace '${ws_name}'"
    "$BDFS_CMD" workspace create --name "$ws_name" --source "$rootfs"
    info "Workspace created: ${ws_name}"
}

# ── export ────────────────────────────────────────────────────────────────────

cmd_export() {
    require_bdfs; _bdfs_incus_require
    local instance="" out="" compression="zstd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --instance)    instance="$2";    shift 2 ;;
            --out)         out="$2";         shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$out" ]] || die "--out required"
    [[ -z "$instance" ]] && instance="$(_dc_instance_name)"

    local rootfs
    rootfs="$(_bdfs_incus_rootfs_path "$instance")"

    info "Exporting '${instance}' → ${out}"
    "$BDFS_CMD" export --source "$rootfs" --out "$out" \
        --compression "$compression"
    info "Exported: ${out}"
}

# ── import ────────────────────────────────────────────────────────────────────
# Import a DwarFS archive (or any tarball) as an Incus image.

cmd_import() {
    _bdfs_incus_require
    local image_path="" alias=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --alias) alias="$2"; shift 2 ;;
            -*)      die "Unknown option: $1" ;;
            *)       image_path="$1"; shift ;;
        esac
    done
    [[ -n "$image_path" ]] || die "Usage: bdfs-devcontainer.sh import <image-path> --alias ALIAS"
    [[ -n "$alias"      ]] || alias="${BDFS_DC_INSTANCE_PREFIX}-import-$(date +%Y%m%d%H%M%S)"

    local result_alias
    result_alias="$(_bdfs_incus_image_import "$image_path" "$alias")"
    info "Imported as Incus image: alias='${result_alias}'"
    info "Launch with: incus launch local:${result_alias} <instance-name>"
}

# ── build ─────────────────────────────────────────────────────────────────────
# Build a devcontainer image using the devcontainer CLI, then import the
# resulting OCI image into the Incus local image store.

cmd_build() {
    require_dc; _bdfs_incus_require
    local workspace_folder="$BDFS_DC_WORKSPACE_FOLDER"
    local pack=false out=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace-folder) workspace_folder="$2"; shift 2 ;;
            --pack)             pack=true;             shift   ;;
            --out)              out="$2";              shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    local image_name
    image_name="${BDFS_DC_INSTANCE_PREFIX}-$(basename "$workspace_folder" \
        | tr '[:upper:]' '[:lower:]' | tr -cs 'a-z0-9' '-' | sed 's/-*$//')"

    info "Building devcontainer image for: ${workspace_folder}"

    # devcontainer build writes an OCI image. We capture the image ID.
    local build_out
    build_out="$("$DC_CMD" build \
        --workspace-folder "$workspace_folder" \
        --image-name "$image_name" \
        2>&1)" || die "devcontainer build failed"

    info "devcontainer build output:"
    echo "$build_out" | sed 's/^/  /'

    # The devcontainer CLI uses Docker/Podman internally for the build step.
    # We need to export the resulting image and import it into Incus.
    # Try docker save, then podman save, then skip if neither available.
    local tmp_tar
    tmp_tar="$(mktemp /tmp/bdfs-dc-XXXXXX.tar)"

    if command -v docker &>/dev/null; then
        docker save "$image_name" -o "$tmp_tar" \
            || die "docker save failed for: ${image_name}"
    elif command -v podman &>/dev/null; then
        podman save "$image_name" -o "$tmp_tar" \
            || die "podman save failed for: ${image_name}"
    else
        warn "Neither docker nor podman found — cannot export built image to Incus"
        warn "The image '${image_name}' was built but not imported into Incus."
        rm -f "$tmp_tar"
        return 0
    fi

    local alias
    alias="$(_bdfs_incus_image_import "$tmp_tar" "$image_name")"
    rm -f "$tmp_tar"
    info "Imported into Incus: alias='${alias}'"

    if $pack && [[ -n "$out" ]]; then
        mkdir -p "$(dirname "$out")"
        _bdfs_incus_image_export "$alias" "$(dirname "$out")"
        info "Packed: ${out}"
    fi
}

# ── up ────────────────────────────────────────────────────────────────────────
# Launch a devcontainer as an Incus instance, bind-mounting the workspace
# folder into the instance at /workspace.

cmd_up() {
    _bdfs_incus_require
    local workspace_folder="$BDFS_DC_WORKSPACE_FOLDER"
    local instance="" vm=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace-folder) workspace_folder="$2"; shift 2 ;;
            --instance)         instance="$2";         shift 2 ;;
            --vm)               vm=true;               shift   ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    [[ -z "$instance" ]] && instance="$(_dc_instance_name "$workspace_folder")"

    # Read the devcontainer.json to find the base image
    local dc_json="${workspace_folder}/.devcontainer/devcontainer.json"
    [[ -f "$dc_json" ]] || dc_json="${workspace_folder}/.devcontainer.json"
    [[ -f "$dc_json" ]] || die "No devcontainer.json found in: ${workspace_folder}"

    local base_image
    base_image="$(python3 -c "
import json, re, sys
raw = open('${dc_json}').read()
# Strip JSON comments
raw = re.sub(r'//.*', '', raw)
raw = re.sub(r'/\*.*?\*/', '', raw, flags=re.DOTALL)
d = json.loads(raw)
print(d.get('image', d.get('build', {}).get('image', '')))
" 2>/dev/null)" || base_image=""

    if [[ -z "$base_image" ]]; then
        warn "Could not extract base image from devcontainer.json"
        warn "Using ubuntu:24.04 as fallback"
        base_image="ubuntu:24.04"
    fi

    local abs_workspace
    abs_workspace="$(realpath "$workspace_folder")"

    local -a launch_args=(
        --device "workspace,type=disk,source=${abs_workspace},path=/workspace"
        --config "environment.WORKSPACE_FOLDER=/workspace"
    )
    $vm && launch_args+=(--vm)

    info "Launching devcontainer '${instance}' from image: ${base_image}"
    _bdfs_incus_launch "$base_image" "$instance" "${launch_args[@]}"
    _bdfs_incus_wait_ready "$instance" 60

    info "Instance running: ${instance}"
    info "Connect with: incus exec ${instance} -- bash"
    info "Workspace at: /workspace (inside instance)"
}

# ── status ────────────────────────────────────────────────────────────────────

cmd_status() {
    echo "[bdfs-devcontainer] Status"; echo ""
    _bdfs_incus_require
    echo "  Incus devcontainer instances:"
    "$INCUS_CMD" list --format csv 2>/dev/null \
        | grep "^${BDFS_DC_INSTANCE_PREFIX}" | sed 's/^/    /' \
        || echo "    none"
    echo ""
    if command -v bdfs &>/dev/null; then
        echo "  bdfs workspaces (devcontainer):"
        bdfs workspace list 2>/dev/null | grep -i devcontainer | sed 's/^/    /' \
            || echo "    none"
    fi
}

# ── dispatch ──────────────────────────────────────────────────────────────────

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift
case "$SUBCMD" in
    snapshot) cmd_snapshot "$@" ;;
    export)   cmd_export   "$@" ;;
    import)   cmd_import   "$@" ;;
    build)    cmd_build    "$@" ;;
    up)       cmd_up       "$@" ;;
    status)   cmd_status   "$@" ;;
    --help|-h) sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: snapshot|export|import|build|up|status" ;;
esac
