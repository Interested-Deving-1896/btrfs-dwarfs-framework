# fwupd Integration

Bridges [fwupd](https://fwupd.org)'s firmware update lifecycle with
btrfs-dwarfs-framework's snapshot and workspace capabilities.

Firmware updates are one of the highest-risk operations on a Linux system —
a failed UEFI capsule update or bad NVMe firmware can leave a machine
unbootable. This integration adds a safety layer: snapshot before every
update, rollback if something goes wrong, and DwarFS archival of firmware
state for audit and recovery.

## What this provides

| Script | Purpose |
|---|---|
| `bdfs-fwupd.sh` | CLI: snapshot, update, rollback, export, status, workspace, audit |
| `bdfs-fwupd.conf` | Site configuration (copy to `/etc/bdfs/fwupd.conf`) |

## How it fits together

```
LVFS (Linux Vendor Firmware Service)
      │
      └──fwupdmgr refresh──► local metadata cache
                                    │
                                    └──fwupdmgr update
                                              │
                              ┌───────────────┴──────────────────┐
                              │                                  │
                    bdfs snapshot (pre-update)          firmware capsule staged
                    "fwupd-pre-update-{ts}"             for next boot
                              │
                    ┌─────────┴──────────┐
                    │                   │
              update succeeds      update fails
                    │                   │
              prune old           bdfs-fwupd rollback
              snapshots           restores pre-update state
```

## Safe update flow

```bash
# The recommended way — snapshots automatically, then updates
sudo bdfs-fwupd update

# Check what would be updated without applying
sudo bdfs-fwupd update --check

# If something goes wrong after reboot
sudo bdfs-fwupd rollback
```

## Manual snapshot

```bash
# Snapshot before any manual firmware operation
sudo bdfs-fwupd snapshot --name before-bios-update
```

## DwarFS firmware archive

```bash
# Export current firmware state (device list, capsules, history)
sudo bdfs-fwupd export --out /backup/firmware-$(date +%Y%m%d).dwarfs

# Full audit trail
sudo bdfs-fwupd audit --out-dir /backup/firmware-audit/
```

## Test workspace

```bash
# Create an isolated workspace to test firmware update flows
sudo bdfs-fwupd workspace --name fwupd-test
sudo bdfs workspace enter fwupd-test
# Run fwupdmgr commands here — changes are isolated
# Exit without committing to discard
```

## Status

```bash
bdfs-fwupd status
# Shows: fwupd device list, pending updates, active bdfs snapshots
```

## Install

```bash
install -m 755 bdfs-fwupd.sh /usr/local/bin/bdfs-fwupd
install -m 644 bdfs-fwupd.conf /etc/bdfs/fwupd.conf
```

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `BDFS_FWUPD_SNAPSHOT_PREFIX` | `fwupd` | Prefix for auto-named snapshots |
| `BDFS_FWUPD_EXPORT_DIR` | `/var/lib/bdfs/fwupd` | DwarFS archive output directory |
| `BDFS_FWUPD_KEEP_SNAPSHOTS` | `3` | Number of pre-update snapshots to retain |
| `BDFS_FWUPD_COMPRESSION` | `zstd` | DwarFS compression for exports |

## Dependencies

- [fwupd](https://fwupd.org) — firmware update daemon
- [btrfs-dwarfs-framework](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework) — snapshot + workspace engine
- [dwarfs](https://github.com/mhx/dwarfs) — required for `export` and `audit` commands only

## Related fwupd org projects

The [fwupd org](https://github.com/fwupd) maintains several related projects
tracked as consumers in this framework:

| Repo | Role |
|---|---|
| [fwupd/fwupd](https://github.com/fwupd/fwupd) | Core firmware update daemon |
| [fwupd/fwupd-efi](https://github.com/fwupd/fwupd-efi) | EFI update helper |
| [fwupd/fwupd-signed](https://github.com/fwupd/fwupd-signed) | Signed EFI binaries |
| [fwupd/dbx-firmware](https://github.com/fwupd/dbx-firmware) | UEFI Secure Boot DBX updates |
| [fwupd/fwupd-snap-deb](https://github.com/fwupd/fwupd-snap-deb) | Snap/deb packaging |
