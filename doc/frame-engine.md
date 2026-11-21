# BDFS Frame Engine

**Status:** proposed architecture; this document does not imply runtime implementation.

## Purpose and ownership

The **BDFS Frame Engine** is a platform-neutral userspace subsystem for system-wide Web3 provider access and content-addressed/decentralized artifact storage. It is not a filesystem backend, blockchain, wallet implementation, or mandatory desktop application.

- **Frame Core (BDFS):** versioned capability contracts, lifecycle, policy, artifact references, adapter discovery, and evidence.
- **Frame provider adapter:** optional system-wide wallet/provider bridge; EIP-1193 may be supported.
- **IPFS adapter:** content addressing, import/export, pin status, and retrieval.
- **Filecoin adapter:** storage-provider discovery, deal lifecycle, persistence, and retrieval evidence.
- **Lotus adapter:** one Filecoin node/client implementation behind the replaceable Filecoin interface.
- **OS adapters:** native secure credential storage, process/service integration, and permission UX.
- **FSA:** later coordinates source pins, builds, tests, SBOM, provenance, and opt-in packaging. It does not own BDFS runtime semantics.

Btrfs/DwarFS and other local backends remain valid local storage. CIDs and Filecoin deal metadata are additional references/replicas, not replacements for local integrity and recovery guarantees.

## Default profile: IPFS-first, Filecoin-capable

1. **IPFS is the default content-addressed naming and retrieval layer** for immutable artifacts explicitly eligible for distribution.
2. Prefer a local Kubo-compatible node when installed and explicitly enabled. Otherwise use only a configured remote endpoint/gateway. Never silently fall back to an arbitrary public gateway.
3. **Filecoin is the preferred decentralized persistence network** for artifacts explicitly classified for archival. It is not automatically used for every artifact and does not mean a deal has been created or funded.
4. **Lotus is the reference/default Filecoin client adapter** when an operator selects a self-managed Lotus node. Keep the Filecoin contract implementation-neutral so other compatible clients/providers can be added.
5. Preserve local BDFS copies and manifests by default. Verify CID, multihash, size, bytes, signed manifest, provenance, deal status, and retrieval evidence independently.
6. Private repositories, secrets, credentials, personal data, proprietary firmware, and unlicensed material are excluded from public networks by default.

“Default” selects preferred integration and policy; it does **not** authorize daemon installation, public upload, pinning, paid storage, wallet access, transaction signing, or deletion.

## Capability and authorization model

Separate capabilities for `discover`, `resolve`, `retrieve`, `verify`, `pin`, `publish`, `deal_quote`, `deal_create`, `deal_status`, `wallet_connect`, `wallet_sign`, `unpin`, and `delete`.

Read-only discovery/retrieval must never imply write capability. Pinning, publication, deal creation, payment, signing, unpinning, and deletion require separate explicit authorization and auditable confirmation. CI receives no wallet-signing or payment capability.

## Artifact provenance extension

A future versioned manifest should record, where applicable:

- artifact identity, digest, size, media type, source repository and exact commit/tree
- build inputs, toolchain, SBOM, and provenance statement
- CID codec, multihash algorithm/digest, and verification result
- IPFS node/provider identity, endpoint class, pin state, and observation time
- Filecoin network, client/version, deal/provider IDs, deal state, time bounds, retrieval evidence, and cost evidence
- retention/replication policy, privacy classification, and publication authorization reference

Never record seed phrases, private keys, bearer tokens, RPC credentials, or personal data. A CID alone is not proof of authenticity, confidentiality, ownership, or availability.

## Supplied Frame source assessment

The supplied [Interested-Deving-1896/frame](https://github.com/Interested-Deving-1896/frame) is a fork of [floating/frame](https://github.com/floating/frame). Its README describes a desktop Electron Web3 application exposing a system-wide JSON-RPC endpoint and injected EIP-1193 provider. The inspected fork manifest reports version 0.6.11, GPL-3.0, Electron 23, Node.js 18.12.1+, and IPFS-related dependencies.

The upstream repository is archived; the fork exposes a `develop` branch. This is a material maintenance/security risk. Do not treat it as production-ready or vendor its full source into BDFS core. Keep it optional and replaceable.

Before importing/building, audit exact fork/upstream commit SHAs and ancestry, divergence, security advisories, dependency age and lockfile, reproducibility, supported OS/architectures, Electron/Node support, packaging/update/signing, GPL-3.0 and transitive license obligations, RPC binding/authentication/origin controls, transaction approval, and hardware-wallet handling. Determine whether a minimal provider adapter can be used without embedding the whole desktop app. Never expose private keys, seed phrases, signing APIs, or unrestricted transaction submission to build workers or repository automation.

## Security and privacy

- Treat remote responses and retrieved content as untrusted; stage and validate before use.
- Verify fetched bytes against the requested CID, then separately verify signed BDFS provenance.
- Use authenticated local RPC, loopback-only listeners by default, strict origin/permission controls, and least-privilege tokens.
- Keep keys in OS secure credential facilities or hardware signers, never in images, artifacts, repos, CI logs, manifests, or environment dumps.
- Threat-model malicious gateways/providers, substitution, wallet compromise, dependency attacks, RPC exposure, privacy leakage, denial-of-service, bandwidth exhaustion, and storage-cost abuse.
- Provide quotas, bandwidth/storage limits, pin expiry, retention, cost estimates, cancellation, and safe garbage collection.
- Preserve local data unless an explicit retention policy and verified recovery path authorize removal.
- No automatic package installation, privileged daemon activation, firewall/network changes, wallet/account mutation, publication, or spending.
- Do not assume Linux Btrfs/VFS behavior applies to FreeBSD or Darwin/XNU.

## Cross-platform contract

The core schema is OS-neutral. Native adapters may use OS-specific secure storage and service facilities. Every capability result is scoped to OS, architecture, client/version, configuration, and operation. Unsupported and unknown are explicit outcomes. Linux, Android/AOSP, Windows, macOS, FreeBSD, and Darwin/XNU evidence must be reported separately.

## Phased delivery and acceptance

1. Design versioned schema, threat model, privacy classification, default profile, and authorization matrix.
2. Audit Frame fork/upstream pins; no source import yet.
3. Implement mock adapters and positive/negative/unknown/denied fixtures.
4. Prototype read-only IPFS discovery, CID resolution, staged retrieval, and digest verification.
5. Separately gate local-node pin/publish tests.
6. Prototype Filecoin/Lotus with mocks or disposable test networks only; no real funds or production credentials.
7. Add opt-in OS credential/service integration.
8. Add FSA exact pins, bounded builds, SBOM/provenance, CI evidence, and opt-in downstream packaging.

Acceptance requires schema fixtures, adapter replacement tests, content-integrity tests, proof that defaults do not publish/pay/sign, secret-leak tests, scoped cross-platform evidence, and auditable provenance.

## Initial non-goals

No replacement of Btrfs/DwarFS; no automatic public uploads; no public gateway/provider deployment by default; no wallet creation/seed import/signing/payment; no wholesale import of archived Frame; no host package installation, privileged service activation, firewall changes, device flashing, boot/partition writes, or mirror propagation.
