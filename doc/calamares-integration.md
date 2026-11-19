# BDFS–Calamares integration contract (proposal)

**Status:** design contract; not implemented or stable API yet  
**Tracking:** [BDFS issue #68](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/issues/68)  
**Upstream:** [Calamares on Codeberg](https://codeberg.org/Calamares/calamares) · [Calamares documentation](https://calamares.codeberg.page/)

This document defines the boundary that must exist before a Calamares module can safely install a BDFS-native system. It deliberately does not claim that the proposed commands or module already exist.

## 1. Current-state audit

The repository currently contains Calamares references in the Eggs-AI assistant/knowledge area, including installer selection, system inspection, configuration guidance, and an assistant endpoint. Those are advisory/configuration features, not a BDFS deployment engine.

The repository's `scripts/bdfs-dev.sh` is a developer-workspace manager. Its BTRFS, OverlayFS, and DwarFS modes create mutable workspaces; it is not a target-system installer and must not be called as one.

BDFS does have a **partial immutable-root boot path** in `boot/doc/boot-integration.md`, `boot/install.sh`, the initramfs-tools/dracut hooks, and `boot/bdfs-image-update`. The documented model boots a DwarFS system image from a BTRFS image store with a BTRFS-backed writable OverlayFS upper layer; the guide also describes a manual initial-root-image example using `debootstrap` and `mkdwarfs`. This is valuable existing functionality and should be reused rather than replaced.

That boot path is not yet a complete installer contract. The documented initial root-image creation is manual; the boot installer installs boot support on the currently running host and rebuilds its initramfs; and the guide's GRUB configuration is a manual follow-up. The repository does not yet demonstrate a transactional target-device deployment that populates a selected Debian/Devuan/Ubuntu root, finalizes that target's identity/packages/init system, writes its boot configuration, and verifies/recoverably activates the result.

The bootloader/firmware compatibility work is tracked separately in [issue #76](https://github.com/Interested-Deving-1896/btrfs-dwarfs-framework/issues/76). Installer capability responses must consume that machine-readable support state rather than infer boot support from the presence of GRUB, an EFI system partition, or an initramfs hook. Firmware interface (BIOS/UEFI), partition-table format (MBR/GPT), bootloader, architecture, and kernel handoff method are separate compatibility dimensions. Until a combination has reproducible CI/emulator evidence, it must be reported as planned, unknown, or unsupported—not as installer-capable.

The current BDFS CLI/daemon and storage integrations therefore do not yet expose a verified end-to-end installer API for target provisioning, distro finalization, or interrupted-install recovery. In particular:

- Reuse and test the existing immutable-root boot path; do not invent a competing boot architecture.
- Do not invoke `boot/install.sh` as if it were a target-root installer: its documented operations install boot components on the running host.
- Do not translate a developer workspace into a system installation.
- Do not advertise BTRFS/DwarFS workspace modes as installer-supported rootfs modes without a bootable target lifecycle.
- Keep Calamares selection configurable until transactional target deployment and disposable-VM tests pass.

## 2. Responsibility boundaries

| Component | Owns |
|---|---|
| Calamares core | Installer UI, user confirmation, partition workflow, progress and job orchestration |
| BDFS installer service/API | BDFS capability discovery, validated deployment plan, BDFS-native rootfs creation/population, transaction state, verification and recovery |
| BDFS Calamares module | Translate Calamares job inputs to the versioned BDFS API; display safe progress and actionable errors |
| Distro adapter | Package/repository/runtime details, initramfs, bootloader and service-manager finalization for one target distribution family |
| BDFS media profile | Whether a graphical installer is included and which Calamares branding/configuration is selected |

Calamares should use its native partitioning, mount, user, locale and bootloader modules where their documented contracts are compatible. The BDFS module must not bypass Calamares' confirmation UX or independently reinterpret partition selections.

## 3. Proposed versioned BDFS installer API

The following is a proposed interface, not a promise that these commands currently exist. The implementation must publish a machine-readable schema and compatibility policy before a Calamares module depends on it.

### 3.1 Read-only capability discovery

Proposed operation:

```text
bdfs installer capabilities --format=json
```

Requirements:

- No disk writes, mounts, formatting, partition changes, or package installation.
- Return API major/minor version, BDFS version, supported target architectures, available storage capabilities, required kernel features/tools, and supported distro-adapter identifiers.
- Report each capability as supported, unavailable, or unknown; unknown is not supported.
- Never infer support merely because a binary or filesystem utility is installed.

### 3.2 Plan validation

Proposed operation:

```text
bdfs installer plan --request <validated-request.json> --format=json
```

The request must identify the selected target partition/mount mapping, target distro/release/architecture, firmware mode, selected BDFS storage profile, and adapter ID. It must not accept arbitrary shell fragments, executable paths, hooks, or command strings.

The response must be a non-destructive plan containing:

- normalized target identity and mount mapping;
- exact operations and affected block devices/filesystems;
- required space and capability checks;
- selected BDFS storage model and distro adapter;
- API compatibility result and a digest binding the plan to its inputs;
- explicit warnings and blockers.

Planning must not format, mount persistently, write a rootfs, or install packages. A plan becomes invalid if its target identity or relevant device state changes.

### 3.3 Explicitly authorized apply

Proposed operation:

```text
bdfs installer apply --plan <plan.json> --confirmation <one-time-token>
```

The one-time confirmation token must be issued only after the Calamares UI has shown the exact target and destructive operations and the user has explicitly confirmed. It must be bound to the plan digest, target identity, and short expiry. The service must revalidate the target immediately before the first write.

Apply must:

- reject stale, altered, unsupported, or replayed plans;
- execute only typed operations implemented by BDFS (never arbitrary commands from JSON);
- write transaction state before each irreversible stage;
- expose structured progress and stable error codes;
- stop safely on any mismatch or unsupported state;
- never silently substitute a conventional distro-native rootfs when BDFS deployment fails.

### 3.4 Status, verification and recovery

Proposed operations:

```text
bdfs installer status --transaction <id> --format=json
bdfs installer verify --transaction <id> --format=json
bdfs installer recover --transaction <id> --mode <resume|rollback> --format=json
```

Recovery modes must be explicitly supported per operation. If safe rollback is impossible, the service must report the exact incomplete state and require operator intervention rather than deleting or overwriting data speculatively. Verification must report rootfs, storage, boot configuration and adapter finalization independently.

### 3.5 API compatibility

- Version the API independently of the BDFS application version.
- A module must reject an unsupported API major version.
- Minor-version additions must be backward compatible; clients must ignore unknown optional fields and reject unknown required operations.
- Use JSON schemas and golden request/response fixtures in tests.
- Keep diagnostics structured and redact secrets, credentials and personal data.

## 4. Storage model requirements

BDFS must define installer storage profiles separately from the existing developer-workspace backends.

- **BTRFS target:** specify subvolume layout, mount options, snapshot/rollback boundaries, quota assumptions, and boot-time mount integration.
- **DwarFS-backed target:** specify where the immutable image is stored, how it is verified, which writable layer is used, how that layer is persisted across boots, and how initramfs/bootloader discovery works.
- **Other modes:** remain unavailable to the installer until an explicit target lifecycle and bootability contract is implemented.

A DwarFS image plus OverlayFS upper layer is not automatically a valid installed operating system. The design must prove persistence, early-boot availability, upgrade semantics, recovery and integrity before enabling that profile.

## 5. Calamares module design

Initial implementation should be a separately packaged Python job module using the versioned BDFS API. A custom C++/Qt/QML view module is unnecessary unless testing demonstrates a genuinely new user interaction is required.

Proposed job sequence:

1. **BDFS preflight:** read-only capability discovery and adapter compatibility checks.
2. **Plan:** consume the already-confirmed Calamares partition/mount result and request a non-destructive BDFS plan.
3. **Confirm plan:** show target, storage model, and operations through Calamares' normal confirmation flow; no writes yet.
4. **Deploy:** submit the one-time authorized plan and report structured progress.
5. **Finalize:** invoke only the selected distro adapter's typed finalization operations.
6. **Verify:** validate rootfs/storage/boot configuration and emit an installation report.

The module must validate all Calamares job inputs, use argument arrays or a documented IPC protocol (never shell interpolation), enforce timeouts, capture structured output, and map stable BDFS error codes to actionable installer messages.

## 6. Distribution adapter boundary

Debian, Devuan and Ubuntu must be separate adapter implementations or separately versioned adapter profiles. Do not assume identical package names, repositories, init systems, initramfs tooling, bootloader defaults, service management or release support.

Each adapter must declare:

- supported releases and architectures;
- package source and package population strategy;
- init system and service finalization;
- initramfs and bootloader integration;
- required target filesystem and kernel capabilities;
- supported firmware modes;
- adapter version and compatibility with the BDFS installer API.

No adapter may change the build host or live installer environment as a side effect of target finalization.

## 7. Installer defaults and profiles

- Graphical BDFS desktop installation media: Calamares is the intended default once the BDFS deployment contract and VM tests are complete.
- `desktop-kde`: use BDFS KDE branding/configuration.
- Other desktop profiles: use neutral or profile-specific branding; do not force KDE.
- `headless-minimal` and `cloud-server`: remain headless by default.
- Unattended installation: explicit opt-in with a separately validated non-interactive policy.
- Installer choice remains distinct from distro, release, architecture and BDFS storage profile.

Do not make Calamares the default in a release until the acceptance tests below pass. Existing installer-selection hints in the AI assistant are not sufficient evidence.

## 8. Security and failure behaviour

- No destructive operation before explicit Calamares confirmation.
- Validate block-device identity, mount ownership, filesystem state and plan digest again immediately before writes.
- Reject symlinks/path traversal and unexpected device aliases in target paths.
- Never accept arbitrary commands, scripts, package hooks or environment overrides from an installer request.
- Do not run as root longer than required; minimize the privileged service surface.
- Keep the live installer, build host and target rootfs clearly separated.
- Do not silently fall back to a different rootfs implementation or storage backend.
- Preserve structured transaction logs without storing secrets.
- Test interrupted writes, unplugged devices, stale plans, wrong target selection, low space, unsupported firmware, missing tools and API mismatch.

## 9. Test and release gates

The integration is not complete until all of the following are demonstrated:

1. Unit tests cover request validation, schema compatibility, plan digest binding, token expiry/replay rejection, error mapping and recovery-state transitions.
2. Calamares module tests use mocked BDFS responses and never touch block devices.
3. Disposable-VM tests cover supported distro/release/architecture combinations independently.
4. BTRFS and DwarFS target modes are tested separately; unsupported modes fail closed.
5. UEFI/BIOS support is claimed only for combinations that actually boot in tests.
6. Interrupted installation and recovery are exercised at every transaction boundary.
7. A successful VM install boots, verifies the BDFS-native rootfs, and produces an auditable report.
8. Headless profiles are verified not to pull in or launch Calamares by default.
9. Packaging/configuration is independently installable and removable.
10. CI is green for the exact PR head; no bypassing required checks.

## 10. Explicit exclusions

This integration does not use, modify, package, test, or depend on BootC. The existing `integrations/bootc` directory is outside this work and must remain unchanged.

This proposal does not authorize changes to Calamares upstream, release publication, mirror propagation, protected repository settings, or destructive tests on real user devices. Upstream coordination must use the active Codeberg project after the BDFS API and tests are mature.

## 11. Implementation order

1. Implement and test the BDFS installer service/API and transaction model.
2. Define the BTRFS target lifecycle and DwarFS target lifecycle; mark unsupported modes explicitly.
3. Implement one distro adapter end-to-end, then add the other adapters independently.
4. Implement the Calamares Python job module against the published API.
5. Add packaging, branding and graphical-media defaults while preserving headless profiles.
6. Complete disposable-VM matrix and recovery tests.
7. Prepare an independently reviewable upstream proposal for Calamares on Codeberg.

Until step 6 passes, this integration remains experimental and must not be presented as a supported production installer.
