#!/usr/bin/env bash
# bdfs-bootc-image-builder — bootc-image-builder integration for btrfs-dwarfs-framework
#
# Bridges bootc-image-builder's disk image production with bdfs's workspace
# and snapshot model. Uses Incus as the runtime backend instead of Podman.
#
# bootc-image-builder converts a bootc OCI image into a disk image
# (qcow2, raw, iso, vmdk, ami). Despite the "centos-bootc" image namespace,
# it is a distro-neutral tool.
#
# Incus replaces Podman here in two ways:
#   1. The builder tool itself runs as an ephemeral Incus VM (--vm --ephemeral),
#      which gives it the /dev access it needs to write disk images.
#   2. The output disk image can be imported directly as an Incus storage volume
#      or launched as an Incus VM via `incus launch --vm`.
#
# Subcommands:
#   build          Build a disk image from a bootc OCI image reference
#   from-workspace Build a disk image from a bdfs workspace
#   import-vm      Import a built disk image as an Incus VM
#   status         Show recent builds and any imported Incus VMs
#
# Environment:
#   BDFS_BIB_IMAGE    bootc-image-builder OCI image
#                     (default: quay.io/centos-bootc/bootc-image-builder:latest)
#   BDFS_BIB_OUT_DIR  Output directory for disk images (default: ./output)
#   INCUS_CMD         Incus CLI binary (default: incus)
#   BDFS_INCUS_POOL   Incus storage pool (default: default)
#
# Dependencies: incus, bdfs
#
# Usage:
#   bdfs-bootc-image-builder.sh build          --image IMAGE [--type TYPE] [--out DIR] [--config FILE]
#   bdfs-bootc-image-builder.sh from-workspace <workspace> [--type TYPE] [--out DIR]
#   bdfs-bootc-image-builder.sh import-vm      <disk-image> --name NAME [--pool POOL]
#   bdfs-bootc-image-builder.sh status

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "${SCRIPT_DIR}/../lib/bdfs-incus.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
BDFS_BIB_IMAGE="${BDFS_BIB_IMAGE:-quay.io/centos-bootc/bootc-image-builder:latest}"
BDFS_BIB_OUT_DIR="${BDFS_BIB_OUT_DIR:-./output}"

info() { echo "[bdfs-bib] $*"; }
warn() { echo "[bdfs-bib] WARN: $*" >&2; }
die()  { echo "[bdfs-bib] ERROR: $*" >&2; exit 1; }

# ── build ─────────────────────────────────────────────────────────────────────
#
# Runs bootc-image-builder inside an ephemeral Incus VM.
# The VM needs /dev access to write disk images, which is why we use --vm
# rather than a system container.
#
# The builder writes its output to /output inside the VM. We bind-mount
# a host directory there via an Incus disk device.

cmd_build() {
    _bdfs_incus_require
    local image="" type="qcow2" out_dir="$BDFS_BIB_OUT_DIR" config_file=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --image)  image="$2";       shift 2 ;;
            --type)   type="$2";        shift 2 ;;
            --out)    out_dir="$2";     shift 2 ;;
            --config) config_file="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$image" ]] || die "--image required (OCI image reference to convert)"

    mkdir -p "$out_dir"
    local abs_out
    abs_out="$(realpath "$out_dir")"

    info "Building ${type} disk image from: ${image}"
    info "Output directory: ${abs_out}"

    # Instance name for this build run
    local instance="bdfs-bib-$$"

    # Build the incus launch args.
    # We run bootc-image-builder as an ephemeral VM so it has full /dev access.
    # The output directory is bind-mounted into the VM at /output.
    local -a launch_args=(
        --vm
        --ephemeral
        --config "security.secureboot=false"
        --device "output,type=disk,source=${abs_out},path=/output"
    )

    # If a config file is provided, bind-mount it into the VM
    if [[ -n "$config_file" ]]; then
        [[ -f "$config_file" ]] || die "Config file not found: ${config_file}"
        local abs_config
        abs_config="$(realpath "$config_file")"
        launch_args+=(--device "bibconfig,type=disk,source=${abs_config},path=/config.toml,readonly=true")
    fi

    info "Launching ephemeral builder VM (${BDFS_BIB_IMAGE})..."
    _bdfs_incus_launch "$BDFS_BIB_IMAGE" "$instance" "${launch_args[@]}"
    _bdfs_incus_wait_ready "$instance" 120

    # Run bootc-image-builder inside the VM
    local bib_args=("--type" "$type" "--output" "/output")
    [[ -n "$config_file" ]] && bib_args+=("--config" "/config.toml")
    bib_args+=("$image")

    info "Running bootc-image-builder ${bib_args[*]}"
    _bdfs_incus_exec "$instance" -- \
        bootc-image-builder "${bib_args[@]}" \
        || die "bootc-image-builder failed"

    # VM is ephemeral — it deletes itself on stop
    _bdfs_incus_stop "$instance"

    info "Build complete. Output in: ${abs_out}"
    find "$abs_out" -maxdepth 2 -type f \( -name "*.qcow2" -o -name "*.raw" \
        -o -name "*.iso" -o -name "*.vmdk" -o -name "*.ami" \) \
        | sed 's/^/  /'
}

# ── from-workspace ────────────────────────────────────────────────────────────
#
# Builds a disk image from a bdfs workspace by:
#   1. Publishing the workspace as a local Incus image
#   2. Pushing that image to a temporary OCI-compatible local registry
#      (or using incus image export + re-import as bootc source)
#   3. Running cmd_build against it

cmd_from_workspace() {
    _bdfs_incus_require
    local workspace="" type="qcow2" out_dir="$BDFS_BIB_OUT_DIR"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --type) type="$2";    shift 2 ;;
            --out)  out_dir="$2"; shift 2 ;;
            -*)     die "Unknown option: $1" ;;
            *)      workspace="$1"; shift ;;
        esac
    done
    [[ -n "$workspace" ]] || die "Usage: bdfs-bootc-image-builder.sh from-workspace <workspace> [--type TYPE]"

    command -v bdfs &>/dev/null || die "bdfs not found"
    local ws_root
    ws_root="$(bdfs workspace path "$workspace")" \
        || die "Workspace not found: ${workspace}"

    # Publish workspace as a local Incus image
    info "Publishing workspace '${workspace}' as Incus image..."
    local tmp_tar
    tmp_tar="$(mktemp /tmp/bdfs-bib-ws-XXXXXX.tar.gz)"
    tar --numeric-owner -czf "$tmp_tar" -C "$ws_root" . \
        || die "Failed to tar workspace"
    local alias
    alias="$(_bdfs_incus_image_import "$tmp_tar" "bdfs-bib-ws-${workspace}")"
    rm -f "$tmp_tar"

    # Export the Incus image as an OCI tarball, then use it as the build source.
    # bootc-image-builder accepts a local OCI directory via oci-archive: transport.
    local oci_dir
    oci_dir="$(mktemp -d /tmp/bdfs-bib-oci-XXXXXX)"
    _bdfs_incus_image_export "$alias" "$oci_dir"

    # Find the exported tarball
    local exported_tar
    exported_tar="$(find "$oci_dir" -maxdepth 1 -name "*.tar.gz" | head -1)"
    [[ -n "$exported_tar" ]] || die "No exported image tarball found in ${oci_dir}"

    info "Building ${type} disk image from workspace '${workspace}'..."
    cmd_build --image "oci-archive:${exported_tar}" --type "$type" --out "$out_dir"

    rm -rf "$oci_dir"
}

# ── import-vm ─────────────────────────────────────────────────────────────────
#
# Imports a built disk image as an Incus VM storage volume, then creates
# a VM instance from it.

cmd_import_vm() {
    _bdfs_incus_require
    local disk_image="" name="" pool="$BDFS_INCUS_POOL"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --name) name="$2"; shift 2 ;;
            --pool) pool="$2"; shift 2 ;;
            -*)     die "Unknown option: $1" ;;
            *)      disk_image="$1"; shift ;;
        esac
    done
    [[ -n "$disk_image" ]] || die "Usage: bdfs-bootc-image-builder.sh import-vm <disk-image> --name NAME"
    [[ -n "$name"       ]] || die "--name required"
    [[ -f "$disk_image" ]] || die "Disk image not found: ${disk_image}"

    info "Importing disk image as Incus VM: ${name}"

    # Import the disk image as a storage volume
    _bdfs_incus_storage_import "$pool" "$disk_image" "${name}-disk" --type custom

    # Create a VM that boots from the imported volume
    "$INCUS_CMD" init --empty "$name" --vm \
        --config "security.secureboot=false" \
        || die "Failed to create VM: ${name}"

    "$INCUS_CMD" config device add "$name" root disk \
        pool="$pool" source="${name}-disk" path=/ \
        || die "Failed to attach disk to VM: ${name}"

    info "VM created: ${name}"
    info "Start with: incus start ${name}"
}

# ── status ────────────────────────────────────────────────────────────────────

cmd_status() {
    echo "[bdfs-bib] Status"; echo ""
    echo "  Builder image: ${BDFS_BIB_IMAGE}"
    echo "  Output dir:    ${BDFS_BIB_OUT_DIR}"
    echo ""
    if command -v incus &>/dev/null; then
        echo "  Incus VMs (bdfs-bib):"
        incus list --format csv 2>/dev/null | grep 'bdfs-bib' | sed 's/^/    /' \
            || echo "    none"
        echo ""
        echo "  Incus images (bdfs-bib):"
        incus image list --format csv 2>/dev/null | grep 'bdfs-bib' | sed 's/^/    /' \
            || echo "    none"
    fi
}

# ── dispatch ──────────────────────────────────────────────────────────────────

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift
case "$SUBCMD" in
    build)          cmd_build          "$@" ;;
    from-workspace) cmd_from_workspace "$@" ;;
    import-vm)      cmd_import_vm      "$@" ;;
    status)         cmd_status         "$@" ;;
    --help|-h)      sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: build|from-workspace|import-vm|status" ;;
esac
