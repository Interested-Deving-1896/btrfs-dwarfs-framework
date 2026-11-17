# BDFS integration adapters

Each adapter in this directory translates between BDFS workspaces/artifacts and an external ecosystem. Adapters retain their ecosystem-specific semantics while sharing a provider-neutral artifact envelope.

## KDE Neon Editions target adapter

[`kde-neon-editions/`](kde-neon-editions/) is the default OS-target integration for BDFS import/export selection. It connects the KDE Neon Editions environment profile to the shared artifact contract. The current adapter documentation defines the boundary and safety requirements; it does not claim that all KDE Neon image formats can already be converted or that a deployment action is available.

## Existing adapters

| Adapter | External system | Existing documented operations |
|---|---|---|
| [devcontainer](devcontainer/) | Dev Container CLI and OCI/container runtime | Snapshot, export, import, build, up, status |
| [ostree](ostree/) | OSTree repositories and deployments | Commit, publish, export, import, prune, status |
| [bootc](bootc/) | bootc and OCI images | Workspace, commit, switch, upgrade, export, status |
| [incus-os](incus-os/) | IncusOS and Incus images | Workspace, export, import, update, status |

Upstream source mirrors/submodules (for example `devcontainers-*`, `ostree-upstream`, `bootc-upstream`, and `incus-os-upstream`) are distinct from BDFS adapter implementations. Do not treat an upstream source checkout as an executable adapter.

## Shared artifact contract

The metadata envelope is specified in [bdfs-artifact-manifest.schema.json](../doc/schemas/bdfs-artifact-manifest.schema.json). The payload remains in its native format (for example DwarFS, OCI, an OSTree commit, or an Incus image); the manifest records its identity, source adapter, platform, digest, compatibility, provenance, capabilities, and conversion warnings.

An adapter should expose the operations it actually supports:

- `inspect`: parse metadata and validate the artifact without executing it;
- `export`: produce a native payload and manifest;
- `import`: validate then materialize into an explicitly chosen target;
- `workspace`: create a writable BDFS workspace when supported;
- `build` / `commit` / `publish`: only where the target ecosystem defines those operations;
- `status`: report adapter and target state.

Adapters must report unsupported directions rather than pretending every format is losslessly interchangeable. OSTree commits, bootc OCI images, Incus images, and Dev Container images have different metadata and activation semantics; a shared envelope does not erase those differences.

## Round-trip rules

A round trip is successful only when both conversions complete and the result is checked. Compare payload digests where byte-identical output is expected; otherwise compare declared filesystem metadata and content manifests, and report expected differences such as ownership mapping, xattrs, sparse files, capabilities, labels, or boot metadata.

Import is not deployment. Publishing, switching, updating, launching, or activating a target remains a separate explicit operation.

## Security baseline

- Validate manifest schema and payload digest before import.
- Reject path traversal, symlink/hardlink escapes, device nodes, malformed archives, and unsupported special files unless a reviewed adapter explicitly supports them.
- Keep imported hooks and entrypoints inert during inspection and import.
- Require explicit destination and overwrite policy.
- Never package secrets, tokens, private keys, or machine identity.
- Do not automatically run imported hooks, deploy images, switch boot targets, update hosts, flash devices, or alter bootloaders.
- Preserve source provenance and licenses; record lossy conversions and omitted metadata.

## Implementation status

These docs establish the shared contract and inventory. They do **not** claim that a common adapter CLI, manifest validator, or cross-adapter round-trip test suite has been implemented yet. Adapter code must be migrated incrementally and tested independently.
