# BTRFS+DwarFS Framework — Architecture

## Overview

The framework blends two complementary filesystem technologies into a single
coherent system:

- **BTRFS** ([kdave/btrfs-devel](https://github.com/kdave/btrfs-devel)):
  mutable, Copy-on-Write block filesystem with subvolumes, snapshots,
  per-block checksumming, and transparent compression.

- **DwarFS** ([mhx/dwarfs](https://github.com/mhx/dwarfs)):
  read-only, highly-compressed FUSE filesystem stored as a single image
  file. Achieves 10–16× compression on redundant data via similarity
  hashing and cross-file segmentation analysis.

The combination gives you:

| Workload | Tier | Technology |
|---|---|---|
| Active, writable data | Upper | BTRFS subvolume |
| Archived, read-mostly data | Lower | DwarFS image |
| Image storage with CoW | Container | BTRFS holds `.dwarfs` files |
| Snapshot of image collection | Container snapshot | BTRFS snapshot |

---

## Partition Types

### DwarFS-backed Partition

A DwarFS-backed partition stores BTRFS subvolumes and snapshots as compressed
DwarFS image files. This is the primary archival tier.

```
┌─────────────────────────────────────────┐
│         DwarFS-backed Partition         │
│                                         │
│  ┌──────────────┐  ┌──────────────┐    │
│  │ snap_v1.dwarfs│  │ snap_v2.dwarfs│   │
│  │  (BTRFS snap) │  │  (BTRFS snap) │   │
│  └──────────────┘  └──────────────┘    │
│                                         │
│  Each .dwarfs file is a self-contained  │
│  compressed image of a BTRFS subvolume  │
└─────────────────────────────────────────┘
```

**Export pipeline** (BTRFS subvolume → DwarFS image):

1. Kernel module receives `BDFS_IOC_EXPORT_TO_DWARFS` ioctl.
2. Emits `BDFS_EVT_SNAPSHOT_EXPORTED` netlink event.
3. Daemon creates a read-only BTRFS snapshot of the source subvolume.
4. Runs `btrfs send | btrfs receive` to extract a clean POSIX tree.
5. Runs `mkdwarfs` on the extracted tree to produce the `.dwarfs` image.
6. Atomically renames the image to its final backing path.
7. Cleans up the temporary snapshot and extracted tree.

### BTRFS-backed Partition

A BTRFS-backed partition is a live BTRFS filesystem that stores DwarFS image
files as regular files. This gives images:

- **CoW semantics**: no partial-write corruption; interrupted copies leave
  the original intact.
- **Per-file checksumming**: BTRFS verifies every data block on read.
- **Snapshot capability**: point-in-time copies of entire image collections
  via `btrfs subvolume snapshot`.
- **Transparent compression**: BTRFS can compress image metadata (though
  DwarFS images are already compressed internally).

```
┌─────────────────────────────────────────────┐
│           BTRFS-backed Partition             │
│                                             │
│  /images/  (BTRFS subvolume)                │
│    ├── app_v1.dwarfs                        │
│    ├── app_v2.dwarfs                        │
│    └── libs.dwarfs                          │
│                                             │
│  /images_snap_20250101/  (BTRFS snapshot)   │
│    ├── app_v1.dwarfs  ──CoW──┐              │
│    ├── app_v2.dwarfs  ──CoW──┤  shared      │
│    └── libs.dwarfs    ──CoW──┘  extents     │
└─────────────────────────────────────────────┘
```

**Import pipeline** (DwarFS image → BTRFS subvolume):

1. Kernel module receives `BDFS_IOC_IMPORT_FROM_DWARFS` ioctl.
2. Emits `BDFS_EVT_IMAGE_IMPORTED` netlink event.
3. Daemon creates a new BTRFS subvolume at the target path.
4. Runs `dwarfsextract` to populate the subvolume.
5. Optionally marks the subvolume read-only.

---

## Blend Layer

The blend layer merges a BTRFS upper partition and one or more DwarFS lower
partitions into a single coherent namespace. It is implemented as a stackable
VFS layer registered as filesystem type `bdfs_blend`.

```
┌──────────────────────────────────────────────────────┐
│                  Blend Namespace                     │
│                  /mnt/blend/                         │
│                                                      │
│   READ:  BTRFS upper first → DwarFS lower fallback   │
│   WRITE: always to BTRFS upper (copy-up on demand)   │
└──────────────┬───────────────────────┬───────────────┘
               │                       │
    ┌──────────▼──────────┐  ┌─────────▼──────────────┐
    │   BTRFS Upper Layer │  │  DwarFS Lower Layer(s)  │
    │   (writable, live)  │  │  (read-only, compressed)│
    │                     │  │                         │
    │  /mnt/btrfs/upper/  │  │  /mnt/dwarfs/image.dwarfs│
    └─────────────────────┘  └─────────────────────────┘
```

### Promote / Demote

**Promote** (`bdfs promote`): Extract a DwarFS-backed path into a new writable
BTRFS subvolume. The DwarFS lower layer remains until explicitly removed.
Use this to make archived data writable again.

**Demote** (`bdfs demote`): Compress a BTRFS subvolume into a DwarFS image
and optionally delete the subvolume. Use this to archive live data and
reclaim BTRFS space.

---

## Component Map

```
┌─────────────────────────────────────────────────────────────────┐
│                        Kernel Space                             │
│                                                                 │
│  ┌──────────────────────────────────────────────────────────┐  │
│  │                  btrfs_dwarfs.ko                         │  │
│  │                                                          │  │
│  │  bdfs_main.c        — module init, /dev/bdfs_ctl, netlink│  │
│  │  bdfs_blend.c       — bdfs_blend VFS type, blend ioctls  │  │
│  │  bdfs_btrfs_part.c  — BTRFS-backed partition backend     │  │
│  │  bdfs_dwarfs_part.c — DwarFS-backed partition backend    │  │
│  └──────────────────────────────────────────────────────────┘  │
│                          │ netlink events                       │
│                          │ /dev/bdfs_ctl ioctls                 │
└──────────────────────────┼──────────────────────────────────────┘
                           │
┌──────────────────────────┼──────────────────────────────────────┐
│                   Userspace                                     │
│                          │                                      │
│  ┌───────────────────────▼──────────────────────────────────┐  │
│  │                   bdfs_daemon                            │  │
│  │                                                          │  │
│  │  bdfs_daemon.c   — lifecycle, worker pool, main loop     │  │
│  │  bdfs_netlink.c  — netlink event listener                │  │
│  │  bdfs_jobs.c     — job handlers (export/import/mount...) │  │
│  │  bdfs_exec.c     — mkdwarfs/dwarfs/btrfs tool wrappers   │  │
│  │  bdfs_socket.c   — Unix socket server for CLI            │  │
│  └──────────────────────────────────────────────────────────┘  │
│                          │ Unix socket                          │
│  ┌───────────────────────▼──────────────────────────────────┐  │
│  │                      bdfs CLI                            │  │
│  │                                                          │  │
│  │  bdfs partition add/remove/list/show                     │  │
│  │  bdfs export / import                                    │  │
│  │  bdfs mount / umount                                     │  │
│  │  bdfs snapshot / promote / demote                        │  │
│  │  bdfs blend mount / umount                               │  │
│  │  bdfs status                                             │  │
│  └──────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Data Flow: Export (BTRFS → DwarFS)

```
bdfs export --partition <uuid> --subvol-id <id> --name snap_v3
     │
     ▼
BDFS_IOC_EXPORT_TO_DWARFS  →  /dev/bdfs_ctl
     │
     ▼  (kernel allocates image entry, emits netlink event)
bdfs_daemon receives BDFS_EVT_SNAPSHOT_EXPORTED
     │
     ├─ btrfs subvolume snapshot -r <subvol> <tmp_snap>
     │
     ├─ btrfs send <tmp_snap>  ──pipe──▶  btrfs receive <extract_dir>
     │
     ├─ mkdwarfs -i <extract_dir> -o <image.dwarfs.tmp>
     │           --compression zstd --categorize
     │
     ├─ rename(<image.dwarfs.tmp>, <backing_path/snap_v3.dwarfs>)
     │
     └─ cleanup: delete tmp_snap, rm -rf extract_dir
```

## Data Flow: Import (DwarFS → BTRFS)

```
bdfs import --partition <uuid> --image-id <id> --subvol-name restored
     │
     ▼
BDFS_IOC_IMPORT_FROM_DWARFS  →  /dev/bdfs_ctl
     │
     ▼  (kernel allocates subvol entry, emits netlink event)
bdfs_daemon receives BDFS_EVT_IMAGE_IMPORTED
     │
     ├─ btrfs subvolume create <btrfs_mount>/restored
     │
     └─ dwarfsextract -i <image.dwarfs> -o <btrfs_mount>/restored
```

---

## Build

```bash
# Kernel module (requires kernel headers)
make kernel KDIR=/lib/modules/$(uname -r)/build

# Userspace (daemon + CLI)
make userspace

# Both
make all

# Install
sudo make install

# Run integration tests (requires root, btrfs-progs, dwarfs)
sudo make test
```

## Dependencies

| Component | Dependency | Purpose |
|---|---|---|
| Kernel module | Linux ≥ 5.15, BTRFS, FUSE | VFS integration |
| Daemon | pthreads, libc | Worker pool, netlink |
| CLI | libc | ioctl interface |
| Export | mkdwarfs | DwarFS image creation |
| Mount | dwarfs (FUSE3) | DwarFS image mounting |
| Import | dwarfsextract | DwarFS image extraction |
| Verify | dwarfsck | Image integrity checking |
| BTRFS ops | btrfs-progs | send/receive/snapshot |
