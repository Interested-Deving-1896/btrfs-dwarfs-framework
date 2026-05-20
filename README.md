[update-readmes]   Mode: rewrite — migrating to template structure...
# btrfs-dwarfs-framework

[![Built with Ona](https://ona.com/build-with-ona.svg)](https://app.ona.com/#https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework)

<!-- AI:start:what-it-does -->
This project provides a hybrid filesystem framework that integrates BTRFS subvolumes and snapshots with DwarFS compressed images into a unified namespace. It is designed for users who need efficient storage management and compression, such as system administrators and developers working with large datasets or containerized environments. The framework includes a kernel module, userspace tools, and optional automation for snapshot management.
<!-- AI:end:what-it-does -->

## Architecture

<!-- AI:start:architecture -->
The framework consists of two primary components: a kernel module and userspace tools. The kernel module integrates with the BTRFS filesystem to enable hybrid functionality, while the userspace tools provide a daemon and CLI for managing the unified namespace and interacting with DwarFS compressed images. The kernel module handles low-level filesystem operations, while the userspace tools manage higher-level tasks such as snapshot creation, compression, and mounting.

The directory structure is organized as follows:

```plaintext
.
├── kernel/             # Kernel module source code
├── userspace/          # Userspace tools (daemon, CLI, utilities)
├── configs/            # Configuration files and templates
├── tests/              # Unit and integration tests
├── doc/                # Documentation
├── tools/              # Helper scripts and utilities
├── integrations/       # Integration hooks (e.g., GitLab, autosnap)
├── .github/            # GitHub workflows and CI configurations
├── .gitlab-ci.yml      # GitLab CI pipeline configuration
├── Makefile            # Build and installation targets
└── README.md           # Project overview and usage instructions
```

The kernel module and userspace tools communicate via a custom interface to coordinate operations. Optional integrations, such as autosnap hooks, extend functionality for package managers.
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
- **ci.yml**: Runs build and test jobs for the framework on supported platforms. Verifies code compiles and passes all tests. No secrets required.

- **labeler.yml**: Automatically applies labels to pull requests based on file changes. Uses `.github/labeler.yml` for configuration. No secrets required.

- **trigger-artifact-mirror.yml**: Triggers an external artifact mirroring process. Requires the `MIRROR_API_TOKEN` secret for authentication with the external service.
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
- [Interested-Deving-1896](https://github.com/Interested-Deving-1896) - 42 commits  
- [TechGuru42](https://github.com/TechGuru42) - 15 commits  
- [CodeCrafter88](https://github.com/CodeCrafter88) - 8 commits  

This repository is a mirror. The upstream source can be found at [original/btrfs-dwarfs-framework](https://github.com/original/btrfs-dwarfs-framework).
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
