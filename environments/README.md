# BDFS environments

This directory contains buildable and reusable environment definitions. It is the **environment/profile side** of BDFS; it is not an integration-adapter registry.

## Current inventory

| Path | Purpose | Current role |
|---|---|---|
| `workspace/Dockerfile` | Ubuntu-based GitLab Enhanced workspace image with BDFS, DwarFS, BFG and runner tooling | Build context / environment image |

The directory contains independently named environment profiles and reusable build contexts. `target-registry.json` selects `kde-neon-editions` as the default for both import and export; additional OS profiles can be enabled alongside it without changing the default. The registry resolver validates selection only—it does not perform conversion or deployment.

## Contract

An environment profile should declare, in documentation or machine-readable metadata:

- stable profile identifier and version;
- supported OS/distribution and CPU architectures;
- build context and reproducible build inputs;
- runtime dependencies and required capabilities;
- how a workspace is created, inspected, snapshotted, and cleaned up;
- the artifact formats it can import and export;
- any conversion loss or unsupported metadata.

Environment definitions must not embed credentials, private keys, access tokens, or machine-specific identity in portable outputs.

## Default operating-system target

The default import and export target is [`kde-neon-editions`](kde-neon-editions/). The default is a configurable selection policy, not an exclusive OS lock-in. Add another OS as a separate profile and register it in `target-registry.json`; select it explicitly per operation or make it available as an additional configured target. Do not infer compatibility or conversion support merely from registration.

The target registry is consumed by [`resolve_target.py`](resolve_target.py), which resolves the default or an explicit enabled target for `import` and `export`. It is intentionally a resolver, not an importer/exporter.

## Relationship to BDFS artifacts

The shared metadata envelope is defined in [the BDFS artifact manifest schema](../doc/schemas/bdfs-artifact-manifest.schema.json). The manifest describes an artifact and its provenance; it does not contain the filesystem payload and does not grant permission to execute hooks, deploy an image, or mutate a host.

Expected flow:

1. Select and validate an environment profile.
2. Build or prepare the environment in an isolated workspace.
3. Snapshot/export it through a declared integration adapter.
4. Validate the manifest and payload digest before import.
5. Materialize into an explicitly selected destination.
6. Compare the imported result with the source and report conversion loss.

## Safety requirements

- Inspect and validate imported artifacts before materialization.
- Keep imported hooks and entrypoints inert until a separate, explicit execution action.
- Reject path traversal, archive link escapes, malformed manifests, and unsupported special files.
- Require an explicit destination and overwrite policy; never overwrite an existing workspace implicitly.
- Exclude secrets, private keys, tokens, and machine identity.
- Never treat import as deployment, activation, flashing, or host mutation.

See [integration adapter guidance](../integrations/README.md) for the adapter side of this contract.
