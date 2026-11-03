#!/usr/bin/env bash
# bdfs-ageless — Ageless Linux integration for btrfs-dwarfs-framework
#
# Distro-agnostic and architecture-agnostic. Works on any Linux distribution
# and any CPU architecture supported by qemu-user-static.
#
# Ageless Linux is a Debian-based OS in deliberate noncompliance with
# California AB 1043 and similar age verification mandates. It patches
# systemd's birthDate field and installs a stub age verification API.
# See https://agelesslinux.org/ for full context.
#
# Subcommands:
#   snapshot    Snapshot the current system before conversion
#   convert     Apply the Ageless Linux conversion to a workspace or live system
#   revert      Revert a workspace to its pre-conversion snapshot
#   export      Export a workspace as a DwarFS image
#   distribute  Build a distributable disk image via bootc-image-builder
#   status      Show conversion status and active bdfs workspaces
#   verify      Check that a workspace has no age verification infrastructure
#
# Environment:
#   BDFS_AGELESS_SCRIPT     URL or path to become-ageless.sh
#                           (default: https://agelesslinux.org/become-ageless.sh)
#   BDFS_AGELESS_MODE       Conversion mode: flagrant|standard|minimal (default: standard)
#   BDFS_AGELESS_WORKSPACE  Default workspace name
#   BDFS_BIB_IMAGE          bootc-image-builder tool image (for distribute)
#
# Dependencies: bdfs, curl, bash
# Optional:     podman (for distribute), qemu-user-static (for cross-arch)
#
# Usage:
#   bdfs-ageless.sh snapshot   [--name NAME] [--source PATH]
#   bdfs-ageless.sh convert    [--workspace NAME] [--mode MODE] [--dry-run]
#   bdfs-ageless.sh revert     [--workspace NAME]
#   bdfs-ageless.sh export     [--workspace NAME] --out PATH [--compression TYPE]
#   bdfs-ageless.sh distribute [--workspace NAME] [--type TYPE] [--out DIR]
#   bdfs-ageless.sh status
#   bdfs-ageless.sh verify     [--workspace NAME]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/bdfs-sysdetect.sh
source "${SCRIPT_DIR}/../lib/bdfs-sysdetect.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
BDFS_AGELESS_SCRIPT="${BDFS_AGELESS_SCRIPT:-https://agelesslinux.org/become-ageless.sh}"
BDFS_AGELESS_MODE="${BDFS_AGELESS_MODE:-standard}"
BDFS_AGELESS_WORKSPACE="${BDFS_AGELESS_WORKSPACE:-ageless-$(date +%Y%m%d)}"
BDFS_BIB_IMAGE="${BDFS_BIB_IMAGE:-quay.io/centos-bootc/bootc-image-builder:latest}"

# ── Helpers ───────────────────────────────────────────────────────────────────

info() { echo "[bdfs-ageless] $*"; }
warn() { echo "[bdfs-ageless] WARN: $*" >&2; }
die()  { echo "[bdfs-ageless] ERROR: $*" >&2; exit "${2:-1}"; }

require_cmd()  { command -v "$1" &>/dev/null || die "'$1' not found — install $2"; }
require_bdfs() { require_cmd bdfs "btrfs-dwarfs-framework"; }

_resolve_script() {
    if [[ "$BDFS_AGELESS_SCRIPT" == http* ]]; then
        require_cmd curl "curl"
        local tmp
        tmp="$(mktemp /tmp/become-ageless-XXXXXX.sh)"
        info "Fetching: ${BDFS_AGELESS_SCRIPT}"
        curl -fsSL "$BDFS_AGELESS_SCRIPT" -o "$tmp" \
            || die "Failed to fetch: ${BDFS_AGELESS_SCRIPT}"
        echo "$tmp"
    else
        [[ -f "$BDFS_AGELESS_SCRIPT" ]] || die "Script not found: ${BDFS_AGELESS_SCRIPT}"
        echo "$BDFS_AGELESS_SCRIPT"
    fi
}

_mode_flag() {
    case "${1:-$BDFS_AGELESS_MODE}" in
        flagrant) echo "--flagrant" ;;
        minimal)  echo "--minimal"  ;;
        standard) echo "--accept"   ;;
        *) die "Unknown mode: $1 (use: standard|flagrant|minimal)" ;;
    esac
}

_workspace_root() {
    local workspace="${1:-}"
    if [[ -n "$workspace" ]]; then
        require_bdfs
        "$BDFS_CMD" workspace path "$workspace" 2>/dev/null \
            || die "Workspace not found: ${workspace}"
    else
        echo "/"
    fi
}

# Write Ageless Linux metadata for non-Debian roots (patched via freeport).
_write_ageless_metadata() {
    local root="$1" mode="$2"
    local conf="${root%/}/etc/agelesslinux.conf"
    mkdir -p "$(dirname "$conf")"
    cat > "$conf" <<EOF
# Ageless Linux — applied via bdfs-ageless (non-Debian path)
MODE=${mode}
CONVERSION_DATE=$(date -u +%Y-%m-%dT%H:%M:%SZ)
BDFS_AGELESS_VERSION=1.0
COMPLIANCE_STATUS=NONCOMPLIANT
EOF
    local os_release="${root%/}/etc/os-release"
    if [[ -f "$os_release" ]] && ! grep -q 'Ageless' "$os_release" 2>/dev/null; then
        printf '\n# Ageless Linux overlay\nVARIANT="Ageless Linux (bdfs-ageless)"\nVARIANT_ID=ageless-linux\n' \
            >> "$os_release"
    fi
    info "Ageless Linux metadata written to ${conf}"
}

# ── Subcommands ───────────────────────────────────────────────────────────────

cmd_snapshot() {
    require_bdfs
    local name="${BDFS_AGELESS_WORKSPACE}-pre-conversion"
    local source="/"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --name)   name="$2";   shift 2 ;;
            --source) source="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    local distro arch
    distro="$(_bdfs_detect_distro "$source")"
    arch="$(_bdfs_detect_arch "$source")"
    info "Snapshot: ${name}  (distro=${distro} arch=${arch})"
    "$BDFS_CMD" snapshot create --name "$name" --source "$source" \
        || die "Failed to create snapshot"
    info "Created: ${name}"
}

cmd_convert() {
    local workspace="" mode="$BDFS_AGELESS_MODE" dry_run=false
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace) workspace="$2"; shift 2 ;;
            --mode)      mode="$2";      shift 2 ;;
            --dry-run)   dry_run=true;   shift   ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    local script mode_flag root distro arch
    script="$(_resolve_script)"
    mode_flag="$(_mode_flag "$mode")"
    root="$(_workspace_root "$workspace")"
    distro="$(_bdfs_detect_distro "$root")"
    arch="$(_bdfs_detect_arch "$root")"

    info "Target: root=${root}  distro=${distro}  arch=${arch}  mode=${mode}"

    # become-ageless.sh is Debian/Ubuntu-specific.
    # On other distros, delegate to bdfs-freeport for the patch work,
    # then write the Ageless metadata ourselves.
    if [[ "$distro" != "debian" && "$distro" != "ubuntu" ]]; then
        warn "become-ageless.sh targets Debian/Ubuntu; detected: ${distro}"
        warn "Applying equivalent patches via bdfs-freeport..."
        local freeport_script="${SCRIPT_DIR}/../freeport/bdfs-freeport.sh"
        if [[ -x "$freeport_script" ]]; then
            local fp_args=()
            [[ -n "$workspace" ]] && fp_args+=(--workspace "$workspace")
            $dry_run && fp_args+=(--dry-run)
            bash "$freeport_script" apply "${fp_args[@]}" \
                || warn "freeport apply reported errors"
        else
            warn "bdfs-freeport.sh not found — cannot patch ${distro}"
        fi
        $dry_run || _write_ageless_metadata "$root" "$mode"
        [[ "$script" == /tmp/become-ageless-* ]] && rm -f "$script"
        return 0
    fi

    if $dry_run; then
        if [[ "$root" == "/" ]]; then
            info "[dry-run] bash ${script} --dry-run"
            bash "$script" --dry-run
        else
            info "[dry-run] _bdfs_chroot ${root} bash /tmp/become-ageless.sh --dry-run"
        fi
        [[ "$script" == /tmp/become-ageless-* ]] && rm -f "$script"
        return 0
    fi

    if [[ "$root" == "/" ]]; then
        warn "Converting live system. Snapshot first if you haven't."
        bash "$script" "$mode_flag"
    else
        # Auto-snapshot, then run inside cross-arch-aware chroot
        local snap_name="${workspace:-live}-pre-conversion"
        info "Auto-snapshotting: ${snap_name}"
        "$BDFS_CMD" snapshot create --name "$snap_name" --source "$root" 2>/dev/null \
            || warn "Could not snapshot — proceeding anyway"

        local script_dest="${root%/}/tmp/become-ageless.sh"
        cp "$script" "$script_dest"
        chmod +x "$script_dest"
        # _bdfs_chroot handles qemu-user-static for cross-arch automatically
        _bdfs_chroot "$root" bash /tmp/become-ageless.sh "$mode_flag" \
            || die "Conversion failed (arch=${arch})"
        rm -f "$script_dest"
    fi

    [[ "$script" == /tmp/become-ageless-* ]] && rm -f "$script"
    info "Conversion complete."
}

cmd_revert() {
    require_bdfs
    local workspace="${BDFS_AGELESS_WORKSPACE}"
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done
    local snap_name="${workspace}-pre-conversion"
    info "Reverting '${workspace}' from '${snap_name}'..."
    "$BDFS_CMD" snapshot restore --name "$snap_name" --target "$workspace" \
        || die "Revert failed — snapshot may not exist: ${snap_name}"
    info "Reverted."
}

cmd_export() {
    require_bdfs
    local workspace="${BDFS_AGELESS_WORKSPACE}" out="" compression="zstd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace)   workspace="$2";   shift 2 ;;
            --out)         out="$2";         shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$out" ]] || die "Usage: bdfs-ageless.sh export --out PATH"
    info "Exporting '${workspace}' → ${out}"
    "$BDFS_CMD" export --workspace "$workspace" --out "$out" \
        --compression "$compression" || die "Export failed"
    info "Exported: ${out}"
}

cmd_distribute() {
    require_cmd podman "podman"
    local workspace="${BDFS_AGELESS_WORKSPACE}" type="qcow2" out="./output"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace) workspace="$2"; shift 2 ;;
            --type)      type="$2";      shift 2 ;;
            --out)       out="$2";       shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    local bib="${SCRIPT_DIR}/../bootc-image-builder/bdfs-bootc-image-builder.sh"
    [[ -x "$bib" ]] || die "bdfs-bootc-image-builder.sh not found"
    BDFS_BIB_IMAGE="$BDFS_BIB_IMAGE" \
        bash "$bib" from-workspace "$workspace" --type "$type" --out "$out"
}

cmd_status() {
    info "Ageless Linux status"
    echo ""
    local distro arch
    distro="$(_bdfs_detect_distro /)"
    arch="$(_bdfs_detect_arch /)"
    printf "  %-14s %s\n" "Host distro:" "$distro"
    printf "  %-14s %s\n" "Host arch:"   "$arch"
    echo ""
    if [[ -f /etc/agelesslinux.conf ]]; then
        echo "  Conversion: applied"
        grep -E '^(MODE|CONVERSION_DATE|COMPLIANCE_STATUS)=' /etc/agelesslinux.conf \
            | sed 's/^/    /' || true
    elif grep -q 'Ageless' /etc/os-release 2>/dev/null; then
        echo "  Conversion: applied (os-release)"
    else
        echo "  Conversion: not applied"
    fi
    echo ""
    if command -v bdfs &>/dev/null; then
        local ws
        ws="$(bdfs workspace list 2>/dev/null | grep -i ageless || true)"
        if [[ -n "$ws" ]]; then
            echo "  Ageless workspaces:"
            echo "$ws" | sed 's/^/    /'
        else
            echo "  Ageless workspaces: none"
        fi
    fi
}

cmd_verify() {
    local workspace=""
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done

    local root distro arch
    root="$(_workspace_root "$workspace")"
    distro="$(_bdfs_detect_distro "$root")"
    arch="$(_bdfs_detect_arch "$root")"

    info "Verifying: root=${root}  distro=${distro}  arch=${arch}"
    echo ""

    local found=0

    # Scan key binaries — paths are distro-agnostic (search common locations)
    local -a bin_candidates=(
        "${root%/}/usr/lib/systemd/systemd"
        "${root%/}/lib/systemd/systemd"
        "${root%/}/usr/libexec/accounts-daemon"
        "${root%/}/usr/lib/accountsservice/accounts-daemon"
        "${root%/}/usr/libexec/xdg-desktop-portal"
        "${root%/}/usr/lib/xdg-desktop-portal"
        "${root%/}/usr/lib/xdg-desktop-portal/xdg-desktop-portal"
    )
    for target in "${bin_candidates[@]}"; do
        [[ -f "$target" ]] || continue
        local hit
        hit="$(_bdfs_scan_file "$target" || true)"
        if [[ -n "$hit" ]]; then
            warn "  ✘ $(basename "$target"): found '${hit}'"
            (( found++ )) || true
        else
            info "  ✔ $(basename "$target"): clean"
        fi
    done

    # Scan D-Bus interface files (distro-agnostic paths)
    local -a iface_dirs=(
        "${root%/}/usr/share/dbus-1/interfaces"
        "${root%/}/usr/share/dbus-1/system.d"
        "${root%/}/usr/share/dbus-1/session.d"
    )
    for iface_dir in "${iface_dirs[@]}"; do
        [[ -d "$iface_dir" ]] || continue
        while IFS= read -r -d '' f; do
            local hit
            hit="$(_bdfs_scan_file "$f" || true)"
            if [[ -n "$hit" ]]; then
                warn "  ✘ D-Bus: $(basename "$f") contains '${hit}'"
                (( found++ )) || true
            fi
        done < <(find "$iface_dir" -maxdepth 1 -name "*.xml" -print0 2>/dev/null)
    done

    [[ -f "${root%/}/etc/agelesslinux.conf" ]] \
        && info "  ✔ Ageless Linux conversion record present"

    echo ""
    if [[ $found -eq 0 ]]; then
        info "Verification passed — no age verification infrastructure detected."
    else
        warn "Verification found ${found} issue(s). Run 'bdfs-ageless.sh convert' to remediate."
        exit 1
    fi
}

# ── Dispatch ──────────────────────────────────────────────────────────────────

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift

case "$SUBCMD" in
    snapshot)   cmd_snapshot   "$@" ;;
    convert)    cmd_convert    "$@" ;;
    revert)     cmd_revert     "$@" ;;
    export)     cmd_export     "$@" ;;
    distribute) cmd_distribute "$@" ;;
    status)     cmd_status     "$@" ;;
    verify)     cmd_verify     "$@" ;;
    --help|-h)  sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: snapshot|convert|revert|export|distribute|status|verify" ;;
esac
