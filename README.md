[update-readmes]   Mode: rewrite — migrating to template structure...
# btrfs-dwarfs-framework

[![Built with Ona](https://ona.com/build-with-ona.svg)](https://app.ona.com/#https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework)

<!-- AI:start:what-it-does -->
This project provides a hybrid filesystem framework that integrates BTRFS subvolumes and snapshots with DwarFS compressed images into a unified namespace. It is designed for developers and system administrators who need efficient storage management and compression for Linux-based systems. The framework enables seamless interaction between BTRFS and DwarFS, optimizing storage usage and performance.
<!-- AI:end:what-it-does -->

## Architecture

<!-- AI:start:architecture -->
The BTRFS+DwarFS framework integrates BTRFS subvolumes and snapshots with DwarFS compressed images into a unified namespace. Core components:

1. **BTRFS Subvolume Manager** — create, delete, snapshot, and promote/demote BTRFS subvolumes.
2. **DwarFS Image Handler** — mount, export, import, and cache DwarFS compressed images.
3. **Namespace Orchestrator** — unified overlay of BTRFS subvolumes and DwarFS images via kernel blend module or userspace fuse-overlayfs.
4. **bdfs dev** — mutable development workspaces on top of any immutable root (OSTree deployment, bootc image, IncusOS root, dev container, or plain directory).

```plaintext
.
├── bin/                        # Compiled binaries
├── boot/                       # Boot integration (UEFI, systemd-boot, GRUB)
├── cmd/                        # Go CLI entry points
├── config/                     # gitlab-subgroups.yml, workflow-sync.yml, cost profiles
├── doc/                        # Design docs and guides
├── integrations/               # Per-ecosystem bdfs integration scripts
│   ├── ostree/                 #   OSTree: commit, publish, export, import, prune
│   ├── bootc/                  #   bootc: workspace, commit, switch, upgrade, export
│   ├── incus-os/               #   IncusOS: workspace, export, import, update
│   ├── devcontainer/           #   Dev Containers: snapshot, export, import, build, up
│   ├── ostree-upstream/        #   ostreedev/ostree (submodule)
│   ├── bootc-upstream/         #   bootc-dev/bootc (submodule)
│   ├── incus-os-upstream/      #   lxc/incus-os (submodule)
│   ├── ashos/                  #   openos-project/ashos (submodule)
│   ├── btrfs-assistant/        #   openos-project/btrfs-assistant (submodule)
│   ├── btr-fs-git/             #   openos-project/btr-fs-git (submodule)
│   ├── frzr-meta-root/         #   openos-project/frzr-meta-root (submodule)
│   ├── gitlab-enhanced/        #   openos-project/gitlab-enhanced (submodule)
│   └── devcontainers-*/        #   devcontainers org upstream sources (7 submodules)
├── scripts/                    # bdfs CLI, mirror, sync, and validation scripts
├── tests/                      # Integration and unit tests
├── userspace/                  # FUSE daemon and socket layer
└── lkm/                        # Linux kernel module (bdfs_blend)
```
<!-- AI:end:architecture -->

## Integrations

Each subdirectory under `integrations/` bridges bdfs with a specific ecosystem.

### bdfs integration scripts

| Directory | CLI | What it does |
|---|---|---|
| [`integrations/ostree/`](integrations/ostree/) | `bdfs-ostree` | Commit bdfs workspaces to an OSTree repo, deploy as next boot target, round-trip through DwarFS images. Includes systemd units for auto-pruning old deployments. |
| [`integrations/bootc/`](integrations/bootc/) | `bdfs-bootc` | Create bdfs workspaces from a live bootc root, pack them back into OCI images via podman, switch/upgrade the booted image, export root as DwarFS. |
| [`integrations/incus-os/`](integrations/incus-os/) | `bdfs-incusos` | Create bdfs workspaces from a live IncusOS root, export as DwarFS, import DwarFS archives as Incus container/VM images, trigger in-place updates. |
| [`integrations/devcontainer/`](integrations/devcontainer/) | `bdfs-devcontainer` | Snapshot running dev containers into bdfs workspaces, export/import via DwarFS for offline distribution, wrap `devcontainer up` with pre-snapshots. |

### Upstream submodules

These track upstream source repos and feed the GitLab mirror pipeline:

| Directory | Upstream | Description |
|---|---|---|
| `integrations/ostree-upstream/` | [ostreedev/ostree](https://github.com/ostreedev/ostree) | OSTree — OS and container binary deployment |
| `integrations/bootc-upstream/` | [bootc-dev/bootc](https://github.com/bootc-dev/bootc) | bootc — OCI-container-as-OS tooling |
| `integrations/incus-os-upstream/` | [lxc/incus-os](https://github.com/lxc/incus-os) | IncusOS — immutable OS for running Incus |
| `integrations/ashos/` | [openos-project/ashos](https://gitlab.com/openos-project/linux-kernel_filesystem_deving/ashos) | AshOS immutable distro |
| `integrations/btrfs-assistant/` | [openos-project/btrfs-assistant](https://gitlab.com/openos-project/linux-kernel_filesystem_deving/btrfs-assistant) | BTRFS management GUI |
| `integrations/btr-fs-git/` | [openos-project/btr-fs-git](https://gitlab.com/openos-project/linux-kernel_filesystem_deving/btr-fs-git) | Git-on-BTRFS tooling |
| `integrations/frzr-meta-root/` | [openos-project/frzr-meta-root](https://gitlab.com/openos-project/linux-kernel_filesystem_deving/frzr-meta-root) | frzr immutable root |
| `integrations/gitlab-enhanced/` | [openos-project/gitlab-enhanced](https://gitlab.com/openos-project/git-management_deving/gitlab-enhanced) | GitLab workflow tooling |
| `integrations/devcontainers-spec/` | [devcontainers/spec](https://github.com/devcontainers/spec) | Dev Container specification |
| `integrations/devcontainers-features/` | [devcontainers/features](https://github.com/devcontainers/features) | Official Dev Container Features |
| `integrations/devcontainers-cli/` | [devcontainers/cli](https://github.com/devcontainers/cli) | Reference CLI implementation |
| `integrations/devcontainers-templates/` | [devcontainers/templates](https://github.com/devcontainers/templates) | Official Dev Container Templates |
| `integrations/devcontainers-images/` | [devcontainers/images](https://github.com/devcontainers/images) | Pre-built dev container images |
| `integrations/devcontainers-action/` | [devcontainers/action](https://github.com/devcontainers/action) | GitHub Action for publishing features/templates |
| `integrations/devcontainers-ci/` | [devcontainers/ci](https://github.com/devcontainers/ci) | GitHub Action / Azure DevOps Task for CI |

## Install

<!-- Add installation instructions here. This section is yours — the AI will not modify it. -->

```bash
git clone https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework.git
cd btrfs-dwarfs-framework
```

## Usage


### 1. Start the daemon

```bash
sudo systemctl start bdfs_daemon
# or in the foreground for debugging:
sudo bdfs_daemon -f -v
```

### 2. Register partitions

```bash
# DwarFS-backed: stores BTRFS snapshots as compressed images
bdfs partition add \
    --type dwarfs-backed \
    --device /dev/sdb1 \
    --label archive \
    --mount /mnt/archive

# BTRFS-backed: stores DwarFS image files with CoW + checksums
bdfs partition add \
    --type btrfs-backed \
    --device /dev/sdc1 \
    --label images \
    --mount /mnt/images
```

### 3. Export a BTRFS subvolume to a DwarFS image

```bash
# Find the subvolume ID
btrfs subvolume list /mnt/data

# Export it (creates a read-only snapshot, runs mkdwarfs, cleans up)
bdfs export \
    --partition <dwarfs-backed-uuid> \
    --subvol-id 256 \
    --btrfs-mount /mnt/data \
    --name myapp_v1 \
    --compression zstd \
    --verify
```

### 4. Mount a DwarFS image

```bash
bdfs mount \
    --partition <dwarfs-backed-uuid> \
    --image-id 1 \
    --mountpoint /mnt/myapp_v1 \
    --cache-mb 512
```

### 5. Import a DwarFS image into a BTRFS subvolume

```bash
bdfs import \
    --partition <btrfs-backed-uuid> \
    --image-id 1 \
    --btrfs-mount /mnt/data \
    --subvol-name myapp_restored
```

### 6. Snapshot the BTRFS container of a DwarFS image

```bash
# Point-in-time CoW snapshot of the subvolume holding the image file
bdfs snapshot \
    --partition <btrfs-backed-uuid> \
    --image-id 1 \
    --name images_snap_20250101 \
    --readonly
```

### 7. Mount the blend namespace

```bash
# Kernel blend (requires bdfs_blend module)
bdfs blend mount \
    --btrfs-uuid <uuid> \
    --dwarfs-uuid <uuid> \
    --mountpoint /mnt/blend

# Userspace blend via fuse-overlayfs (no kernel module needed)
bdfs blend mount \
    --btrfs-uuid <uuid> \
    --dwarfs-uuid <uuid> \
    --mountpoint /mnt/blend \
    --userspace
```

### 8. Promote / demote

```bash
# Promote: make a DwarFS-backed path writable (extract to BTRFS subvolume)
bdfs promote \
    --blend-path /mnt/blend/myapp \
    --subvol-name myapp_live

# Demote: compress a BTRFS subvolume to DwarFS and reclaim space
bdfs demote \
    --blend-path /mnt/blend/myapp_live \
    --image-name myapp_archived \
    --compression zstd \
    --delete-subvol
```

### 9. Prune snapshots

```bash
# Keep 5 most recent, archive older ones as DwarFS before deleting
bdfs snapshot prune /mnt/data --keep 5 --demote-first

# Preview without making changes
bdfs snapshot prune /mnt/data --keep 5 --dry-run
```

### 10. Home directory snapshots

```bash
bdfs home init /home/alice
bdfs home snapshot /home/alice
bdfs home demote /home/alice
```

### 11. Distro-agnostic setup

```bash
# Generate /etc/fstab from live btrfs subvolume introspection
bdfs setup fstab

# Verify setup health
bdfs setup check

# Install weekly scrub + monthly balance timers
sudo bash boot/install.sh --maintenance
```

### Status

```bash
bdfs status
bdfs status --json
```

---

## Configuration

<!-- Document configuration options here. This section is yours — the AI will not modify it. -->

## CI

<!-- AI:start:ci -->
The repository uses GitHub Actions for continuous integration and automation. Below are the key workflows and their purposes:

- **build.yml**: Builds the project for all supported architectures. No secrets required.
- **ci.yml**: Runs tests, linting, and static analysis. No secrets required.
- **build-x86.yml**: Builds the project specifically for x86 architecture. No secrets required.
- **build-arm64.yml**: Builds the project specifically for ARM64 architecture. No secrets required.
- **cleanup-branches.yml**: Deletes stale branches. Requires `GITHUB_TOKEN`.
- **mirror-artifacts.yml**: Mirrors build artifacts to external storage. Requires `ARTIFACT_STORAGE_KEY`.
- **release.yaml**: Handles release creation and tagging. Requires `GITHUB_TOKEN`.
- **rotate-token.yml**: Rotates API tokens for external integrations. Requires `ADMIN_TOKEN`.
- **sync-to-gitlab.yml**: Syncs repository changes to GitLab. Requires `GITLAB_TOKEN`.
- **update-readmes.yml**: Updates README files across repositories. No secrets required.

Refer to `.github/workflows/` for additional workflows and their configurations.
<!-- AI:end:ci -->

## Mirror chain

<!-- AI:start:mirror-chain -->
This repo is maintained in [`Interested-Deving-1896/btrfs-dwarfs-framework`](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework) and mirrored through:

```
Interested-Deving-1896/btrfs-dwarfs-framework  ──►  OpenOS-Project-OSP/btrfs-dwarfs-framework  ──►  OpenOS-Project-Ecosystem-OOC/btrfs-dwarfs-framework
```

Changes flow downstream automatically via the hourly mirror chain in
[`fork-sync-all`](https://github.com/Interested-Deving-1896/fork-sync-all).
Direct commits to OSP or OOC are detected and opened as PRs back to `Interested-Deving-1896`.
<!-- AI:end:mirror-chain -->

## Contributors

<!-- AI:start:contributors -->
- [@Interested-Deving-1896](https://github.com/Interested-Deving-1896): 155 commits  
- [@ona-agent](https://github.com/ona-agent): 1 commit  

*Note: This repository is a mirror. Please refer to the upstream source for the original project.*
<!-- AI:end:contributors -->

## Origins

<!-- AI:start:origins -->
_Original project — no upstream fork._
<!-- AI:end:origins -->

## Resources

<!-- AI:start:resources -->
_No additional resource files found._
<!-- AI:end:resources -->

## License

<!-- AI:start:license -->
[MIT](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/blob/main/LICENSE) © 2026 [Interested-Deving-1896](https://github.com/Interested-Deving-1896)
<!-- AI:end:license -->
