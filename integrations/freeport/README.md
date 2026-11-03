# Freeport Integration

Bridges [Freeport](https://github.com/ryandward/freeport)'s package-level age
verification patch system with btrfs-dwarfs-framework's snapshot and workspace
capabilities.

Freeport patches individual packages (systemd, accountsservice,
xdg-desktop-portal) to remove identity collection fields (`birthDate`,
`BirthDate`, `QueryAgeBracket`, etc.), then rebuilds them. It carries the
minimum diff to remove the identity fields and applies it on top of whatever
your distro ships. See [ryandward.github.io/freeport](https://ryandward.github.io/freeport/)
for the full context on why these fields exist and what legislation drives them.

## What this provides

| File | Purpose |
|---|---|
| `bdfs-freeport.sh` | CLI wrapper: snapshot, apply, scan, hook, export, status, update |
| `bdfs-freeport.conf` | Site configuration (copy to `/etc/bdfs/freeport.conf`) |

## Distro and architecture support

This integration is **distro-agnostic and architecture-agnostic**:

- Distro detection reads `/etc/os-release` from the *target root*, not the host.
  Supported families: Arch, Debian, Ubuntu, Fedora/RHEL, openSUSE, Alpine, Void,
  Gentoo. Falls back to generic unified diffs if no distro-specific patches exist.
- Architecture detection reads the ELF header of the target root's `/bin/sh`.
  Cross-arch chroot operations use `qemu-user-static` automatically when the
  target arch differs from the host.
- Package manager dispatch (`_bdfs_pm_*`) uses the target root's package manager
  (pacman, apt, dnf, zypper, apk, xbps, emerge) — not the host's.
- Pre-install hooks are installed for each package manager's native mechanism:
  pacman alpm hooks, apt `DPkg::Pre-Install-Pkgs`, DNF Python plugin, apk commit
  hooks, Portage bashrc, xbps scanner script.

All detection and dispatch logic lives in the shared library
[`../lib/bdfs-sysdetect.sh`](../lib/bdfs-sysdetect.sh).

## How it fits together

1. **Pre-patch snapshot** — `bdfs-freeport snapshot` creates a bdfs checkpoint
   before any changes. Revert with `bdfs snapshot restore`.

2. **Patch application** — `bdfs-freeport apply` clones the freeport repo,
   finds the best-matching patches for the target distro, fetches package
   source via the native package manager, applies the patches, rebuilds, and
   installs the patched package into the workspace root.

3. **Pre-install hook** — `bdfs-freeport hook` installs a package manager hook
   that scans every package before installation and blocks anything containing
   age verification strings. Works across all supported package managers.

4. **DwarFS export** — `bdfs-freeport export` packs a patched workspace into a
   compressed DwarFS image for distribution or archival.

5. **Continuous updates** — `bdfs-freeport update` pulls the latest freeport
   patches and re-applies them to a workspace, keeping pace with upstream
   package updates.

## Usage

```bash
# Snapshot before patching
bdfs-freeport.sh snapshot --name my-system-pre-freeport

# Apply patches to a bdfs workspace (auto-detects distro and arch)
bdfs-freeport.sh apply --workspace my-workspace

# Apply to a specific distro (override auto-detection)
bdfs-freeport.sh apply --workspace my-workspace --distro fedora

# Patch only specific packages
bdfs-freeport.sh apply --workspace my-workspace --pkg systemd,accountsservice

# Preview without making changes
bdfs-freeport.sh apply --workspace my-workspace --dry-run

# Scan for age verification infrastructure
bdfs-freeport.sh scan --workspace my-workspace

# Install pre-install hook (blocks future age-verification packages)
bdfs-freeport.sh hook --workspace my-workspace

# Show patched package versions
bdfs-freeport.sh status --workspace my-workspace

# Export as DwarFS image
bdfs-freeport.sh export --workspace my-workspace --out /srv/images/freeport.dwarfs

# Pull latest patches and re-apply
bdfs-freeport.sh update --workspace my-workspace
```

## Cross-arch example

```bash
# Patch an aarch64 workspace from an x86_64 host
# (requires qemu-user-static)
bdfs-freeport.sh apply --workspace aarch64-workspace
# bdfs-sysdetect detects aarch64 ELF, copies qemu-aarch64-static,
# runs the build inside a qemu-backed chroot automatically.
```

## NixOS

NixOS binary patching is not supported — packages are content-addressed and
immutable. Apply freeport patches via a Nix overlay instead. See
`~/.cache/bdfs/freeport/distros/nixos/` after cloning the repo.

## Dependencies

- `bdfs` — btrfs-dwarfs-framework CLI
- `git` — for cloning/updating the freeport patch repo
- `patch`, `strings` (binutils) — for applying and scanning patches
- Distro build tools (one of): `makepkg`, `dpkg-buildpackage`, `rpmbuild`
- `qemu-user-static` — only for cross-arch workspace operations

## Relationship to Ageless Linux

[Ageless Linux](../ageless-linux/) operates at the OS level: it converts a
running system and installs a stub age verification API. Freeport operates at
the package level: it patches individual packages before they are installed.

The two are complementary. Use Ageless Linux for the distribution-level
statement; use Freeport for ongoing package-level enforcement. The
`bdfs-ageless convert` command automatically delegates to `bdfs-freeport apply`
on non-Debian/Ubuntu systems.
