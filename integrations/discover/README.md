# KDE Discover integration

**Status: integration contract / candidate frontend; no package or backend support is certified.**

This integration registers the user's fork of KDE Discover as a desktop application-management frontend that can be evaluated alongside BDFS. It does not make Discover a BDFS filesystem component, and it does not make BDFS a package manager.

## Project identity

- Maintained fork: https://github.com/Interested-Deving-1896/discover
- Upstream parent: https://github.com/KDE/discover
- Upstream project homepage: https://invent.kde.org/plasma/discover
- Language: C++ / Qt / KDE Frameworks
- Observed fork version: 6.6.80 (from its CMake project metadata)
- Default branch: master
- Source revision: not yet pinned for a reproducible downstream build

The fork README currently contains generated placeholder sections and an inaccurate "original project" statement. Treat the GitHub parent relationship as authoritative provenance. Do not infer license or source freshness from the README; audit the license files and compare the fork against upstream before packaging.

## Integration responsibilities

- **Discover:** UI, application search, AppStream presentation, reviews, and transaction presentation through its enabled backend plugins.
- **Package providers:** perform package resolution, dependency handling, authorization, transaction execution, and rollback.
- **KPort:** remains a separate Pacstall/KPort package repository and package-management implementation.
- **BDFS:** supplies filesystem/image inventory, immutable artifact metadata, snapshot/export/import capabilities, and provenance where available. BDFS does not own application transactions.

Discover already has distinct backend plugins (including PackageKit, Flatpak, Snap, fwupd, rpm-ostree and optional Alpine APK support). The presence of a Discover frontend does not imply that every backend is built or enabled on a given image.

## Safe integration sequence

1. Pin the fork to an immutable commit and record its tree hash, upstream parent commit, license, and source provenance.
2. Compare the fork with current KDE upstream; identify local patches and determine whether they are maintained.
3. Build in a disposable, unprivileged CI environment against the declared Qt 6 / KDE Frameworks dependency set.
4. Test existing backend discovery and transaction UX using mock/test backends or disposable package roots.
5. Keep package-provider transactions disabled in tests that do not have an isolated package root.
6. Integrate BDFS only through documented artifact/image metadata and snapshot/export interfaces. Do not let Discover invoke BDFS filesystem mutation paths implicitly.
7. Evaluate a KPort Discover backend only after KPort publishes a stable, authenticated, machine-readable package and transaction API. Do not scrape pacscripts or execute arbitrary pacscript content from Discover.

## Current support statement

- Discover frontend candidate: **identified**
- Fork build reproducibility: **unknown**
- Upstream divergence / patch set: **not audited**
- KPort backend: **not implemented**
- BDFS artifact integration: **contract only**
- Package install/remove/update through this integration: **not enabled**

This record is an integration proposal and provenance boundary, not a declaration of tested compatibility.
