# Nextcloud All-in-One integration

This integration documents how to host **Nextcloud All-in-One (AIO)** on a
BDFS-managed Linux system. It is an optional userspace/container deployment
profile; it does not add Nextcloud code to the BDFS kernel module or change the
system boot path.

Upstream project: [nextcloud/all-in-one](https://github.com/nextcloud/all-in-one)
Maintained mirror: [Interested-Deving-1896/All-In-One-NextCloud-Installer](https://github.com/Interested-Deving-1896/All-In-One-NextCloud-Installer)

The maintained repository is a fork of the official Nextcloud AIO project and
is licensed AGPL-3.0. Keep its upstream provenance and license notices intact.
Prefer the official `nextcloud/all-in-one` project as the canonical source of
release information; treat the maintained fork as a mirror/working copy, not
as an independent upstream.

## Integration boundary

- Nextcloud AIO remains responsible for its supported container orchestration,
  service lifecycle, updates, and its optional BorgBackup workflow.
- BDFS may provide the host filesystem substrate for persistent data and
  versioned host-side recovery points.
- Container images, Docker/Podman runtime requirements, networking, reverse
  proxy configuration, TLS, and AIO's supported backup/restore procedures
  remain governed by the AIO upstream documentation.
- No kernel module, privileged mount, automatic network exposure, or default
  service enablement is introduced by this documentation.

## Suggested storage layout

Use dedicated Btrfs subvolumes for host-managed persistent paths where the
chosen AIO deployment method supports host paths. Keep the AIO mastercontainer
configuration volume and the Nextcloud data directory at stable locations.
AIO explicitly warns that changing `NEXTCLOUD_DATADIR` after initial
installation is not supported as a routine relocation procedure.

Example layout (illustrative; adapt to the deployment's actual volume names):

```text
/srv/nextcloud-aio/
  mastercontainer/   # AIO configuration volume; preserve its expected path
  data/              # Nextcloud datadir, if configured as a host path
  backups/           # Separate backup target; preferably separate storage
```

Do not place the live database, data directory, and AIO configuration into an
arbitrary single read-only DwarFS image. They are mutable service state.

## Snapshot and backup safety

1. Use AIO's supported backup/restore mechanism (BorgBackup when enabled) as
   the application-consistent recovery path.
2. Treat BDFS/Btrfs snapshots as an additional host-level recovery layer, not
   a replacement for AIO backups.
3. Do not snapshot a running database and claim application consistency.
   Coordinate a maintenance window or use documented database/application
   quiescing hooks before taking a multi-volume recovery snapshot.
4. Keep backup data off the same failure domain as the primary data.
5. Test restore into a clean AIO instance before relying on a snapshot set.
6. Never prune a BDFS snapshot that is still referenced by a backup or recovery
   procedure.

## Security considerations

AIO's compose configuration mounts the Docker socket into the mastercontainer
(the upstream comments document supported alternatives). Access to a Docker
socket is effectively host-level control. Keep the socket read-only where
upstream supports it, restrict access to trusted administrators, and do not
expose the AIO management interface publicly without following AIO's reverse
proxy and TLS guidance.

Do not hard-code credentials, domain names, tokens, or private paths into BDFS
profiles. Do not automatically open ports or alter firewall policy.

## KDE Neon relationship

KDE Neon Editions can offer a discoverable optional launcher/documentation
entry for administrators to deploy or open a separately managed AIO instance.
The desktop edition must not silently install a server, enable containers,
create a public endpoint, or assume that the desktop is an always-on server.

## Validation before automation

A future executable helper should not be enabled until it has tests for:

- supported container runtime and AIO version detection;
- persistent-volume mapping without changing AIO's required container names,
  volume semantics, or configuration paths;
- Btrfs subvolume and snapshot capability detection;
- database-aware backup/snapshot coordination;
- restore verification and failure rollback;
- least-privilege handling of Docker socket and service credentials;
- clean operation when BDFS is unavailable (AIO remains independently usable).

This document is an integration design, not an installer or a claim that
Nextcloud AIO has been tested on every BDFS-supported distribution.
