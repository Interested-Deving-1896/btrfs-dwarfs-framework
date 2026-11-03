#!/usr/bin/env bash
# bdfs-freeport — Freeport integration for btrfs-dwarfs-framework
#
# Distro-agnostic and architecture-agnostic. Applies freeport's age
# verification removal patches to any Linux distribution on any CPU
# architecture supported by qemu-user-static.
#
# Freeport patches individual packages (systemd, accountsservice,
# xdg-desktop-portal) to remove identity collection fields, then rebuilds
# them. See https://github.com/ryandward/freeport for full context.
#
# Subcommands:
#   snapshot  Snapshot the system before applying patches
#   apply     Apply freeport patches to a workspace or live system
#   scan      Scan a workspace for age verification infrastructure
#   hook      Install the freeport pre-install hook into a workspace
#   export    Export a patched workspace as a DwarFS image
#   status    Show which packages have been patched and their versions
#   update    Pull latest freeport patches and re-apply
#
# Environment:
#   BDFS_FREEPORT_REPO      Freeport git repo URL
#                           (default: https://github.com/ryandward/freeport.git)
#   BDFS_FREEPORT_DIR       Local clone path (default: ~/.cache/bdfs/freeport)
#   BDFS_FREEPORT_DISTRO    Target distro family: auto|arch|debian|ubuntu|fedora|
#                           opensuse|alpine|void|gentoo (default: auto)
#   BDFS_FREEPORT_WORKSPACE Default workspace name
#
# Dependencies: bdfs, git, bash, patch, strings (binutils)
# Optional:     makepkg, dpkg-buildpackage, rpmbuild, qemu-user-static
#
# Usage:
#   bdfs-freeport.sh snapshot [--name NAME] [--source PATH]
#   bdfs-freeport.sh apply    [--workspace NAME] [--distro DISTRO] [--pkg PKG,...] [--dry-run]
#   bdfs-freeport.sh scan     [--workspace NAME]
#   bdfs-freeport.sh hook     [--workspace NAME]
#   bdfs-freeport.sh export   [--workspace NAME] --out PATH [--compression TYPE]
#   bdfs-freeport.sh status   [--workspace NAME]
#   bdfs-freeport.sh update   [--workspace NAME]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=../lib/bdfs-sysdetect.sh
source "${SCRIPT_DIR}/../lib/bdfs-sysdetect.sh"

BDFS_CMD="${BDFS_CMD:-bdfs}"
BDFS_FREEPORT_REPO="${BDFS_FREEPORT_REPO:-https://github.com/ryandward/freeport.git}"
BDFS_FREEPORT_DIR="${BDFS_FREEPORT_DIR:-${HOME}/.cache/bdfs/freeport}"
BDFS_FREEPORT_DISTRO="${BDFS_FREEPORT_DISTRO:-auto}"
BDFS_FREEPORT_WORKSPACE="${BDFS_FREEPORT_WORKSPACE:-freeport-$(date +%Y%m%d)}"

# Packages freeport currently tracks upstream
FREEPORT_PACKAGES=(systemd accountsservice xdg-desktop-portal)

# ── Helpers ───────────────────────────────────────────────────────────────────

info() { echo "[bdfs-freeport] $*"; }
warn() { echo "[bdfs-freeport] WARN: $*" >&2; }
die()  { echo "[bdfs-freeport] ERROR: $*" >&2; exit "${2:-1}"; }

require_cmd()  { command -v "$1" &>/dev/null || die "'$1' not found — install $2"; }
require_bdfs() { require_cmd bdfs "btrfs-dwarfs-framework"; }
require_git()  { require_cmd git "git"; }

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

_ensure_freeport_repo() {
    require_git
    if [[ ! -d "${BDFS_FREEPORT_DIR}/.git" ]]; then
        info "Cloning freeport → ${BDFS_FREEPORT_DIR}"
        git clone --depth=1 "$BDFS_FREEPORT_REPO" "$BDFS_FREEPORT_DIR" \
            || die "Failed to clone freeport"
    else
        info "Updating freeport repo..."
        git -C "$BDFS_FREEPORT_DIR" pull --ff-only --quiet 2>/dev/null \
            || warn "Could not update — using cached version"
    fi
}

# Find the best-matching patch directory for a package + distro family.
# Freeport's layout: distros/<distro>/<pkg>/patches/
# Falls back to a generic "patches" dir at the repo root if no distro-specific
# patches exist, allowing distro-agnostic unified diffs to be applied anywhere.
_patch_dir() {
    local pkg="$1" distro="$2"

    # Exact distro match
    local exact="${BDFS_FREEPORT_DIR}/distros/${distro}/${pkg}/patches"
    [[ -d "$exact" ]] && { echo "$exact"; return; }

    # Family fallbacks (e.g. ubuntu → debian, fedora → rpm, opensuse → rpm)
    local fallback=""
    case "$distro" in
        ubuntu)   fallback="debian" ;;
        opensuse) fallback="fedora" ;;
        void|alpine|gentoo|slackware) fallback="generic" ;;
    esac
    if [[ -n "$fallback" ]]; then
        local fb_dir="${BDFS_FREEPORT_DIR}/distros/${fallback}/${pkg}/patches"
        [[ -d "$fb_dir" ]] && { echo "$fb_dir"; return; }
    fi

    # Generic patches at repo root (distro-neutral unified diffs)
    local generic="${BDFS_FREEPORT_DIR}/patches/${pkg}"
    [[ -d "$generic" ]] && { echo "$generic"; return; }

    echo ""
}

# Apply a set of unified diff patches to a source directory.
# Skips patches that are already applied or don't match cleanly (with warning).
_apply_patches() {
    local srcdir="$1"; shift
    local patches=("$@")
    local applied=0 skipped=0

    for p in "${patches[@]}"; do
        [[ -f "$p" ]] || continue
        if patch -Np1 --dry-run -i "$p" -d "$srcdir" &>/dev/null; then
            patch -Np1 -i "$p" -d "$srcdir" \
                && { info "    applied: $(basename "$p")"; (( applied++ )) || true; } \
                || { warn "    failed:  $(basename "$p")"; }
        else
            warn "    skipped (already applied or no match): $(basename "$p")"
            (( skipped++ )) || true
        fi
    done
    info "    ${applied} patch(es) applied, ${skipped} skipped"
}

# ── Build backends ────────────────────────────────────────────────────────────
# Each backend: fetch source, apply patches, build, install into root.
# All use _bdfs_pm_* from bdfs-sysdetect.sh for distro-agnostic operations.

_build_and_install() {
    local pkg="$1" root="$2" distro="$3"
    shift 3
    local patches=("$@")

    local build_dir
    build_dir="$(mktemp -d /tmp/bdfs-freeport-XXXXXX)"
    # shellcheck disable=SC2064
    trap "rm -rf '${build_dir}'" RETURN

    info "  Fetching source: ${pkg} (${distro})"
    _bdfs_pm_source "$root" "$pkg" "$build_dir" \
        || { warn "  Source fetch failed for ${pkg}"; return 1; }

    # Find the unpacked source directory
    local srcdir
    srcdir="$(find "$build_dir" -maxdepth 2 -name "*.spec" -o -name "PKGBUILD" \
              -o -name "debian" -type d 2>/dev/null \
              | head -1 | xargs -I{} dirname {} 2>/dev/null || true)"
    [[ -z "$srcdir" ]] && srcdir="$(find "$build_dir" -maxdepth 1 -mindepth 1 -type d | head -1)"
    [[ -d "$srcdir" ]] || { warn "  No source directory found for ${pkg}"; return 1; }

    info "  Applying ${#patches[@]} patch(es) to ${srcdir}..."
    _apply_patches "$srcdir" "${patches[@]}"

    info "  Building ${pkg}..."
    local artifact
    artifact="$(_bdfs_pm_build "$root" "$srcdir")" \
        || { warn "  Build failed for ${pkg}"; return 1; }

    [[ -n "$artifact" && -f "$artifact" ]] \
        || { warn "  No artifact produced for ${pkg}"; return 1; }

    info "  Installing: $(basename "$artifact")"
    _bdfs_pm_install_file "$root" "$artifact" \
        || { warn "  Install failed for ${pkg}"; return 1; }

    info "  ✔ ${pkg} patched and installed"
}

# ── Subcommands ───────────────────────────────────────────────────────────────

cmd_snapshot() {
    require_bdfs
    local name="${BDFS_FREEPORT_WORKSPACE}-pre-freeport" source="/"
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

cmd_apply() {
    local workspace="" distro="$BDFS_FREEPORT_DISTRO" dry_run=false
    local -a packages=("${FREEPORT_PACKAGES[@]}")

    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace) workspace="$2"; shift 2 ;;
            --distro)    distro="$2";    shift 2 ;;
            --pkg)       IFS=',' read -ra packages <<< "$2"; shift 2 ;;
            --dry-run)   dry_run=true;   shift ;;
            *) die "Unknown option: $1" ;;
        esac
    done

    _ensure_freeport_repo

    local root
    root="$(_workspace_root "$workspace")"

    # Auto-detect distro from the target root, not the host
    if [[ "$distro" == "auto" ]]; then
        distro="$(_bdfs_detect_distro "$root")"
        info "Detected distro: ${distro}"
    fi

    local arch
    arch="$(_bdfs_detect_arch "$root")"
    info "Target: root=${root}  distro=${distro}  arch=${arch}"

    if [[ "$distro" == "unknown" ]]; then
        warn "Could not detect distro for root: ${root}"
        warn "Use --distro to specify: arch|debian|ubuntu|fedora|opensuse|alpine|void|gentoo"
        return 1
    fi

    # NixOS: patches must be applied via overlay, not binary replacement
    if [[ "$distro" == "nixos" ]]; then
        warn "NixOS: binary patching is not supported. Apply freeport patches via a Nix overlay."
        warn "See: ${BDFS_FREEPORT_DIR}/distros/nixos/ (if it exists)"
        return 1
    fi

    for pkg in "${packages[@]}"; do
        local patch_dir
        patch_dir="$(_patch_dir "$pkg" "$distro")"

        if [[ -z "$patch_dir" ]]; then
            warn "  No patches for ${pkg} on ${distro} — skipping"
            continue
        fi

        local -a patches=("${patch_dir}"/*.patch)
        if [[ ! -e "${patches[0]}" ]]; then
            warn "  No .patch files in ${patch_dir} — skipping ${pkg}"
            continue
        fi

        info "Package: ${pkg} (${#patches[@]} patch(es) from ${patch_dir})"

        if $dry_run; then
            for p in "${patches[@]}"; do
                info "  [dry-run] would apply: $(basename "$p")"
            done
            continue
        fi

        _build_and_install "$pkg" "$root" "$distro" "${patches[@]}" \
            || warn "  Failed to patch ${pkg} — continuing with remaining packages"
    done

    info "Apply complete."
}

cmd_scan() {
    local workspace=""
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done

    local root distro arch
    root="$(_workspace_root "$workspace")"
    distro="$(_bdfs_detect_distro "$root")"
    arch="$(_bdfs_detect_arch "$root")"

    info "Scanning: root=${root}  distro=${distro}  arch=${arch}"
    echo ""

    local found=0

    # Scan all binaries that could contain age verification code.
    # Search common install paths across distros — no hardcoded paths.
    local -a search_dirs=(
        "${root%/}/usr/lib/systemd"
        "${root%/}/lib/systemd"
        "${root%/}/usr/libexec"
        "${root%/}/usr/lib/accountsservice"
        "${root%/}/usr/lib/xdg-desktop-portal"
        "${root%/}/usr/share/dbus-1"
    )

    for dir in "${search_dirs[@]}"; do
        [[ -d "$dir" ]] || continue
        while IFS= read -r -d '' f; do
            local hit
            hit="$(_bdfs_scan_file "$f" || true)"
            if [[ -n "$hit" ]]; then
                warn "  ✘ ${f#${root%/}}: '${hit}'"
                (( found++ )) || true
            fi
        done < <(find "$dir" -maxdepth 2 \( -type f -o -type l \) \
                 \( -name "*.so*" -o -name "systemd" -o -name "*daemon*" \
                    -o -name "*portal*" -o -name "*.xml" \) \
                 -print0 2>/dev/null)
    done

    echo ""
    if [[ $found -eq 0 ]]; then
        info "Scan complete — no age verification infrastructure detected."
    else
        warn "Scan found ${found} issue(s). Run 'bdfs-freeport.sh apply' to patch."
        exit 1
    fi
}

cmd_hook() {
    local workspace=""
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done

    local root distro
    root="$(_workspace_root "$workspace")"
    distro="$(_bdfs_detect_distro "$root")"
    local pm
    pm="$(_bdfs_detect_pm "$root")"

    info "Installing freeport hook: root=${root}  distro=${distro}  pm=${pm}"

    local hook_dir
    hook_dir="$(_bdfs_pm_hook_dir "$root")"
    mkdir -p "$hook_dir"

    # Write a distro-agnostic scan script that all hooks call
    local scan_script="${root%/}/usr/local/lib/freeport/scan.sh"
    mkdir -p "$(dirname "$scan_script")"
    cat > "$scan_script" << 'SCANEOF'
#!/usr/bin/env bash
# freeport age verification scanner — called by package manager hooks
set -euo pipefail
AGE_STRINGS=(
    birthDate birth-date BirthDate birth_date
    QueryAgeBracket AgeVerification AgeVerification1
    GetAgeBracket SetDateOfBirth SetAge ageverifyd
    "org.freedesktop.AgeVerification"
)
scan_file() {
    local f="$1"
    [[ -f "$f" ]] || return 0
    for s in "${AGE_STRINGS[@]}"; do
        if strings "$f" 2>/dev/null | grep -qF "$s" \
        || grep -qF "$s" "$f" 2>/dev/null; then
            echo "freeport: BLOCKED $(basename "$f") — contains: ${s}" >&2
            return 1
        fi
    done
}
# Called with a list of files or package paths on stdin/args
if [[ $# -gt 0 ]]; then
    for f in "$@"; do scan_file "$f" || exit 1; done
else
    while read -r f; do scan_file "$f" || exit 1; done
fi
SCANEOF
    chmod +x "$scan_script"

    # Install the PM-specific hook that calls the scan script
    case "$pm" in
        pacman)
            cat > "${hook_dir}/00-freeport-scan.hook" << 'HOOKEOF'
[Trigger]
Operation = Install
Operation = Upgrade
Type = Package
Target = *

[Action]
Description = freeport: scanning for age verification infrastructure...
When = PreTransaction
Exec = /usr/local/lib/freeport/scan.sh
AbortOnFail
HOOKEOF
            # pacman hook script wrapper
            local scripts_dir="${root%/}/usr/share/libalpm/scripts"
            mkdir -p "$scripts_dir"
            cat > "${scripts_dir}/freeport-scan" << 'WRAPEOF'
#!/usr/bin/env bash
exec /usr/local/lib/freeport/scan.sh "$@"
WRAPEOF
            chmod +x "${scripts_dir}/freeport-scan"
            info "Installed pacman hook: ${hook_dir}/00-freeport-scan.hook"
            ;;

        apt-deb)
            cat > "${hook_dir}/00freeport" << 'HOOKEOF'
// freeport pre-install hook for apt/dpkg
DPkg::Pre-Install-Pkgs {
    "/usr/local/lib/freeport/apt-scan.sh";
};
HOOKEOF
            local apt_scan="${root%/}/usr/local/lib/freeport/apt-scan.sh"
            cat > "$apt_scan" << 'APTEOF'
#!/usr/bin/env bash
# freeport apt pre-install scanner
set -euo pipefail
while read -r deb; do
    [[ -f "$deb" ]] || continue
    tmpdir="$(mktemp -d /tmp/freeport-apt-XXXXXX)"
    dpkg-deb -x "$deb" "$tmpdir" 2>/dev/null || { rm -rf "$tmpdir"; continue; }
    find "$tmpdir" -type f | /usr/local/lib/freeport/scan.sh || {
        rm -rf "$tmpdir"
        exit 1
    }
    rm -rf "$tmpdir"
done
APTEOF
            chmod +x "$apt_scan"
            info "Installed apt hook: ${hook_dir}/00freeport"
            ;;

        rpm-dnf)
            # DNF plugin (Python) — minimal stub that calls our scan script
            cat > "${hook_dir}/freeport.conf" << 'HOOKEOF'
[main]
enabled=1
HOOKEOF
            local dnf_plugin="${root%/}/usr/lib/python3/dist-packages/dnf-plugins/freeport.py"
            mkdir -p "$(dirname "$dnf_plugin")"
            cat > "$dnf_plugin" << 'PYEOF'
"""freeport DNF plugin — scans packages for age verification strings before install."""
import dnf, subprocess

class FreeportPlugin(dnf.Plugin):
    name = "freeport"
    def pre_transaction(self):
        for pkg in self.base.transaction.install_set:
            try:
                result = subprocess.run(
                    ["/usr/local/lib/freeport/scan.sh", pkg.localPkg()],
                    capture_output=True, text=True
                )
                if result.returncode != 0:
                    raise dnf.exceptions.Error(result.stderr.strip())
            except FileNotFoundError:
                pass
PYEOF
            info "Installed DNF plugin: ${dnf_plugin}"
            ;;

        apk)
            # Alpine apk commit hook
            cat > "${hook_dir}/freeport.sh" << 'HOOKEOF'
#!/usr/bin/env sh
# freeport apk commit hook
find /var/cache/apk -name "*.apk" -newer /var/lib/apk/world \
    | xargs -r /usr/local/lib/freeport/scan.sh
HOOKEOF
            chmod +x "${hook_dir}/freeport.sh"
            info "Installed apk hook: ${hook_dir}/freeport.sh"
            ;;

        xbps)
            # xbps doesn't have a pre-install hook mechanism; install a
            # wrapper script that users can call manually or via cron.
            local xbps_wrap="${root%/}/usr/local/bin/xbps-freeport-scan"
            cat > "$xbps_wrap" << 'HOOKEOF'
#!/usr/bin/env bash
# freeport xbps scanner — run before xbps-install to check packages
set -euo pipefail
find /var/cache/xbps -name "*.xbps" | /usr/local/lib/freeport/scan.sh
HOOKEOF
            chmod +x "$xbps_wrap"
            warn "xbps has no pre-install hook mechanism."
            warn "Run '${xbps_wrap}' manually before installing packages."
            info "Installed xbps scanner: ${xbps_wrap}"
            ;;

        portage)
            # Gentoo: install a package.env entry and a bashrc hook
            local portage_hook="${root%/}/etc/portage/bashrc.d/freeport.sh"
            mkdir -p "$(dirname "$portage_hook")"
            cat > "$portage_hook" << 'HOOKEOF'
# freeport Portage bashrc hook
pre_pkg_preinst() {
    find "${D}" -type f | /usr/local/lib/freeport/scan.sh \
        || die "freeport: age verification infrastructure detected in ${PN}"
}
HOOKEOF
            info "Installed Portage hook: ${portage_hook}"
            ;;

        *)
            warn "No hook implementation for package manager: ${pm}"
            warn "The scan script is at: ${scan_script}"
            warn "Wire it into your package manager's pre-install mechanism manually."
            ;;
    esac
}

cmd_export() {
    require_bdfs
    local workspace="${BDFS_FREEPORT_WORKSPACE}" out="" compression="zstd"
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --workspace)   workspace="$2";   shift 2 ;;
            --out)         out="$2";         shift 2 ;;
            --compression) compression="$2"; shift 2 ;;
            *) die "Unknown option: $1" ;;
        esac
    done
    [[ -n "$out" ]] || die "Usage: bdfs-freeport.sh export --out PATH"
    info "Exporting '${workspace}' → ${out}"
    "$BDFS_CMD" export --workspace "$workspace" --out "$out" \
        --compression "$compression" || die "Export failed"
    info "Exported: ${out}"
}

cmd_status() {
    local workspace=""
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done

    local root distro arch pm
    root="$(_workspace_root "$workspace")"
    distro="$(_bdfs_detect_distro "$root")"
    arch="$(_bdfs_detect_arch "$root")"
    pm="$(_bdfs_detect_pm "$root")"

    info "Freeport status: root=${root}  distro=${distro}  arch=${arch}  pm=${pm}"
    echo ""

    for pkg in "${FREEPORT_PACKAGES[@]}"; do
        local ver
        ver="$(_bdfs_pm_query "$root" "$pkg" 2>/dev/null || echo "(not installed)")"
        printf "  %-30s %s\n" "$pkg" "${ver:-(not installed)}"
    done

    echo ""
    local hook_dir
    hook_dir="$(_bdfs_pm_hook_dir "$root")"
    if [[ -f "${hook_dir}/00-freeport-scan.hook" ]] \
    || [[ -f "${hook_dir}/00freeport" ]] \
    || [[ -f "${hook_dir}/freeport.sh" ]] \
    || [[ -f "${hook_dir}/freeport.conf" ]]; then
        info "  Pre-install hook: installed (${hook_dir})"
    else
        warn "  Pre-install hook: not installed (run 'bdfs-freeport.sh hook')"
    fi
}

cmd_update() {
    local workspace="${BDFS_FREEPORT_WORKSPACE}"
    while [[ $# -gt 0 ]]; do
        case "$1" in --workspace) workspace="$2"; shift 2 ;; *) die "Unknown option: $1" ;; esac
    done
    info "Updating freeport patches..."
    _ensure_freeport_repo
    info "Re-applying to workspace: ${workspace}"
    cmd_apply --workspace "$workspace"
}

# ── Dispatch ──────────────────────────────────────────────────────────────────

SUBCMD="${1:-}"
[[ -z "$SUBCMD" ]] && { sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 1; }
shift

case "$SUBCMD" in
    snapshot) cmd_snapshot "$@" ;;
    apply)    cmd_apply    "$@" ;;
    scan)     cmd_scan     "$@" ;;
    hook)     cmd_hook     "$@" ;;
    export)   cmd_export   "$@" ;;
    status)   cmd_status   "$@" ;;
    update)   cmd_update   "$@" ;;
    --help|-h) sed -n '2,/^$/p' "$0" | grep '^#' | sed 's/^# \?//'; exit 0 ;;
    *) die "Unknown subcommand: ${SUBCMD}. Try: snapshot|apply|scan|hook|export|status|update" ;;
esac
