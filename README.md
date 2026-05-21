[update-readmes]   Mode: rewrite — migrating to template structure...
# btrfs-dwarfs-framework

[![Built with Ona](https://ona.com/build-with-ona.svg)](https://app.ona.com/#https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework)

<!-- AI:start:what-it-does -->
This project provides a hybrid filesystem framework that integrates BTRFS subvolumes and snapshots with DwarFS compressed images into a unified namespace. It addresses the need for efficient storage and retrieval of large datasets while maintaining snapshot capabilities. It is used by system administrators and developers managing complex storage environments.
<!-- AI:end:what-it-does -->

## Architecture

<!-- AI:start:architecture -->
The BTRFS+DwarFS framework consists of two primary components: a kernel module and userspace tools. The kernel module integrates with the BTRFS filesystem to manage subvolumes and snapshots. Userspace tools include a daemon and CLI utilities for managing DwarFS compressed images and providing a unified namespace. The framework also includes optional integration hooks, such as the autosnap package-manager hook for automated snapshot creation.

The directory structure is organized as follows:

```plaintext
.
├── kernel/          # Kernel module source code
├── userspace/       # Userspace tools (daemon, CLI)
├── integrations/    # Optional integrations (e.g., GitLab, autosnap)
├── scripts/         # Helper scripts for build and deployment
├── configs/         # Configuration files for runtime behavior
├── tests/           # Unit and integration tests
├── doc/             # Documentation
├── tools/           # Additional utilities
├── LICENSE          # License file
├── Makefile         # Build and installation instructions
└── README.md        # Project documentation
```

The kernel module communicates with the userspace daemon via a custom interface to synchronize BTRFS subvolume operations with DwarFS image management. The userspace tools handle mounting, unmounting, and namespace operations. Optional integrations, such as autosnap, extend functionality for specific use cases.
<!-- AI:end:architecture -->

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
- **ci.yml**: Runs unit tests (`make check`) and integration tests (`make test`) on supported platforms. No secrets required.
- **cleanup-branches.yml**: Deletes stale branches in the repository. Requires `GITHUB_TOKEN` secret.
- **mirror-artifacts.yml**: Syncs build artifacts to external storage. Requires `ARTIFACT_STORAGE_KEY` and `ARTIFACT_STORAGE_SECRET` secrets.
- **rate-limit-status.yml**: Monitors API rate limits and logs usage. No secrets required.
- **rotate-token.yml**: Rotates access tokens for external integrations. Requires `ADMIN_TOKEN` secret.
- **sync-btrfs-devel-branches.yml**: Syncs development branches with upstream repositories. Requires `GITHUB_TOKEN` secret.
- **validate-config.yml**: Validates YAML configuration files in the repository. No secrets required.
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
| File | Description |
|---|---|
| [.gitlab/merge_request_templates/Default.md](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/blob/main/.gitlab/merge_request_templates/Default.md) | GitLab MR template |
| [config/gitlab-subgroups.yml](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/blob/main/config/gitlab-subgroups.yml) | GitLab subgroup map |
<!-- AI:end:resources -->

## License

<!-- AI:start:license -->
[MIT](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/blob/master/LICENSE) © 2026 [Interested-Deving-1896](https://github.com/Interested-Deving-1896)
<!-- AI:end:license -->
