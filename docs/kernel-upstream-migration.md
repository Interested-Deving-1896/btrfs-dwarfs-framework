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

### Multi-source manifest v1.1

The additive schema at `schemas/kernel-upstream-migration/v1.1/manifest.schema.json` introduces `source_inputs` without changing the v1.0 contract. It models every independently selected input using a stable `input_id`, a semantic `role`, and a `kind`:

- **Git inputs** carry repository, URL, requested ref, and a full immutable commit ID.
- **Archive inputs** carry URL, archive version, and a full SHA-256 digest.
- Each input carries an explicit integrity state. The planner blocks inputs not declared `verified`; that declaration is evidence supplied by the producer and is not itself cryptographically verified by the read-only planner.
- At least one input must have role `kernel-source`. Additional inputs can represent provider patch bundles, Liquorix/Zen patches, package archives, config bundles, or other independently versioned material.

For example, a Liquorix migration must not collapse the vanilla kernel tarball and Liquorix package archive into one synthetic source identity. A LiqXanMod migration must keep the XanMod Git source separate from its Liquorix/Zen and hybrid patch inputs. Provider adapters remain responsible for collecting trustworthy checksum/signature evidence; missing or unverified provenance blocks planning.

The planner supports both v1.0 single-source manifests and v1.1 multi-source manifests. It remains read-only and does not download inputs, validate signatures, recompute archive digests, apply patches, or build kernels.

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


## Initial provider inventory implementations

The first provider-side read-only inventory commands are being introduced alongside this contract:

| Provider | Inventory command | Draft PR |
|---|---|---|
| LiqXanMod | `python3 scripts/kumbe-inventory.py` | [#35](https://github.com/Interested-Deving-1896/liqxanmod/pull/35) |
| Liquorix Unified Kernel | `python3 scripts/kumbe-inventory.py` | [#21](https://github.com/Interested-Deving-1896/liquorix-unified-kernel/pull/21) |
| XanMod Unified Kernel | `python3 scripts/kumbe-inventory.py` | [#18](https://github.com/Interested-Deving-1896/xanmod-unified-kernel/pull/18) |

Each command emits a JSON inventory of locally available source candidates, commit/ref information where present, patch-series entries and SHA-256 digests, config inputs, and explicit warnings. It accepts `--root` for isolated tests. It performs no network access and does not modify source trees. Its output is an evidence input, not a KUMBE migration manifest or compatibility verdict.

The focused inventory tests currently pass in all three provider PRs. The next implementation stage is to make each provider resolve and pin its own immutable source inputs, then add a dry-run dependency/applicability planner against this shared manifest. Isolated patch application remains disabled until those gates are implemented and reviewed.
