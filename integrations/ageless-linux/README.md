# Ageless Linux Integration

Bridges [Ageless Linux](https://agelesslinux.org/)'s conversion model with
btrfs-dwarfs-framework's snapshot and workspace capabilities.

Ageless Linux is a Debian-based OS in deliberate noncompliance with California
AB 1043 and similar age verification mandates. It patches systemd's `birthDate`
userdb field, removes the `QueryAgeBracket` D-Bus interface, and installs a
stub age verification API that returns no data. See
[agelesslinux.org](https://agelesslinux.org/) for the full legal and technical
context.

## What this provides

| File | Purpose |
|---|---|
| `bdfs-ageless.sh` | CLI wrapper: snapshot, convert, revert, export, distribute, status, verify |
| `bdfs-ageless.conf` | Site configuration (copy to `/etc/bdfs/ageless.conf`) |

## Distro and architecture support

This integration is **distro-agnostic and architecture-agnostic**:

- On **Debian/Ubuntu** targets: runs `become-ageless.sh` directly (or inside a
  cross-arch chroot for foreign architectures).
- On **all other distros** (Arch, Fedora, Alpine, Void, Gentoo, openSUSE, …):
  automatically delegates to [`bdfs-freeport apply`](../freeport/) for the
  package-level patches, then writes the Ageless Linux metadata
  (`/etc/agelesslinux.conf`, `os-release` variant fields) itself.
- **Cross-arch**: uses `qemu-user-static` automatically when the target
  workspace architecture differs from the host. No manual setup required.

All detection and dispatch logic lives in the shared library
[`../lib/bdfs-sysdetect.sh`](../lib/bdfs-sysdetect.sh).

## How it fits together

1. **Pre-conversion snapshot** — `bdfs-ageless snapshot` creates a bdfs
   checkpoint. `bdfs-ageless revert` restores it.
2. **Workspace conversion** — `bdfs-ageless convert --workspace NAME` runs the
   conversion inside a bdfs workspace chroot, keeping the host untouched.
3. **DwarFS export** — `bdfs-ageless export` packs a converted workspace into a
   compressed DwarFS image for offline distribution or archival.
4. **Disk image distribution** — `bdfs-ageless distribute` delegates to the
   [bootc-image-builder integration](../bootc-image-builder/) to produce
   qcow2/raw/iso/vmdk images.
5. **Verification** — `bdfs-ageless verify` scans binaries and D-Bus interface
   files across all common install paths (distro-agnostic) for age verification
   strings.

## Usage

```bash
# Snapshot before converting
bdfs-ageless.sh snapshot --name my-system-pre-ageless

# Convert a bdfs workspace (auto-detects distro and arch)
bdfs-ageless.sh convert --workspace ageless-20260527

# Preview without making changes
bdfs-ageless.sh convert --workspace ageless-20260527 --dry-run

# Convert with flagrant mode (installs REFUSAL file + noncompliance docs)
bdfs-ageless.sh convert --workspace ageless-20260527 --mode flagrant

# Verify no age verification infrastructure remains
bdfs-ageless.sh verify --workspace ageless-20260527

# Export as DwarFS image
bdfs-ageless.sh export --workspace ageless-20260527 --out /srv/images/ageless.dwarfs

# Build a distributable qcow2 disk image
bdfs-ageless.sh distribute --workspace ageless-20260527 --type qcow2 --out ./output

# Revert if something went wrong
bdfs-ageless.sh revert --workspace ageless-20260527
```

## Cross-arch example

```bash
# Convert an aarch64 workspace from an x86_64 host (requires qemu-user-static)
bdfs-ageless.sh convert --workspace aarch64-ageless
# bdfs-sysdetect detects aarch64 ELF, copies qemu-aarch64-static,
# runs become-ageless.sh inside a qemu-backed chroot automatically.
```

## Conversion modes

| Mode | Flag | What it does |
|---|---|---|
| `standard` | `--accept` | Patches systemd birthDate, installs stub API, updates `/etc/os-release` |
| `flagrant` | `--flagrant` | Standard + installs `REFUSAL` file and noncompliance documentation |
| `minimal` | `--minimal` | Patches systemd birthDate only, no os-release changes |

## Dependencies

- `bdfs` — btrfs-dwarfs-framework CLI
- `curl` — for fetching `become-ageless.sh`
- `qemu-user-static` — only for cross-arch workspace operations
- `podman` — only for `distribute` (via bootc-image-builder)
- [`bdfs-freeport`](../freeport/) — used automatically on non-Debian/Ubuntu targets

## Relationship to Freeport

Ageless Linux operates at the OS level. [Freeport](../freeport/) operates at
the package level. On Debian/Ubuntu, `bdfs-ageless convert` runs
`become-ageless.sh`. On all other distros, it delegates to `bdfs-freeport
apply` for the patches and writes the Ageless metadata itself. The two
integrations are designed to work together.
