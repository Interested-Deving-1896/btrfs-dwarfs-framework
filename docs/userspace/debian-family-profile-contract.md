# Debian-family userspace profile contract

Status: design contract; implementation and package-availability validation are pending.

This document defines the userspace profile dimension for BDFS rootfs builds. It generalizes the profile intent in the following Ubuntu 26.04 bootc repositories:

- [Ubuntu 26.04 minimal bootc base](https://github.com/Interested-Deving-1896/ubuntu-26.04-bootc)
- [Ubuntu 26.04 desktop bootc](https://github.com/Interested-Deving-1896/ubuntu-26.04-desktop-bootc)
- [Ubuntu 26.04 server bootc](https://github.com/Interested-Deving-1896/ubuntu-26.04-server-bootc)

These are reference implementations, not runtime dependencies. BDFS must build ordinary root filesystems without requiring bootc, OCI image semantics, or bootc's special `/var` layout.

## 1. Profile matrix

Every supported Debian, Devuan, and Ubuntu rootfs exposes three independent profile choices:

| Distribution | `headless-minimal` | `desktop-kde` | `cloud-server` |
|---|---|---|---|
| Debian | Required | Required; default desktop choice is KDE Plasma | Required |
| Devuan | Required | Required; default desktop choice is KDE Plasma | Required |
| Ubuntu | Required | Required; default desktop choice is KDE Plasma | Required |

“Default desktop” means the profile selected when a desktop profile is requested without a desktop-environment override. It does not mean that every rootfs defaults to a desktop. The overall default profile remains `headless-minimal`.

## 2. Release cohorts

The release rows below are compatibility cohorts, not claims that Ubuntu releases are directly based on the corresponding Debian release.

| Cohort | Ubuntu | Debian | Devuan |
|---|---|---|---|
| 22.04 generation | 22.04 LTS (Jammy) | 11 (Bullseye) | 4 (Chimaera) |
| 24.04 generation | 24.04 LTS (Noble) | 12 (Bookworm) | 5 (Daedalus) |
| 26.04 generation | 26.04 LTS (Resolute) | 13 (Trixie) | 6 (Excalibur) |

Legacy status is independent of buildability. Debian 11 and Devuan 4 must be marked legacy/compatibility-only in current reports; do not imply that archived or end-of-support releases receive security maintenance. Release status must be refreshed from authoritative distribution metadata before each support decision.

## 3. Profile semantics

### headless-minimal

- Smallest supported userspace and BDFS runtime/build prerequisites for the selected target.
- No desktop environment, display manager, graphical login, desktop session, or desktop-only services.
- No cloud-init or server role packages unless a target-specific dependency requires them and that dependency is documented.
- This is the default profile when no profile is supplied.

### desktop-kde

- Composes on the minimal profile.
- Installs a release-supported KDE Plasma desktop/session, a compatible display manager, and the required desktop integration (audio, network management, portals, and session services).
- KDE Plasma is the default desktop-environment choice. Package names and supported Plasma generation must be resolved per distribution and release; never assume Ubuntu package names exist unchanged on Debian or Devuan.
- Does not silently enable unrelated server/cloud roles.

### cloud-server

- Composes on the minimal profile.
- Adds only target-supported server/cloud capabilities, such as SSH, cloud-init, networking configuration, time synchronization, firewall policy, and server provisioning utilities.
- Service activation must be expressed as policy and applied through the target's service manager.
- Does not install a desktop environment by default.

Profiles may share package groups, but their manifests and outputs remain independently selectable. Any explicit profile composition must be deterministic and report conflicts before modifying a rootfs.

## 4. Distribution and init-system boundaries

- Preserve the selected distribution, release, architecture, package manager, and init system throughout bootstrap and build.
- Do not treat Devuan as “Debian plus a different label”. Discover its actual init/service manager and use a distro adapter for service enablement, masking, and dependency policy.
- Never invoke systemd-only operations on a Devuan target unless that target is explicitly configured with a compatible systemd variant.
- Do not replace, convert, or migrate the target init system implicitly.
- Keep package naming, virtual-package resolution, recommends/suggests policy, firmware policy, and service names in distro/release adapters rather than in common profile logic.

## 5. Manifest contract

Each profile build must be driven by a versioned machine-readable manifest. A profile manifest should contain at least:

- schema version and stable profile ID;
- distro ID, release ID, codename, and support/legacy state;
- target architecture and normalized architecture alias;
- package groups and explicit package exclusions;
- recommends/suggests policy and repository/component requirements;
- init-system and service-manager requirements;
- services to enable, disable, or leave at package defaults;
- profile conflicts and supported composition rules;
- source repository/ref and package metadata provenance;
- validation state and evidence references.

Illustrative shape (package lists intentionally omitted until resolved against each target's repositories):

```json
{
  "schema_version": 1,
  "profile_id": "desktop-kde",
  "distribution": {
    "id": "debian",
    "release": "13",
    "codename": "trixie",
    "support_state": "supported"
  },
  "architecture": "amd64",
  "desktop": {
    "environment": "kde-plasma",
    "default_when_desktop_requested": true
  },
  "package_groups": [],
  "exclude_groups": ["cloud-server"],
  "init": {
    "required": "target-native",
    "service_manager": "detect"
  },
  "validation": {
    "state": "not-yet-tested",
    "evidence": []
  }
}
```

The example is a schema illustration, not a claim that a Debian 13 KDE profile has already passed.

## 6. Build and validation contract

The build matrix adds profile as a dimension:

`distribution × release × profile × architecture × build strategy`

- Keep host OS and target rootfs independent.
- Prefer native builds where available. Otherwise require an explicit cross-toolchain/sysroot and, where execution is needed, a verified emulator/binfmt or remote native runner.
- Normalize architecture aliases at the orchestration boundary while preserving the target package manager's canonical architecture names.
- Resolve package availability against the exact target release and architecture before starting installation.
- Build in isolated rootfs contexts; never mutate the runner host rootfs.
- Test both positive and negative composition: expected profile packages are present, prohibited profile packages are absent, and service policy matches the target init system.
- Validate package database consistency, rootfs integrity, BDFS build/tests, and reproducibility metadata.
- Treat missing architecture runners, unavailable packages, and untested init variants as unavailable/blocked, not passed.

Allowed report states: `passed`, `failed`, `skipped`, `unavailable`, `legacy`, and `not-applicable`. A skipped or unavailable check must never satisfy a required gate.

## 7. bootc relationship

The Ubuntu bootc repositories demonstrate three useful package-layer intents, but bootc image assembly is a separate integration:

- BDFS profile generation must not depend on bootc being installed.
- Do not copy bootc-specific filesystem operations (including wiping `/var`, deleting the dpkg database, or constructing an ostree symlink forest) into ordinary BDFS rootfs generation.
- If a bootc consumer is added later, it must be an adapter that consumes a completed BDFS-compatible profile and applies bootc-specific validation in its own isolated build.
- Preserve the source repositories' distinct roles; the current desktop reference is GNOME-oriented, while the BDFS desktop target defined here is KDE Plasma.

## 8. Acceptance criteria

1. All nine distribution/profile combinations are represented for every supported release cohort.
2. The default profile is `headless-minimal`; the default desktop choice is KDE Plasma.
3. Each profile can be built independently without package/service leakage from the other profiles.
4. Devuan init/service behavior is independently detected and validated.
5. The matrix report is generated from actual build evidence and distinguishes passed, failed, skipped, unavailable, legacy, and not-applicable.
6. No image publication, downstream mirror write, destructive host operation, or release action is part of profile validation.

## 9. Implementation sequence

1. Inventory existing BDFS rootfs/bootstrap entry points, package installation logic, and CI build contexts.
2. Add manifest parsing and schema validation with negative tests before package installation is automated.
3. Implement package-resolution adapters for one current supported release per distribution.
4. Add the minimal profile and prove rootfs isolation and package database integrity.
5. Add KDE Plasma and cloud/server adapters with per-release package/service validation.
6. Expand to legacy compatibility cohorts and additional architectures only where package metadata, toolchains, and execution evidence exist.
7. Publish the complete machine-readable matrix; keep unsupported combinations explicit.

This contract does not authorize mirror/publication workflows, repository creation, host modification, destructive filesystem operations, or bypassing existing audit and CI gates.
