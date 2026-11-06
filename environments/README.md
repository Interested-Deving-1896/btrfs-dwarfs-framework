# BDFS environments

This directory contains buildable and reusable environment definitions. It is the **environment/profile side** of BDFS; it is not an integration-adapter registry.

## Current inventory

| Path | Purpose | Current role |
|---|---|---|
| `workspace/Dockerfile` | Ubuntu-based GitLab Enhanced workspace image with BDFS, DwarFS, BFG and runner tooling | Build context / environment image |

The directory is intentionally documented as it exists today. New environment profiles should be added as independently named subdirectories with their own build instructions and declared platform requirements.

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
