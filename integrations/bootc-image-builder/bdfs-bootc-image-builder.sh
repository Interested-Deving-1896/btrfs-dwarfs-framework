#!/usr/bin/env bash
# bdfs-bootc-image-builder — bootc-image-builder integration for btrfs-dwarfs-framework
#
# Bridges bootc-image-builder's disk image production with bdfs's workspace
# and snapshot capabilities. Provides helpers for:
#
#   build        Build a disk image from a bootc OCI image
#   from-workspace  Commit a bdfs workspace to OCI, then build a disk image
#   list-types   List supported output image types
#   push-ami     Push a built AMI to AWS EC2
#   status       Show recent builds in the output directory
#
# Environment:
#   BDFS_BIB_IMAGE      bootc-image-builder tool image. Despite the "centos-bootc" namespace,
#                       this is a distro-neutral tool published by osbuild — it builds images
#                       from ANY bootc-compatible OCI image, not just CentOS.
#                       Default: quay.io/centos-bootc/bootc-image-builder:latest
#   BDFS_BIB_OUTPUT     Output directory for built images (default: ./output)
#   BDFS_BIB_CONFIG     Path to config.toml (default: ./config.toml if it exists)
#   BDFS_BIB_ROOTLESS   Set to "true" to use rootless/KVM mode (default: false)
#   BDFS_BOOTC_IMAGE    Default bootc OCI image reference (shared with bdfs-bootc)
#
# Dependencies: podman, bootc-image-builder (pulled automatically via podman)
# Optional:     aws CLI (for push-ami), bdfs-bootc (for from-workspace)
#
# Usage:
#   bdfs-bootc-image-builder.sh build          [--image IMAGE] [--type TYPE] [--out DIR] [--config FILE] [--rootfs TYPE]
#   bdfs-bootc-image-builder.sh from-workspace <workspace-name> [--image IMAGE] [--type TYPE] [--out DIR]
#   bdfs-bootc-image-builder.sh list-types
#   bdfs-bootc-image-builder.sh push-ami       --ami-id AMI_ID --region REGION [--name NAME]
#   bdfs-bootc-image-builder.sh status         [--out DIR]

set -euo pipefail

BDFS_BIB_IMAGE="${BDFS_BIB_IMAGE:-quay.io/centos-bootc/bootc-image-builder:latest}" # tool image — distro-neutral despite the name
BDFS_BIB_OUTPUT="${BDFS_BIB_OUTPUT:-./output}"
BDFS_BIB_CONFIG="${BDFS_BIB_CONFIG:-}"
BDFS_BIB_ROOTLESS="${BDFS_BIB_ROOTLESS:-false}"
BDFS_BOOTC_IMAGE="${BDFS_BOOTC_IMAGE:-}"

PODMAN_CMD="${PODMAN_CMD:-podman}"
BDFS_BOOTC_CMD="${BDFS_BOOTC_CMD:-bdfs-bootc}"

# ── Helpers ───────────────────────────────────────────────────────────────────

info() { echo "[bdfs-bib] $*"; }
die()  { echo "[bdfs-bib] ERROR: $*" >&2; exit "${2:-1}"; }

require_cmd() {
    command -v "$1" &>/dev/null || die "'$1' not found — install $2"
}

require_podman() { require_cmd podman "podman (https://podman.io)"; }
require_bdfs_bootc() {
    command -v "$BDFS_BOOTC_CMD" &>/dev/null || \
        die "'bdfs-bootc' not found — install bdfs-bootc.sh from integrations/bootc/"
}

# Resolve config.toml: explicit flag > env var > ./config.toml if present
resolve_config() {
    local explicit="$1"
    if [[ -n "$explicit" ]]; then
        [[ -f "$explicit" ]] || die "config file not found: $explicit"
        echo "$explicit"
    elif [[ -n "$BDFS_BIB_CONFIG" ]]; then
        [[ -f "$BDFS_BIB_CONFIG" ]] || die "BDFS_BIB_CONFIG not found: $BDFS_BIB_CONFIG"
        echo "$BDFS_BIB_CONFIG"
    elif [[ -f "./config.toml" ]]; then
        echo "./config.toml"
    else
        echo ""
    fi
}

# Build the podman run arguments for bootc-image-builder
bib_run_args() {
    local image="$1" type="$2" outdir="$3" config="$4" rootfs="$5"

    local args=(
        --rm -it
        --privileged
        --pull=newer
        --security-opt label=type:unconfined_t
        -v "${outdir}:/output"
    )

    # Mount container storage — rootless uses user storage, root uses system
    if [[ "$BDFS_BIB_ROOTLESS" == "true" ]]; then
        args+=(
            -v "${HOME}/.local/share/containers/storage:/var/lib/containers/storage"
            --in-vm
        )
    else
        args+=(-v /var/lib/containers/storage:/var/lib/containers/storage)
    fi

    # Mount config.toml if provided
    if [[ -n "$config" ]]; then
        args+=(-v "$(realpath "$config"):/config.toml:ro")
    fi

    # AWS credentials for AMI builds
    if [[ "$type" == "ami" ]]; then
        [[ -n "${AWS_ACCESS_KEY_ID:-}" ]]     || die "AWS_ACCESS_KEY_ID not set (required for ami builds)"
        [[ -n "${AWS_SECRET_ACCESS_KEY:-}" ]] || die "AWS_SECRET_ACCESS_KEY not set (required for ami builds)"
        args+=(
            -e "AWS_ACCESS_KEY_ID=${AWS_ACCESS_KEY_ID}"
            -e "AWS_SECRET_ACCESS_KEY=${AWS_SECRET_ACCESS_KEY}"
        )
        [[ -n "${AWS_SESSION_TOKEN:-}" ]] && args+=(-e "AWS_SESSION_TOKEN=${AWS_SESSION_TOKEN}")
    fi

    # bootc-image-builder container + args
    args+=("$BDFS_BIB_IMAGE" --type "$type")
    [[ -n "$rootfs" ]] && args+=(--rootfs "$rootfs")
    args+=("$image")

    printf '%q ' "${args[@]}"
}

# ── build ─────────────────────────────────────────────────────────────────────

cmd_build() {
    local image="" type="qcow2" outdir="" config="" rootfs=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --image)  image="$2";  shift 2 ;;
            --type)   type="$2";   shift 2 ;;
            --out)    outdir="$2"; shift 2 ;;
            --config) config="$2"; shift 2 ;;
            --rootfs) rootfs="$2"; shift 2 ;;
            -*)       die "Unknown option: $1" ;;
            *)        die "Unexpected argument: $1" ;;
        esac
    done

    image="${image:-$BDFS_BOOTC_IMAGE}"
    [[ -n "$image" ]] || die "--image IMAGE required (or set BDFS_BOOTC_IMAGE)"

    outdir="${outdir:-$BDFS_BIB_OUTPUT}"
    mkdir -p "$outdir"

    config="$(resolve_config "$config")"

    require_podman

    info "Building $type image from: $image"
    info "Output directory: $outdir"
    [[ -n "$config" ]] && info "Config: $config"
    [[ -n "$rootfs" ]] && info "Root filesystem: $rootfs"

    # Pull the source image first so bootc-image-builder can find it
    info "Pulling source image..."
    $PODMAN_CMD pull "$image"

    # Run bootc-image-builder
    local run_args
    run_args="$(bib_run_args "$image" "$type" "$outdir" "$config" "$rootfs")"
    eval "$PODMAN_CMD run $run_args"

    info "Build complete. Output in: $outdir"
    ls -lh "$outdir"/ 2>/dev/null || true
}

# ── from-workspace ────────────────────────────────────────────────────────────

cmd_from_workspace() {
    local name="" image="" type="qcow2" outdir="" config="" rootfs=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --image)  image="$2";  shift 2 ;;
            --type)   type="$2";   shift 2 ;;
            --out)    outdir="$2"; shift 2 ;;
            --config) config="$2"; shift 2 ;;
            --rootfs) rootfs="$2"; shift 2 ;;
            -*)       die "Unknown option: $1" ;;
            *)        name="$1";   shift ;;
        esac
    done

    [[ -n "$name" ]] || die "Usage: bdfs-bootc-image-builder from-workspace <workspace-name> [options]"

    image="${image:-$BDFS_BOOTC_IMAGE}"
    [[ -n "$image" ]] || die "--image IMAGE required (or set BDFS_BOOTC_IMAGE)"

    require_bdfs_bootc
    require_podman

    # Step 1: commit the bdfs workspace to an OCI image
    info "Step 1/2: Committing bdfs workspace '$name' to OCI image: $image"
    "$BDFS_BOOTC_CMD" commit "$name" --image "$image" --push

    # Step 2: build a disk image from the committed OCI image
    info "Step 2/2: Building $type disk image from: $image"
    cmd_build --image "$image" --type "$type" \
        ${outdir:+--out "$outdir"} \
        ${config:+--config "$config"} \
        ${rootfs:+--rootfs "$rootfs"}
}

# ── list-types ────────────────────────────────────────────────────────────────

cmd_list_types() {
    cat <<'EOF'
Supported bootc-image-builder output types:

  qcow2         QEMU disk image (default) — for KVM/QEMU virtualisation
  raw           Unformatted raw disk — for bare-metal or custom partitioning
  vmdk          VMware VMDK — for vSphere, VMware Workstation/Fusion
  vhd           Virtual Hard Disk — for Hyper-V, Azure, Virtual PC
  ami           Amazon Machine Image — requires AWS credentials + --aws-ami-name
  anaconda-iso  Unattended Anaconda installer ISO — installs to first disk
  gce           Google Compute Engine image — for GCP

Root filesystem options (--rootfs):
  btrfs         Btrfs (recommended for btrfs-dwarfs-framework)
  ext4          ext4 (default for most images)
  xfs           XFS

Notes:
  - ami builds require AWS_ACCESS_KEY_ID and AWS_SECRET_ACCESS_KEY env vars
  - anaconda-iso builds from RPMs; requires network access during build
  - Use --rootfs btrfs with btrfs-dwarfs-framework for full bdfs feature support
EOF
}

# ── push-ami ──────────────────────────────────────────────────────────────────

cmd_push_ami() {
    local ami_id="" region="" name=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --ami-id)  ami_id="$2";  shift 2 ;;
            --region)  region="$2";  shift 2 ;;
            --name)    name="$2";    shift 2 ;;
            -*)        die "Unknown option: $1" ;;
            *)         die "Unexpected argument: $1" ;;
        esac
    done

    [[ -n "$ami_id" ]] || die "--ami-id AMI_ID required"
    [[ -n "$region" ]] || die "--region REGION required"

    require_cmd aws "aws CLI (https://aws.amazon.com/cli/)"

    [[ -n "${AWS_ACCESS_KEY_ID:-}" ]]     || die "AWS_ACCESS_KEY_ID not set"
    [[ -n "${AWS_SECRET_ACCESS_KEY:-}" ]] || die "AWS_SECRET_ACCESS_KEY not set"

    info "Registering AMI $ami_id in region $region"
    local register_args=(ec2 register-image
        --region "$region"
        --architecture x86_64
        --virtualization-type hvm
        --root-device-name /dev/xvda
        --block-device-mappings "[{\"DeviceName\":\"/dev/xvda\",\"Ebs\":{\"SnapshotId\":\"$ami_id\"}}]"
    )
    [[ -n "$name" ]] && register_args+=(--name "$name")

    aws "${register_args[@]}"
    info "AMI registered."
}

# ── status ────────────────────────────────────────────────────────────────────

cmd_status() {
    local outdir=""
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --out) outdir="$2"; shift 2 ;;
            -*)    die "Unknown option: $1" ;;
            *)     die "Unexpected argument: $1" ;;
        esac
    done

    outdir="${outdir:-$BDFS_BIB_OUTPUT}"

    echo "=== bootc-image-builder status ==="
    echo "Builder image: $BDFS_BIB_IMAGE"
    echo "Output dir:    $outdir"
    echo "Rootless mode: $BDFS_BIB_ROOTLESS"
    echo ""

    if [[ -d "$outdir" ]]; then
        echo "=== Recent builds in $outdir ==="
        find "$outdir" -maxdepth 2 -type f \
            \( -name "*.qcow2" -o -name "*.raw" -o -name "*.vmdk" \
               -o -name "*.vhd" -o -name "*.iso" -o -name "*.tar.gz" \) \
            -printf "%T@ %Tc  %s bytes  %p\n" 2>/dev/null | \
            sort -rn | head -10 | awk '{$1=""; print}' || \
            echo "(no disk images found)"
    else
        echo "(output directory does not exist: $outdir)"
    fi
}

# ── Dispatch ──────────────────────────────────────────────────────────────────

SUBCOMMAND="${1:-}"
shift || true

case "$SUBCOMMAND" in
    build)          cmd_build          "$@" ;;
    from-workspace) cmd_from_workspace "$@" ;;
    list-types)     cmd_list_types          ;;
    push-ami)       cmd_push_ami       "$@" ;;
    status)         cmd_status         "$@" ;;
    ""|help)
        echo "Usage: bdfs-bootc-image-builder <subcommand> [options]"
        echo ""
        echo "Subcommands:"
        echo "  build           Build a disk image from a bootc OCI image"
        echo "  from-workspace  Commit a bdfs workspace to OCI, then build a disk image"
        echo "  list-types      List supported output image types and rootfs options"
        echo "  push-ami        Register a built AMI snapshot in AWS EC2"
        echo "  status          Show builder config and recent builds"
        echo ""
        echo "Common options:"
        echo "  --image IMAGE   bootc OCI image reference (or set BDFS_BOOTC_IMAGE)"
        echo "  --type  TYPE    Output image type (default: qcow2, see list-types)"
        echo "  --out   DIR     Output directory (default: ./output)"
        echo "  --config FILE   Path to config.toml for user/SSH injection"
        echo "  --rootfs TYPE   Root filesystem type: btrfs, ext4, xfs"
        ;;
    *) die "Unknown subcommand: $SUBCOMMAND (run bdfs-bootc-image-builder help)" ;;
esac
