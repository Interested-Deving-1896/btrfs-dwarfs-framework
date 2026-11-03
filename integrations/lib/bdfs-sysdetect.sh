#!/usr/bin/env bash
# integrations/lib/bdfs-sysdetect.sh
#
# Shared distro, architecture, and package manager detection for
# btrfs-dwarfs-framework integrations.
#
# Sourced by bdfs-ageless.sh and bdfs-freeport.sh (and any future integration
# that needs to operate across distros and architectures).
#
# Never executed directly.
#
# ── Distro detection ──────────────────────────────────────────────────────────
#
# _bdfs_detect_distro [ROOT]
#   Returns a normalised distro family string for the given root (default: /).
#   Output values:
#     arch        Arch Linux and derivatives (Manjaro, EndeavourOS, Garuda, …)
#     debian      Debian, Devuan, Kali, MX Linux, antiX, …
#     ubuntu      Ubuntu and derivatives (Mint, Pop!_OS, elementary, …)
#     fedora      Fedora, RHEL, CentOS Stream, AlmaLinux, Rocky, …
#     opensuse    openSUSE Leap / Tumbleweed, SLES
#     alpine      Alpine Linux
#     void        Void Linux
#     gentoo      Gentoo Linux
#     nixos       NixOS
#     slackware   Slackware
#     unknown     Could not determine
#
# ── Architecture detection ────────────────────────────────────────────────────
#
# _bdfs_detect_arch [ROOT]
#   Returns the CPU architecture of the given root (default: host).
#   Uses the ELF header of /bin/sh (or /usr/bin/sh) inside the root,
#   falling back to uname -m on the host.
#   Output values match Linux kernel arch names:
#     x86_64  aarch64  armv7l  armv6l  riscv64  s390x
#     ppc64le  mips64el  loongarch64  i686
#
# ── Cross-arch chroot ─────────────────────────────────────────────────────────
#
# _bdfs_chroot ROOT [CMD...]
#   Runs CMD inside ROOT, automatically registering qemu-user-static binfmt
#   handlers if the root's architecture differs from the host.
#   Falls back to plain chroot if architectures match.
#
# ── Package manager dispatch ──────────────────────────────────────────────────
#
# _bdfs_pm_install ROOT PKG...       Install packages
# _bdfs_pm_remove  ROOT PKG...       Remove packages
# _bdfs_pm_query   ROOT PKG          Print installed version, or empty string
# _bdfs_pm_source  ROOT PKG DESTDIR  Fetch source into DESTDIR (best-effort)
# _bdfs_pm_build   ROOT SRCDIR       Build package in SRCDIR, print artifact path
# _bdfs_pm_install_file ROOT FILE    Install a pre-built package file
# _bdfs_pm_hook_dir ROOT             Print the hook/trigger directory for the PM
#
# All _bdfs_pm_* functions detect the package manager from the root, not the
# host, so they work correctly when operating on a foreign-arch workspace.

[[ -n "${_BDFS_SYSDETECT_LOADED:-}" ]] && return 0
_BDFS_SYSDETECT_LOADED=1

# ── Internal helpers ──────────────────────────────────────────────────────────

_bdfs_sd_info() { echo "[bdfs-sysdetect] $*"; }
_bdfs_sd_warn() { echo "[bdfs-sysdetect] WARN: $*" >&2; }
_bdfs_sd_die()  { echo "[bdfs-sysdetect] ERROR: $*" >&2; exit 1; }

# ── Distro detection ──────────────────────────────────────────────────────────

_bdfs_detect_distro() {
    local root="${1:-/}"
    local os_release="${root%/}/etc/os-release"

    if [[ ! -f "$os_release" ]]; then
        # Try /usr/lib/os-release (systemd standard fallback)
        os_release="${root%/}/usr/lib/os-release"
        [[ -f "$os_release" ]] || { echo "unknown"; return; }
    fi

    local id id_like
    id="$(      . "$os_release" 2>/dev/null; echo "${ID:-}")"
    id_like="$( . "$os_release" 2>/dev/null; echo "${ID_LIKE:-}")"

    # Normalise to family
    case "$id" in
        arch|manjaro|endeavouros|garuda|artix|parabola|hyperbola)
            echo "arch"; return ;;
        debian|devuan|kali|mx|antix|pureos|tails|raspbian|armbian)
            echo "debian"; return ;;
        ubuntu|linuxmint|pop|elementary|zorin|neon|kubuntu|xubuntu|lubuntu)
            echo "ubuntu"; return ;;
        fedora|rhel|centos|almalinux|rocky|ol|scientific|nobara|ultramarine)
            echo "fedora"; return ;;
        opensuse*|sles|sled)
            echo "opensuse"; return ;;
        alpine)
            echo "alpine"; return ;;
        void)
            echo "void"; return ;;
        gentoo)
            echo "gentoo"; return ;;
        nixos)
            echo "nixos"; return ;;
        slackware)
            echo "slackware"; return ;;
    esac

    # Fall back to ID_LIKE chain
    for like in $id_like; do
        case "$like" in
            arch)    echo "arch";     return ;;
            debian)  echo "debian";   return ;;
            ubuntu)  echo "ubuntu";   return ;;
            fedora|rhel) echo "fedora"; return ;;
            opensuse*) echo "opensuse"; return ;;
            alpine)  echo "alpine";   return ;;
            void)    echo "void";     return ;;
            gentoo)  echo "gentoo";   return ;;
        esac
    done

    # Last resort: probe for package manager binaries in the root
    for pm_bin in pacman dpkg rpm xbps-query apk emerge; do
        if [[ -x "${root%/}/usr/bin/${pm_bin}" ]] \
        || [[ -x "${root%/}/bin/${pm_bin}" ]]; then
            case "$pm_bin" in
                pacman)      echo "arch";     return ;;
                dpkg)        echo "debian";   return ;;
                rpm)         echo "fedora";   return ;;
                xbps-query)  echo "void";     return ;;
                apk)         echo "alpine";   return ;;
                emerge)      echo "gentoo";   return ;;
            esac
        fi
    done

    echo "unknown"
}

# ── Architecture detection ────────────────────────────────────────────────────

_bdfs_detect_arch() {
    local root="${1:-/}"

    # Try to read ELF e_machine from /bin/sh or /usr/bin/sh inside the root
    local sh_bin
    for candidate in "${root%/}/bin/sh" "${root%/}/usr/bin/sh" \
                     "${root%/}/bin/bash" "${root%/}/usr/bin/bash"; do
        if [[ -f "$candidate" ]]; then
            sh_bin="$candidate"
            break
        fi
    done

    if [[ -n "${sh_bin:-}" ]] && command -v python3 &>/dev/null; then
        local arch
        arch="$(python3 - "$sh_bin" 2>/dev/null <<'PYEOF'
import sys, struct
try:
    with open(sys.argv[1], 'rb') as f:
        magic = f.read(4)
        if magic != b'\x7fELF':
            sys.exit(1)
        f.seek(4)
        ei_class = ord(f.read(1))   # 1=32bit, 2=64bit
        ei_data  = ord(f.read(1))   # 1=LE, 2=BE
        f.seek(18)
        fmt = '<H' if ei_data == 1 else '>H'
        e_machine = struct.unpack(fmt, f.read(2))[0]
        # https://en.wikipedia.org/wiki/Executable_and_Linkable_Format#ISA
        table = {
            0x03: 'i686',
            0x3e: 'x86_64',
            0x28: 'armv7l',   # ARM (32-bit)
            0xb7: 'aarch64',
            0xf3: 'riscv64',
            0x16: 's390x',
            0x15: 'ppc64le',
            0x08: 'mips64el',
            0x102: 'loongarch64',
        }
        # Distinguish armv6l from armv7l by EF_ARM_EABI flags (rough heuristic)
        if e_machine == 0x28 and ei_class == 1:
            f.seek(36)
            flags = struct.unpack('<I' if ei_data == 1 else '>I', f.read(4))[0]
            if (flags >> 24) < 4:
                print('armv6l')
            else:
                print('armv7l')
        else:
            print(table.get(e_machine, 'unknown'))
except Exception:
    sys.exit(1)
PYEOF
)"
        if [[ -n "$arch" && "$arch" != "unknown" ]]; then
            echo "$arch"
            return
        fi
    fi

    # Fall back to file(1) if available
    if [[ -n "${sh_bin:-}" ]] && command -v file &>/dev/null; then
        local file_out
        file_out="$(file -b "$sh_bin" 2>/dev/null)"
        case "$file_out" in
            *x86-64*)       echo "x86_64";      return ;;
            *aarch64*)      echo "aarch64";     return ;;
            *ARM*EABI*v7*)  echo "armv7l";      return ;;
            *ARM*EABI*)     echo "armv6l";      return ;;
            *RISC-V*)       echo "riscv64";     return ;;
            *"IBM S/390"*)  echo "s390x";       return ;;
            *64-bit*PowerPC*) echo "ppc64le";   return ;;
            *MIPS*)         echo "mips64el";    return ;;
            *LoongArch*)    echo "loongarch64"; return ;;
            *80386*)        echo "i686";        return ;;
        esac
    fi

    # Last resort: host uname
    uname -m
}

# Returns the host architecture (normalised to kernel names)
_bdfs_host_arch() {
    local m
    m="$(uname -m)"
    case "$m" in
        x86_64)          echo "x86_64"      ;;
        aarch64|arm64)   echo "aarch64"     ;;
        armv7l|armv7*)   echo "armv7l"      ;;
        armv6l|armv6*)   echo "armv6l"      ;;
        riscv64)         echo "riscv64"     ;;
        s390x)           echo "s390x"       ;;
        ppc64le|ppc64el) echo "ppc64le"     ;;
        mips64el)        echo "mips64el"    ;;
        loongarch64)     echo "loongarch64" ;;
        i686|i386|i486|i586) echo "i686"   ;;
        *)               echo "$m"          ;;
    esac
}

# Maps a kernel arch name to the qemu-user-static binary name
_bdfs_qemu_binary() {
    case "$1" in
        aarch64)     echo "qemu-aarch64-static"     ;;
        armv7l)      echo "qemu-arm-static"         ;;
        armv6l)      echo "qemu-arm-static"         ;;
        riscv64)     echo "qemu-riscv64-static"     ;;
        s390x)       echo "qemu-s390x-static"       ;;
        ppc64le)     echo "qemu-ppc64le-static"     ;;
        mips64el)    echo "qemu-mips64el-static"    ;;
        loongarch64) echo "qemu-loongarch64-static" ;;
        i686)        echo "qemu-i386-static"        ;;
        x86_64)      echo ""  ;; # no qemu needed for x86_64 on x86_64
        *)           echo ""  ;;
    esac
}

# ── Cross-arch chroot ─────────────────────────────────────────────────────────

# _bdfs_chroot ROOT [CMD...]
# Runs CMD inside ROOT. Automatically sets up qemu-user-static if the root
# arch differs from the host arch. Mounts /proc, /sys, /dev if not already
# mounted (and unmounts them on exit).
_bdfs_chroot() {
    local root="$1"
    shift

    local root_arch host_arch
    root_arch="$(_bdfs_detect_arch "$root")"
    host_arch="$(_bdfs_host_arch)"

    local qemu_bin=""
    if [[ "$root_arch" != "$host_arch" ]]; then
        qemu_bin="$(_bdfs_qemu_binary "$root_arch")"
        if [[ -n "$qemu_bin" ]]; then
            local qemu_path
            qemu_path="$(command -v "$qemu_bin" 2>/dev/null \
                || find /usr/bin /usr/local/bin -name "$qemu_bin" 2>/dev/null | head -1)"
            if [[ -z "$qemu_path" ]]; then
                _bdfs_sd_die "Cross-arch chroot requires ${qemu_bin} — install qemu-user-static"
            fi
            # Copy qemu binary into the root if not already there
            if [[ ! -f "${root}/usr/bin/${qemu_bin}" ]]; then
                install -m 755 "$qemu_path" "${root}/usr/bin/${qemu_bin}"
                _BDFS_CHROOT_QEMU_INSTALLED="${root}/usr/bin/${qemu_bin}"
            fi
            _bdfs_sd_info "Cross-arch chroot: ${host_arch} → ${root_arch} via ${qemu_bin}"
        fi
    fi

    # Mount pseudo-filesystems if needed
    local _mounts_done=()
    _bdfs_mount_pseudo() {
        local fs="$1" type="$2" target="${root}${3}"
        if ! mountpoint -q "$target" 2>/dev/null; then
            mkdir -p "$target"
            mount -t "$type" "$fs" "$target" 2>/dev/null && _mounts_done+=("$target")
        fi
    }
    _bdfs_mount_pseudo proc  proc  /proc
    _bdfs_mount_pseudo sysfs sysfs /sys
    _bdfs_mount_pseudo devtmpfs devtmpfs /dev 2>/dev/null \
        || _bdfs_mount_pseudo udev devtmpfs /dev 2>/dev/null || true
    if [[ -d "${root}/dev" ]]; then
        mountpoint -q "${root}/dev/pts" 2>/dev/null \
            || { mkdir -p "${root}/dev/pts"
                 mount -t devpts devpts "${root}/dev/pts" 2>/dev/null \
                     && _mounts_done+=("${root}/dev/pts") || true; }
    fi

    # Run the chroot
    local rc=0
    chroot "$root" "$@" || rc=$?

    # Unmount in reverse order
    local i
    for (( i=${#_mounts_done[@]}-1; i>=0; i-- )); do
        umount -l "${_mounts_done[$i]}" 2>/dev/null || true
    done

    # Remove qemu binary if we installed it
    [[ -n "${_BDFS_CHROOT_QEMU_INSTALLED:-}" ]] \
        && rm -f "$_BDFS_CHROOT_QEMU_INSTALLED" \
        && unset _BDFS_CHROOT_QEMU_INSTALLED

    return $rc
}

# ── Package manager detection ─────────────────────────────────────────────────

# Returns the package manager family for a root
_bdfs_detect_pm() {
    local root="${1:-/}"
    local distro
    distro="$(_bdfs_detect_distro "$root")"
    case "$distro" in
        arch)      echo "pacman"  ;;
        debian)    echo "apt-deb" ;;
        ubuntu)    echo "apt-deb" ;;
        fedora)    echo "rpm-dnf" ;;
        opensuse)  echo "rpm-zyp" ;;
        alpine)    echo "apk"     ;;
        void)      echo "xbps"    ;;
        gentoo)    echo "portage" ;;
        nixos)     echo "nix"     ;;
        slackware) echo "pkgtool" ;;
        *)
            # Probe binaries directly
            for b in pacman apt-get dnf zypper apk xbps-install emerge nix-env; do
                [[ -x "${root%/}/usr/bin/$b" || -x "${root%/}/bin/$b" ]] \
                    || command -v "$b" &>/dev/null || continue
                case "$b" in
                    pacman)      echo "pacman";  return ;;
                    apt-get)     echo "apt-deb"; return ;;
                    dnf|yum)     echo "rpm-dnf"; return ;;
                    zypper)      echo "rpm-zyp"; return ;;
                    apk)         echo "apk";     return ;;
                    xbps-install) echo "xbps";  return ;;
                    emerge)      echo "portage"; return ;;
                    nix-env)     echo "nix";     return ;;
                esac
            done
            echo "unknown"
            ;;
    esac
}

# ── Package manager dispatch ──────────────────────────────────────────────────

# Install packages into ROOT (or live system if ROOT="/")
_bdfs_pm_install() {
    local root="$1"; shift
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    case "$pm" in
        pacman)
            if [[ "$root" == "/" ]]; then
                pacman -S --noconfirm "$@"
            else
                _bdfs_chroot "$root" pacman -S --noconfirm --needed "$@"
            fi ;;
        apt-deb)
            if [[ "$root" == "/" ]]; then
                DEBIAN_FRONTEND=noninteractive apt-get install -y "$@"
            else
                _bdfs_chroot "$root" env DEBIAN_FRONTEND=noninteractive \
                    apt-get install -y "$@"
            fi ;;
        rpm-dnf)
            if [[ "$root" == "/" ]]; then
                dnf install -y "$@"
            else
                dnf install -y --installroot="$root" "$@"
            fi ;;
        rpm-zyp)
            if [[ "$root" == "/" ]]; then
                zypper install -y "$@"
            else
                zypper --root "$root" install -y "$@"
            fi ;;
        apk)
            if [[ "$root" == "/" ]]; then
                apk add "$@"
            else
                apk --root "$root" add "$@"
            fi ;;
        xbps)
            if [[ "$root" == "/" ]]; then
                xbps-install -Sy "$@"
            else
                xbps-install -Sy -r "$root" "$@"
            fi ;;
        portage)
            if [[ "$root" == "/" ]]; then
                emerge "$@"
            else
                ROOT="$root" emerge "$@"
            fi ;;
        nix)
            _bdfs_sd_warn "NixOS: package install via nix-env not supported in chroot context"
            return 1 ;;
        *)
            _bdfs_sd_warn "Unknown package manager for root: ${root} — cannot install packages"
            return 1 ;;
    esac
}

# Query installed version of PKG in ROOT. Prints version or empty string.
_bdfs_pm_query() {
    local root="$1"
    local pkg="$2"
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    case "$pm" in
        pacman)
            _bdfs_chroot "$root" pacman -Q "$pkg" 2>/dev/null | awk '{print $2}' || true ;;
        apt-deb)
            _bdfs_chroot "$root" dpkg-query -W -f='${Version}' "$pkg" 2>/dev/null || true ;;
        rpm-dnf|rpm-zyp)
            _bdfs_chroot "$root" rpm -q --queryformat '%{VERSION}-%{RELEASE}' "$pkg" 2>/dev/null || true ;;
        apk)
            _bdfs_chroot "$root" apk info -e "$pkg" 2>/dev/null \
                | grep "^${pkg}-" | sed "s/^${pkg}-//" || true ;;
        xbps)
            _bdfs_chroot "$root" xbps-query -p pkgver "$pkg" 2>/dev/null \
                | sed "s/^${pkg}-//" || true ;;
        portage)
            _bdfs_chroot "$root" qlist -Iv "$pkg" 2>/dev/null | head -1 || true ;;
        *)
            echo "" ;;
    esac
}

# Fetch package source into DESTDIR. Best-effort; not all PMs support this.
_bdfs_pm_source() {
    local root="$1"
    local pkg="$2"
    local destdir="$3"
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    mkdir -p "$destdir"
    case "$pm" in
        apt-deb)
            ( cd "$destdir"
              _bdfs_chroot "$root" apt-get source "$pkg" 2>&1 ) ;;
        rpm-dnf)
            ( cd "$destdir"
              dnf download --source "$pkg" 2>&1
              rpm -i ./*.src.rpm 2>/dev/null || true ) ;;
        pacman)
            # Clone ABS/AUR PKGBUILD
            local aur_url="https://aur.archlinux.org/cgit/aur.git/snapshot/${pkg}.tar.gz"
            curl -fsSL "$aur_url" | tar -xz -C "$destdir" 2>/dev/null \
                || _bdfs_sd_warn "Could not fetch PKGBUILD for ${pkg} from AUR" ;;
        *)
            _bdfs_sd_warn "Source fetch not implemented for PM: ${pm}" ;;
    esac
}

# Build a package from SRCDIR. Returns path to the built artifact.
_bdfs_pm_build() {
    local root="$1"
    local srcdir="$2"
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    case "$pm" in
        pacman)
            ( cd "$srcdir"; makepkg -sf --noconfirm 2>&1 )
            find "$srcdir" -maxdepth 1 -name "*.pkg.tar.*" | head -1 ;;
        apt-deb)
            ( cd "$srcdir"; dpkg-buildpackage -us -uc -b -j"$(nproc)" 2>&1 )
            find "$(dirname "$srcdir")" -maxdepth 1 -name "*.deb" | head -1 ;;
        rpm-dnf|rpm-zyp)
            ( cd "$srcdir"; rpmbuild -bb ./*.spec 2>&1 )
            find ~/rpmbuild/RPMS -name "*.rpm" | head -1 ;;
        *)
            _bdfs_sd_warn "Build not implemented for PM: ${pm}"
            return 1 ;;
    esac
}

# Install a pre-built package file into ROOT
_bdfs_pm_install_file() {
    local root="$1"
    local file="$2"
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    case "$pm" in
        pacman)
            if [[ "$root" == "/" ]]; then
                pacman -U --noconfirm "$file"
            else
                # Copy into root, install, remove
                local tmp_dest="${root}/tmp/$(basename "$file")"
                cp "$file" "$tmp_dest"
                _bdfs_chroot "$root" pacman -U --noconfirm "/tmp/$(basename "$file")"
                rm -f "$tmp_dest"
            fi ;;
        apt-deb)
            if [[ "$root" == "/" ]]; then
                dpkg -i "$file"
            else
                dpkg-deb -x "$file" "$root"
            fi ;;
        rpm-dnf|rpm-zyp)
            if [[ "$root" == "/" ]]; then
                rpm -Uvh "$file"
            else
                rpm --root "$root" -Uvh "$file"
            fi ;;
        apk)
            if [[ "$root" == "/" ]]; then
                apk add --allow-untrusted "$file"
            else
                apk --root "$root" add --allow-untrusted "$file"
            fi ;;
        xbps)
            if [[ "$root" == "/" ]]; then
                xbps-install -y "$file"
            else
                xbps-install -y -r "$root" "$file"
            fi ;;
        *)
            _bdfs_sd_warn "install_file not implemented for PM: ${pm}"
            return 1 ;;
    esac
}

# Returns the directory where package manager hooks/triggers should be installed
_bdfs_pm_hook_dir() {
    local root="$1"
    local pm
    pm="$(_bdfs_detect_pm "$root")"
    case "$pm" in
        pacman)  echo "${root%/}/usr/share/libalpm/hooks" ;;
        apt-deb) echo "${root%/}/etc/apt/apt.conf.d"      ;;
        rpm-dnf) echo "${root%/}/etc/dnf/plugins"         ;;
        rpm-zyp) echo "${root%/}/etc/zypp/plugins/commit" ;;
        apk)     echo "${root%/}/etc/apk/commit_hooks.d"  ;;
        xbps)    echo "${root%/}/etc/xbps.d"              ;;
        portage) echo "${root%/}/etc/portage/env"         ;;
        *)       echo "${root%/}/etc/pkg-hooks.d"         ;;
    esac
}

# ── Age verification string database ─────────────────────────────────────────
#
# Canonical list of strings that indicate age verification infrastructure.
# Used by both bdfs-ageless and bdfs-freeport for scanning.

BDFS_AGE_STRINGS=(
    # systemd userdb / homectl
    "birthDate"
    "birth-date"
    "birth_date"
    "--birth-date"
    # accountsservice
    "BirthDate"
    # xdg-desktop-portal / xdg-specs
    "QueryAgeBracket"
    "AgeVerification"
    "AgeVerification1"
    "GetAgeBracket"
    "SetDateOfBirth"
    "SetAge"
    # ageverifyd reference daemon
    "ageverifyd"
    "org.freedesktop.AgeVerification"
    # Calamares / archinstall installer fields
    "birthdate"
    "birth_date_field"
)

# Scan a binary or text file for age verification strings.
# Returns 0 if any found, 1 if clean.
# Usage: _bdfs_scan_file FILE
_bdfs_scan_file() {
    local file="$1"
    [[ -f "$file" ]] || return 1
    local s
    for s in "${BDFS_AGE_STRINGS[@]}"; do
        if strings "$file" 2>/dev/null | grep -qF "$s" \
        || grep -qF "$s" "$file" 2>/dev/null; then
            echo "$s"
            return 0
        fi
    done
    return 1
}
