# Incus Engine Subsystem Integration

Connects [Incus Engine Subsystem](https://github.com/Interested-Deving-1896/Incus-Engine-Subsystem) to **btrfs-dwarfs-framework (BDFS)** as an ecosystem integration.

This is the BDFS integration point for the broader cross-platform Incus subsystem. It complements, and does not replace, the existing [IncusOS integration](../incus-os/README.md).

## Scope and ownership

The subsystem registry is authoritative for component identity and repository ownership:

- [`components.yml`](https://github.com/Interested-Deving-1896/Incus-Engine-Subsystem/blob/main/components.yml) — machine-readable component registry.
- [Subsystem architecture](https://github.com/Interested-Deving-1896/Incus-Engine-Subsystem) — shared contracts, compatibility, and end-to-end coordination.

| Component | Platform / scope | Responsibility |
|---|---|---|
| [Incus Linux Toolkit](https://github.com/Interested-Deving-1896/incus-linux-toolkit) | Linux | Linux-specific Incus tooling and guest integration |
| [Incus Waydroid Toolkit](https://github.com/Interested-Deving-1896/incus-waydroid-toolkit) | Android / Waydroid | Android and Waydroid guest integration |
| [Incus macOS Toolkit](https://github.com/Interested-Deving-1896/Incus-MacOS-Toolkit) | macOS | macOS-specific Incus tooling and integration |
| [Incus Windows Toolkit](https://github.com/Interested-Deving-1896/incus-windows-toolkit) | Windows | Windows-specific Incus tooling and integration |
| [Incus Image Server](https://github.com/Interested-Deving-1896/incus-image-server) | Cross-platform | Incus image publication and serving infrastructure |

Component source remains in its own repository and is independently versioned. This integration does not vendor, mirror, or silently merge component source into BDFS.

## BDFS integration responsibilities

BDFS is the filesystem, snapshot, workspace, and DwarFS image layer around Incus-related workloads. Depending on the host and workload, BDFS can provide:

- BTRFS snapshot and workspace operations for host-side data and build trees;
- DwarFS export/import workflows for archival and transfer of filesystem trees;
- a consistent filesystem hand-off to Incus image-building or publication workflows, where the relevant component explicitly supports that hand-off;
- documentation and validation boundaries between filesystem artifacts and Incus images.

The Incus Engine Subsystem remains responsible for Incus platform tooling, guest lifecycle integration, cross-platform interfaces, and image-serving behavior. BDFS does not claim to replace Incus, the platform toolkits, or the image server.

## Relationship to the IncusOS integration

[`integrations/incus-os/`](../incus-os/README.md) is a focused bridge for IncusOS, the immutable operating system dedicated to running Incus. It includes its own BDFS/IncusOS workflow and must remain independently usable.

This directory has a broader scope: it registers and documents the complete Incus Engine Subsystem, including Linux, Android/Waydroid, macOS, Windows, and image infrastructure. The two integrations are complementary:

- Use **IncusOS integration** for IncusOS-specific host operating-system workflows.
- Use **Incus Engine Subsystem integration** for the cross-platform component ecosystem and its shared architecture.

## Artifact and compatibility boundaries

Do not assume that every BDFS artifact is directly bootable as an Incus image. DwarFS archives, BTRFS snapshots, OCI images, and Incus image artifacts are distinct formats with different metadata and lifecycle requirements.

A conversion or publication path must be implemented and tested by the component that owns that boundary. Before automating a hand-off, verify at least:

1. source filesystem and required metadata;
2. target Incus image format, architecture, and profile requirements;
3. ownership, permissions, id mapping, and filesystem features;
4. integrity verification and reproducibility of the produced artifact;
5. compatibility with the target Incus version and platform;
6. publication credentials and destination policy.

No image publication, instance creation, upgrade, or destructive filesystem action is performed merely by registering this integration.

## Discovery and validation

The canonical component list is maintained upstream in `Incus-Engine-Subsystem/components.yml`. Consumers should read that registry rather than maintaining a second manually edited component list. A future automated validator can compare this integration's declared source repository and schema version with the canonical registry without cloning component implementations.

For now, this directory intentionally provides the integration contract and source-of-truth links; it does not invent a `bdfs-incus-engine` command or claim an executable adapter exists. Add executable adapters only alongside tested BDFS and Incus APIs, with explicit dry-run behavior and safe defaults.

## Related documentation

- [BDFS integrations](../README.md)
- [IncusOS integration](../incus-os/README.md)
- [Incus Engine Subsystem](https://github.com/Interested-Deving-1896/Incus-Engine-Subsystem)
- [Incus documentation](https://linuxcontainers.org/incus/docs/)
