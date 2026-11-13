## Scope

Document the optional integration boundary for Nitrux Standard and Aesthetic FHS without vendoring upstream code or changing default images.

## Verified upstream facts

- `nitrux-standard` builds an `Architecture: all` metapackage from `package/dependencies`; its current dependency list includes `afhs`.
- `Architecture: all` describes the metapackage architecture, not universal availability of every dependency across Debian/Ubuntu releases or architectures.
- `aesthetic-fhs` provides the userspace `afhs` Generic Netlink controller and ABI declarations.
- The kernel-side implementation has now been located in [Nitrux/linux-kernel patch 0009-aesthetic-fhs.patch](https://github.com/Nitrux/linux-kernel/blob/linux-nitrux/patches/0009-aesthetic-fhs.patch). The upstream kernel build identifies its source version as 7.3.1 and includes this patch in `patches/series`. This is a source lead, not proof of applicability to BDFS target kernels.

## Integration contract

- Treat Nitrux Standard as an optional userspace package profile, never a required BDFS dependency.
- Resolve every dependency against the selected distro release, repository snapshot, and target architecture before image construction. Report unavailable/renamed packages rather than silently dropping them.
- Keep Aesthetic FHS disabled unless the target kernel has the reviewed implementation and exposes the compatible `aesthetic_fhs` Generic Netlink family/ABI. A config symbol alone is not proof of runtime support.
- Review the kernel patch for target-version applicability, namespace/inode lifetime safety, Generic Netlink permissions, and runtime behavior before enabling it.
- Do not vendor or redistribute `afhs` until per-file SPDX headers, repository license metadata, and upstream licensing are reconciled. The userspace repository-level BSD-3-Clause declaration coexists with GPL-2.0 identifiers in `src/afhs.c` and `include/afhs.h`.
- Image import/export tests must preserve metadata supported by the target filesystem and clearly report unsupported xattrs, ownership, ACLs, and capabilities.

## Acceptance gates

1. Pin upstream commits and record source/license provenance.
2. Validate package availability for each supported release/architecture.
3. Review and apply the kernel patch against each selected kernel base; build and boot-test it.
4. Detect kernel family/ABI at runtime and provide a clean unsupported fallback.
5. Exercise image round-trips and package install/remove/rollback in CI.
6. Keep the feature opt-in and defaults unchanged.

## Upstream

- https://github.com/Nitrux/nitrux-standard
- https://github.com/Nitrux/aesthetic-fhs
- https://github.com/Nitrux/linux-kernel/blob/linux-nitrux/patches/0009-aesthetic-fhs.patch

This is an integration specification only; it does not claim kernel patch applicability, runtime support, or passing image tests.