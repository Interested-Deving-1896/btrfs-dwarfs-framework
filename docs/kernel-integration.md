# Kernel integration status and build contract

## Current implementation

BDFS currently provides an **out-of-tree kernel module**, not an in-tree Linux
filesystem implementation that can be copied into `fs/bdfs/` and enabled with
`CONFIG_BDFS_FS`.

The module's actual Kconfig symbol is `CONFIG_BTRFS_DWARFS` (tristate). Its
Kconfig declares dependencies on `BTRFS_FS`, `FUSE_FS`, and `NET`. The
external module build is described by `kernel/Makefile` and
`kernel/btrfs_dwarfs/Kbuild`. The blend filesystem registers as
`bdfs_blend`; that is distinct from a native `bdfs` root filesystem.

Accordingly, do **not** inject the repository root into a kernel's
`fs/bdfs/`, add a made-up `CONFIG_BDFS_FS` / `CONFIG_DWARFS_FS` symbol, or
set `rootfstype=bdfs`. Those steps do not match the current source layout or
registered filesystem type.

## Build against a custom kernel

After the target kernel source/build tree has been configured (including
generated headers and its final `.config`), run:

```bash
./integrations/kernels/build-module.sh /path/to/kernel/build ./artifacts/bdfs
```

The helper:
- verifies that a configured kernel build tree is supplied;
- requires `CONFIG_BTRFS_FS`, `CONFIG_FUSE_FS`, and `CONFIG_NET` to be
  enabled as built-in or modules;
- builds the existing external module with `CONFIG_BTRFS_DWARFS=m`;
- copies the resulting `btrfs_dwarfs.ko` to the requested artifact directory;
- does not install, load, or modify the kernel source tree.

The module must still be tested against each supported kernel version and
architecture. A successful compile alone does not establish runtime correctness,
module-signing compatibility, initramfs availability, or root-filesystem support.

## Path to primary-root readiness

The kernel repositories should integrate BDFS in phases:

1. **Build compatibility:** pin a BDFS commit and compile the external module
   against each kernel build, without changing default boot behavior.
2. **Package integration:** package the module and userspace components using
   each distribution's existing packaging path; handle module signing and
   dependency metadata.
3. **Runtime validation:** load and unload on disposable test VMs, test Btrfs /
   FUSE prerequisites, exercise `bdfs_blend` mount and unmount paths, and run
   regression tests across supported kernel versions.
4. **Early-boot investigation:** validate initramfs integration and determine
   whether the current blend mount model can support the intended root layout.
   Do not change rootfstype or default boot configuration before this succeeds.
5. **Default enablement:** only after successful automated and VM boot tests,
   offer an explicit opt-in preset first; consider a default only after a
   documented rollback path and release-specific validation exist.

The three kernel build systems have different source acquisition and packaging
flows. They should call the shared build helper only at a point where their
kernel tree is configured and built/generated headers are available. A
repository-specific integration should be added only after that location is
verified for its actual build path.
