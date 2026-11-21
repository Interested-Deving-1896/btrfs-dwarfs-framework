# BDFS Web Platform Engine

## Status

Architecture proposal and integration boundary. This document does not claim that a cross-platform browser runtime, PWA installer, or cloud connector has been implemented. The initial phase is contract-first and read-only.

Related tracking:
- BDFS design and implementation: https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/issues/89
- FSA orchestration: https://github.com/Interested-Deving-1896/fork-sync-all/issues/1062
- Candidate browser source: https://github.com/Interested-Deving-1896/brave-core
- Browser project upstream: https://github.com/brave/brave-core

## 1. Purpose

The BDFS Web Platform Engine provides a portable integration layer through which supported operating-system images, userspaces, containers, and application environments can expose browser and web-application capabilities.

The intended capabilities include:

- Browser/runtime discovery and replaceable engine adapters.
- Progressive Web App (PWA) installation and lifecycle integration.
- Service-worker and offline-cache capability reporting.
- Web application launchers, deep links, protocol/file handlers, notifications, and permission integration where supported.
- WebAssembly, WebGPU, media, WebAuthn/passkeys, and other browser features through explicit feature detection.
- Provider-neutral cloud identity, storage, synchronization, and API connector interfaces.
- Reproducible browser/runtime and web-app artifact packaging, provenance, and offline distribution.

This is not a proposal to turn the BDFS filesystem core into a browser, to make Brave mandatory, or to assume all operating systems implement the same browser APIs.

## 2. Existing BDFS integration boundary

BDFS already has an `integrations/` tree for optional ecosystem bridges and an `integrations/web/` directory containing an Eggs-AI/NiceGUI panel. That panel is an application-specific HTTP client/UI integration; it is not a general browser engine, PWA runtime, or cross-OS cloud connector.

The Web Platform Engine should therefore be a distinct integration with a documented contract. It should reuse existing BDFS artifact, workspace, content-integrity, and provenance mechanisms where their behavior is appropriate, without coupling browser lifecycle or user-profile state to filesystem internals.

BDFS's current kernel-facing filesystem implementation is Linux-specific. The web platform layer must remain in userspace and define OS-specific adapters rather than importing Linux VFS, Btrfs, FUSE, or kernel assumptions into non-Linux targets.

## 3. Layered architecture

```
OS / image / container / application profile
                    |
       Web Platform integration API
                    |
     Versioned capability + policy contract
                    |
       +------------+-------------+
       |            |             |
 Browser/runtime  PWA manager   Cloud connector
 adapters         and lifecycle broker interfaces
       |            |             |
 Brave/Chromium   OS app shell   OIDC/OAuth, storage,
 (optional)       service       sync and service APIs
       |            |             |
       +------------+-------------+
                    |
         OS capability adapters
                    |
       BDFS artifact + provenance layer
                    |
        FSA source/build/CI orchestration
```

### BDFS responsibilities

- Own the versioned capability, policy, runtime, and artifact-manifest contracts.
- Provide capability negotiation and explicit `supported`, `unsupported`, `experimental`, and `unknown` states.
- Define adapter interfaces and lifecycle boundaries.
- Integrate reproducible runtime/web-app artifacts with BDFS integrity and provenance mechanisms.
- Keep runtime user data and credentials outside immutable/shared filesystem artifacts by default.
- Define security, privacy, update, rollback, and evidence requirements.

### FSA responsibilities

FSA is a consumer and orchestrator, not the runtime implementation. It coordinates repository/source inventory, exact source pins, build dependencies, target-specific CI, test evidence, SBOM/license capture, provenance, and reviewable opt-in downstream packaging PRs. FSA must consume the BDFS contract instead of defining a competing runtime policy.

### Browser implementation responsibilities

A browser engine implements browser behavior. It does not, by itself, provide a complete cross-OS PWA installer, cloud identity broker, OS notification bridge, secure credential store, or package lifecycle. Those functions belong to separate adapters and services with explicit OS/provider capabilities.

## 4. Browser engine strategy

### Brave Core as a candidate

The supplied `Interested-Deving-1896/brave-core` repository identifies itself as the core engine for Brave browser on mobile and desktop and is a fork of `brave/brave-core`. Its repository metadata identifies MPL-2.0 licensing and a very large Chromium-derived source tree.

Treat this repository as a candidate source for a Brave/Chromium browser adapter, not as a BDFS submodule or mandatory dependency. Before building or importing it, record:

- Exact fork commit and upstream commit/ancestry.
- Chromium/Brave version relationship and update cadence.
- Build tools, dependency fetch behavior, host requirements, target platforms, and build resource requirements.
- MPL-2.0 obligations plus all Chromium and third-party license/notice obligations.
- Branding/trademark restrictions and distribution requirements.
- Security advisory and patch response process.
- Reproducibility, SBOM, vulnerability, and artifact-signing evidence.

The initial work should be source/build feasibility assessment only. Do not vendor or mirror the multi-gigabyte tree into BDFS.

### Replaceable engines

The contract must not assume Brave is the only engine. Future adapters may include Chromium-family, Gecko, WebKit, or platform-native web runtimes where the relevant APIs, licenses, and OS support are validated. A target can report no supported browser engine without invalidating the BDFS core.

## 5. PWA and web application integration

The PWA manager should orchestrate platform integration around capabilities exposed by the selected browser/runtime. It should not reimplement browser standards.

Track the following independently per OS, architecture, engine/version, and packaging profile:

- Web App Manifest parsing and installability criteria.
- Application identity, icons, display modes, and launcher integration.
- Service-worker registration, update, and lifecycle behavior.
- Offline cache/storage availability, quota, eviction, and persistence behavior.
- Deep links, URL routing, file/protocol handlers, and share targets.
- Notifications and permission mediation.
- Background execution limits and synchronization behavior.
- WebAuthn/passkeys and secure OS credential integration.
- WebAssembly, WebGPU, media codecs, DRM, and hardware acceleration.
- Accessibility, locale, text scaling, and input-mode integration.

A successful browser build does not prove PWA installation or runtime behavior. Emulator/simulator evidence and physical-device evidence must be labeled separately. Unsupported or unavailable OS hooks must be explicit rather than silently emulated or overclaimed.

## 6. Cloud connector boundary

Cloud integrations should be provider-neutral interfaces with independently maintained provider adapters. Candidate interface families include identity/OIDC, OAuth authorization, object/file storage, synchronization, notifications, and application APIs.

Requirements:

- User consent, clear scopes, least privilege, expiration, revocation, and account separation.
- Browser-mediated authorization using secure platform-supported flows; never embed client secrets in public apps.
- Credentials stored only in the OS credential facility or a separately reviewed encrypted broker; never in BDFS images, Git repositories, build logs, or shared content-addressed objects.
- No ambient CI credentials or build-time access to personal cloud accounts.
- Explicit offline behavior, conflict handling, data residency, provider availability, and export/deletion semantics.
- Connector permissions and data access must be independently reviewable and auditable.

Cloud connector availability must not be inferred from the presence of a browser engine.

## 7. OS and deployment profiles

Begin with discovery and per-target evidence for:

- Linux distributions and desktop environments.
- Android/AOSP and downstream mobile environments.
- ChromeOS/ChromiumOS-derived environments.
- Windows and macOS.
- FreeBSD and Darwin/XNU systems through separately validated native adapters.
- Containers, VMs, headless and server environments, where GUI, GPU, sandbox, notification, or service-worker features may be unavailable.

The contract must not assume that all targets share a package manager, sandbox, keychain, notification service, file portal, update mechanism, GPU stack, or service manager.

## 8. Artifact model

Browser/runtime and PWA artifacts should be independently versioned and content-addressed. A proposed manifest should include:

- Contract/schema version and artifact type.
- OS, architecture, ABI, runtime/engine and version.
- Exact source repositories and commit identifiers.
- Build system, toolchain, dependency lock/pins, and build profile.
- Feature/capability declarations and their evidence scope.
- SBOM, license/notice references, vulnerability scan summary.
- Artifact digest, signature/attestation references, and provenance.
- Test results with target, environment, test scope, timestamp, and limitations.
- Update channel, rollback target, and support status.

Do not place browser profile databases, cookies, history, session tokens, passkeys, or user-specific service-worker state in shared BDFS artifacts. PWA application assets may be packaged separately from personal runtime data, with origin and license metadata retained.

## 9. Security and privacy model

- Preserve browser sandboxing, site isolation, origin isolation, TLS/certificate validation, and OS security controls.
- Never make container/VM isolation a substitute for browser process sandboxing, or vice versa.
- Use least privilege for file, camera, microphone, location, notification, clipboard, and device permissions.
- Define origin-scoped service-worker, storage, and permission policies.
- Separate immutable system/runtime artifacts from mutable per-user browser profiles and caches.
- Do not package personal browsing state or credentials.
- Require verifiable updates, vulnerability response, rollback, and license/SBOM records.
- Begin with read-only discovery and explicit opt-in activation. No automatic host browser replacement, privileged daemon enablement, package installation, cloud-account mutation, device flashing, boot/partition writes, or mirror propagation.

## 10. Phased implementation

1. **Contract and threat model:** define schema, feature taxonomy, evidence states, adapter API, privacy boundary, and security model.
2. **Read-only discovery:** inventory host OS/runtime/browser/PWA capabilities without changing the host.
3. **Browser adapter prototype:** assess Brave/Chromium source and implement a minimal adapter interface with a synthetic/test implementation before any full browser build.
4. **PWA adapter prototype:** test manifest parsing, service-worker capability, offline cache, launch integration, and permission boundaries on one explicitly selected target.
5. **Cloud connector interface:** define consent/token/credential broker contracts and test with mock providers; no live account linking in CI.
6. **Artifact and FSA integration:** validate manifests, source pins, SBOM, provenance, bounded builds, and opt-in downstream packaging PRs.
7. **Platform expansion:** add OS/architecture adapters only when target-specific runtime and security evidence exists.

## 11. Initial acceptance gates

- Versioned schema validates positive, negative, unknown, and unsupported fixtures.
- Browser/runtime adapter contract is replaceable and does not require Brave.
- PWA features are tested independently and scoped to exact target/runtime versions.
- Cloud connector tests use mock credentials and prove no secret persistence or log leakage.
- Host discovery performs no writes.
- Artifact provenance binds exact source commits, toolchains, dependencies, test scope, and digests.
- CI distinguishes source/build success from runtime/PWA verification.
- License, SBOM, vulnerability, update, and rollback evidence is retained.
- No source import, large browser build, package publication, signing, deployment, host/device mutation, or mirror propagation occurs without its own explicit authorization.
