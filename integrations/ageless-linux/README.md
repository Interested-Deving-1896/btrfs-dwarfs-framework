# Ageless Linux integration profile

**Status: inventory-only; compatibility unverified.**

Ageless Linux currently provides an OS identity/policy conversion tool, not an independently established kernel source distribution. The upstream entrypoint is `become-ageless.sh` (version 0.1.1 at the inspected upstream revision); `build.sh` assembles it from `lib/*.sh`. The tool rewrites `/etc/os-release`, optionally `/etc/lsb-release`, creates `/etc/ageless/` policy/compliance files, and in persistent mode may install a systemd timer/service. It advertises dry-run and revert modes. Its documentation says Debian-based systems are the design/test target but notes it technically runs on systems with `/etc/os-release`; neither Debian-only nor universal compatibility should be assumed.

## Upstream resources

| Role | Repository | Default branch |
|---|---|---|
| Conversion tool | https://github.com/agelesslinux/agelesslinux | `main` |
| Main website | https://github.com/agelesslinux/agelesslinux.org | `main` |
| Documentation site | https://github.com/agelesslinux/agelesslinux.github.io | `main` |
| Store | https://github.com/agelesslinux/store.agelesslinux.org | `master` |

Existing forks are present under `Interested-Deving-1896`. Preserve upstream and fork identities separately; do not assume they are synchronized.

## BDFS integration boundary

- Model Ageless as an **OS identity/policy conversion profile**, not a kernel source or kernel ABI.
- Determine the actual base OS from preserved `/etc/os-release.pre-ageless` or trusted image metadata. Converted `ID=ageless` alone is insufficient for package or kernel selection.
- Kernel source, package manager, release, architecture, init system, and ABI compatibility remain properties of the underlying base distribution and selected kernel provider.
- The Ageless Store roadmap describes a content-addressed catalog with minisign signatures and SHA-256 package hashes. This is a separate package-distribution trust domain, not Debian repository metadata.
- Website and store sources are ecosystem references, not kernel source inputs.

## Safety model

The conversion script is privileged and mutates host configuration. CI must never execute it against the runner host. Any future behavioral tests must use disposable isolated filesystem/VM/container fixtures with no host mounts, secrets, or network, and explicit teardown. Dry-run output is not proof that execution is safe.

Before enabling conversion or image export, audit all `lib/*.sh` modules and tests; verify backup/revert behavior, symlink handling, idempotence, persistent systemd unit behavior, and rollback after partial failure. Preserve original base identity as explicit provenance.

## Readiness gates

1. Pin Ageless source to an immutable commit and record tree/blob hashes.
2. Verify the script is reproducibly assembled from pinned `lib/*.sh` modules by `build.sh`.
3. Record base OS ID/version, architecture, init system, kernel package/source identity, and package-manager identity independently.
4. Keep compatibility `unknown` until the specific base OS/release/architecture has passing isolated tests.
5. Do not claim a kernel build target, package repository, image, or supported architecture without evidence.
6. Keep conversion, package publication, kernel builds, and installation disabled in read-only inventory mode.

## Inspected upstream files

- `become-ageless.sh`, upstream main blob `1ad34672ed072d5c6986317d3389e1df5888a2e7`
- `build.sh`, upstream main blob `abae877174e76a5f7e7d32fab9a932b4e2412892`
- Store `ROADMAP.md`, upstream master blob `a54fe532b5214a06f50b049553b67fec34e0449b`

This is an initial source-grounded integration contract, not a declaration of supported compatibility.
