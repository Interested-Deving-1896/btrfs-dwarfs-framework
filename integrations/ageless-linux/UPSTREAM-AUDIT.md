# Ageless Linux upstream source audit

Audit date: 2026-11-07
Audited repository: https://github.com/agelesslinux/agelesslinux
Audited branch: `main`
Observed HEAD: `1e1b00f45d494a41412403e9e909b0cc737bc2ae`
Observed tree: `71591363ccff54a7a0cfbeffbdc7fd36b087f523`
GitHub commit verification: **unsigned / not verified**. The revision is immutable as a Git object identifier, but its author identity and origin are not cryptographically authenticated by this commit.

## Source inventory

| Component | Path | Observed blob SHA |
|---|---|---|
| Entrypoint | `become-ageless.sh` | `1ad34672ed072d5c6986317d3389e1df5888a2e7` |
| Build assembler | `build.sh` | `abae877174e76a5f7e7d32fab9a932b4e2412892` |
| Header / globals | `lib/00-header.sh` | `8bca7cd41decf4d4afb51773312dd9163545e389` |
| OS identity | `lib/10-os-release.sh` | `559cd49238fb7ba8e62398d8fd67365fb5f6794d` |
| Compliance files | `lib/20-compliance.sh` | `66a39886e61713b15c6968e12573dd55c1336d14` |
| User database | `lib/30-userdb.sh` | `585f3b41f8799a411ba4a274e596408a519e47f6` |
| Persistent daemon | `lib/40-agelessd.sh` | `ba06a5e468b41f82667b454c93f39f85c7d75b26` |
| Configuration | `lib/50-conf.sh` | `4c793921416c97f9079029ef18dc97a0dcc98645` |
| Main orchestration | `lib/99-main.sh` | `e97f386152c85299fd62c2143f979ae2484296a2` |
| Container test harness | `test/run-in-container.sh` | `bcddd08b1441593fd0987274940f11b82cca91a8` |

The build script concatenates sorted `lib/*.sh` files, appends `main "$@"`, and makes the assembled entrypoint executable. Reproducible assembly should be checked against the pinned entrypoint before using it as an input artifact.

## Behavior and risk findings

- The converter requires root for non-dry-run operation.
- It backs up and rewrites `/etc/os-release`; it may also rewrite `/etc/lsb-release`.
- It creates compliance/policy files under `/etc/ageless/` and an installation record at `/etc/agelesslinux.conf`.
- It may create or modify per-user records under `/etc/userdb/`.
- Persistent mode can install and enable an `agelessd` systemd service/timer.
- Revert behavior uses the installation record and backup files; the no-record legacy path prints manual recovery instructions.
- The latest inspected upstream commit specifically fixes userdbd reload behavior when a display manager is active and avoids recommending wholesale removal of `/etc/userdb` in the legacy revert path.
- The conversion changes OS identity metadata. Consumers must not treat `ID=ageless` as the underlying package-manager, ABI, kernel, or release identity.

## Test-harness safety boundary

The upstream test harness is explicitly designed to run as root inside a container and performs real writes/removals under the container's `/etc`, including `/etc/os-release` and `/etc/userdb`. Its usage comment shows a bind mount of the source tree but does not itself enforce read-only mounts, network isolation, dropped capabilities, or a disposable root filesystem.

Therefore BDFS CI must not invoke this harness on the runner host or on a shared/persistent container. If it is ever run, the caller must enforce a disposable container/VM, read-only source mount, no host system mounts, no secrets, no network, dropped capabilities, no-new-privileges, and guaranteed teardown. Do not assume the upstream test's internal cleanup is a complete safety boundary.

## Provenance and compatibility status

- Pinned source revision: `1e1b00f45d494a41412403e9e909b0cc737bc2ae`
- Commit signature: unsigned; cryptographic author verification unavailable.
- Source tree and blob identifiers: recorded above; they identify content but do not establish trust.
- Base distribution, release, architecture, package manager, init system, and kernel source must be captured independently from a trusted pre-conversion image manifest or preserved original OS identity.
- Ageless conversion-tool provenance must remain separate from Linux source archives, distro kernel packages, patch series, configs, and build artifacts.
- Compatibility remains **unknown** for every base OS/release/architecture until separately validated in an isolated environment.

## Explicit non-goals

This audit does not authorize executing the converter, claiming a supported distro matrix, building or installing a kernel, publishing packages, or changing a host. BDFS integration remains inventory-only.
