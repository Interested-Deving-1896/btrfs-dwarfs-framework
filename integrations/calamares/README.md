# Calamares ↔ BDFS integration

This directory defines the BDFS-side integration contract for Calamares. Calamares is a distribution-independent Linux installer; BDFS is a filesystem, artifact, and environment framework. The integration is deliberately an adapter boundary, not a claim that Calamares already understands BDFS filesystems.

## Upstream status

The historical GitHub repository [calamares/calamares](https://github.com/calamares/calamares) is archived and its README directs contributors to the maintained Codeberg project: [Calamares on Codeberg](https://codeberg.org/Calamares/calamares). Do not base new development on the archived GitHub default branch. Pin a reviewed Codeberg commit before building or packaging any module. This repository does not vendor or modify Calamares upstream.

### Import through Fork-Sync-All

Use the existing [Fork-Sync-All importer](https://github.com/Interested-Deving-1896/fork-sync-all/blob/main/scripts/import-repo.sh) for the Codeberg source instead of inventing a second importer. Its workflow is [Import Repository](https://github.com/Interested-Deving-1896/fork-sync-all/blob/main/.github/workflows/import-repo.yml). The intended source URL is `https://codeberg.org/Calamares/calamares`; use the distinct target name `calamares-upstream`, because `Interested-Deving-1896/calamares` already exists as a Rhino Linux Calamares-settings fork and must not be overwritten. For the initial import, keep `mirror_to_osp_ooc=false` and `ongoing_sync=false`: the source audit and previously deferred mirror rollout must finish before enabling propagation or scheduled synchronization. The importer uses a mirror push, so it is appropriate only for the new, empty `calamares-upstream` destination; never point it at the existing settings fork or another repository containing independent work. After import, record and review the exact Codeberg commit and imported GitHub commit before wiring it into builds.

Calamares supports distribution-specific configuration, branding, and external modules. Its documented extension points include Python job modules (with a `module.desc` and `main.py` entry point) and Qt plugin modules for user-facing view steps. The preferred first BDFS module should be a Python job module for non-interactive, read-only manifest/preflight work; add a C++/Qt view step only if a user-facing BDFS storage-selection screen is genuinely needed. Avoid Calamares process modules and shell command strings for privileged operations. The intended BDFS integration is an optional external Calamares module plus a BDFS-side adapter/CLI contract. Keep the two projects independently buildable and versioned.

## Two directions

### Calamares consuming BDFS

A future optional Calamares module may:
- discover and validate BDFS artifact manifests and supported BDFS storage capabilities;
- show read-only compatibility and preflight information before installation;
- hand off explicitly selected storage plans to a separately reviewed BDFS adapter;
- report operation results and provenance to the installer session.

It must not silently format, repartition, mount, convert, or overwrite disks. Partitioning and filesystem mutation remain explicit Calamares/BDFS job steps requiring the installer's selected target, validated plan, and confirmation. The module must not execute arbitrary commands or hooks supplied by an imported artifact.

### BDFS invoking Calamares

BDFS may offer Calamares as an optional installer frontend for a configured OS environment. BDFS should generate or validate a Calamares configuration/profile and launch the installer only in an explicitly authorized live-install session. It must not treat launching Calamares as an artifact import/export operation, and must not run it during ordinary BDFS inventory, import, export, or CI tests.

## Shared artifact and storage contract

Use the versioned BDFS artifact manifest from `doc/schemas/bdfs-artifact-manifest.schema.json` when available. Exchange stable identifiers, filesystem and subvolume metadata, checksums, provenance, compatibility findings, and declared capabilities—not opaque host paths or executable scripts.

Initial capability state is **contract-only / not yet implemented**:
- manifest discovery and validation: planned;
- read-only storage capability/preflight: planned;
- BDFS-aware Calamares module: planned;
- BDFS installer profile/launcher: planned;
- disk mutation, partitioning, formatting, bootloader operations, and automatic conversion: disabled by this contract.

## Safety invariants

- Default mode is read-only discovery and validation.
- No host mutation, privileged command, disk write, mount, filesystem conversion, or bootloader action is enabled by this integration metadata.
- Any future write operation must use a typed operation plan, exact target-device identity, capacity and compatibility checks, an explicit user confirmation, and a separately tested privileged helper.
- Never infer that Btrfs or DwarFS support exists merely because Calamares can display a device or because a distribution ships a Calamares module.
- Preserve existing filesystem metadata and report unsupported/lossy transformations rather than silently dropping them.
- Test with disposable virtual disks/VMs; never run destructive installer tests against CI hosts or production devices.

## Multi-distribution support

Calamares is distribution-independent, but its modules and package hooks are distribution-specific. BDFS should select an OS environment profile first, then validate that profile's Calamares configuration and available package backend. KDE Neon Editions can be the default BDFS environment target where configured; Arch, Gentoo, Debian/Ubuntu-derived and other profiles remain separately selectable. No distribution is declared compatible by registration alone.

## Implementation status

This change establishes a machine-readable integration declaration and validation boundary. It does **not** yet provide a compiled or installable Calamares module, a BDFS storage backend, an installer launcher, or a working disk-image round trip. Those require a pinned maintained Calamares source revision, a module built and exercised with Calamares' `loadmodule` test executable, a defined typed BDFS CLI/library interface, and isolated end-to-end installer tests.
