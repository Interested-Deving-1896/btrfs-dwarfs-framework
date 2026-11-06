# Kernel Upstream Migration & Backport Engine (KUMBE)

BDFS is the provider-neutral contract, evidence registry, and compatibility consumer for KUMBE. It is **not** a kernel source vendor and must not choose or silently replace a provider's kernel line.

## Scope

KUMBE coordinates forward migration to newer upstream Linux releases and controlled backport evaluation for supported stable/LTS branches. Legacy/EOL targets are opt-in, explicitly marked best-effort, and never represented as receiving upstream security maintenance.

Provider adapters remain responsible for their own source resolution, patch stacks, configuration, build systems, packaging, and release cadence:

- LiqXanMod: resolve each actual MAIN/EDGE/LTS/RT source and preserve its selected provider/workload profile.
- Liquorix Unified Kernel: model Liquorix source refs, patch/config layers, package matrix, signing, and reproducibility.
- XanMod Unified Kernel: resolve MAIN/EDGE/LTS/RT refs independently; preserve RT-specific patches/configuration and do not silently fall back.
- BDFS: validate exact kernel/module compatibility, ingest provenance and validation evidence, and expose supported/blocked combinations to packaging and image workflows.

## Shared manifest

The versioned JSON Schema at `schemas/kernel-upstream-migration/v1/manifest.schema.json` is the initial contract. It records immutable source provenance, target kernel line/version/architectures, operation, per-change classification/dependencies/license/risk/ABI impact, policy gates, and validation evidence.

Schema changes must be versioned. Consumers must reject unknown required semantics rather than guessing.

## Required classifications

- `clean-forward-port`, `clean-backport`
- `context-adjusted-backport`
- `semantic-manual-port`
- `not-applicable`, `blocked`, `rejected`

A conflict-free apply is not correctness evidence. Dependency/order, Kconfig, API/ABI, compiler, architecture, and behavior must be evaluated.

## Execution stages

1. Read-only discovery: resolve upstream tags/branches to immutable commits; verify signatures/checksums where available.
2. Inventory: record downstream patch/config state and target kernel capabilities.
3. Plan: resolve dependencies, duplicate fixes, ordering, license, risk, ABI impact, and target applicability.
4. Isolated application: use disposable worktrees/branches; preserve original trees and patch queues.
5. Validation: formatting/static checks, targeted tests, Kconfig/config comparison, kernel/module builds, boot/smoke and runtime tests where infrastructure exists.
6. Evidence: record each check as passed, failed, skipped, unavailable, not-applicable, or unknown. Skipped/unavailable/unknown never count as passed.
7. Publication: create candidate branches/PRs only after configured gates; do not merge, release, install kernels, or mutate bootloaders automatically.

## Mandatory safety rules

- Human approval is required for semantic/manual ports, security-sensitive changes, ABI/config changes, unresolved conflicts, and publication.
- Never write directly to protected branches.
- Never silently drop/reorder downstream patches, change the selected kernel provider/line, or downgrade.
- Treat userspace-facing syscall/ioctl/netlink/VFS/module/driver ABI impact as an explicit gate.
- Separate security fixes from features/performance changes; prioritize supported stable/LTS security updates.
- Mark EOL status, missing hardware, unsupported architectures, and absent runtime evidence explicitly.
- Every applied change must link to immutable upstream provenance and target-specific validation evidence.
- No direct kernel installation, bootloader mutation, automatic merge, or release.

## Current implementation boundary

This commit establishes only the shared schema and design contract. It does not apply patches, alter kernel source/configuration, claim compatibility, or certify any build. Provider adapters and representative LTS validation remain separate follow-on work.
